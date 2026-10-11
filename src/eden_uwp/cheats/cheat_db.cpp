// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/cheats/cheat_db.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>

#include "eden_uwp/cheats/cheat_catalog.h"
#include "eden_uwp/utf8.h"

namespace EdenXbox::Cheats {
namespace {
constexpr std::string_view Repository = "https://raw.githubusercontent.com/sthetix/nx-cheats-db/main/";
constexpr std::size_t BuildDigits = 16;
constexpr unsigned MaxDepth = 32;

bool IsHex(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    });
}

std::string UpperBuild(std::string_view key) {
    std::string build{key.substr(0, BuildDigits)};
    for (char& c : build) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return build;
}

/// Just enough JSON for the database: objects walked key by key, strings decoded, anything else
/// skipped. Every step checks the input, so a damaged file ends as a failure, never a crash.
class JsonReader {
public:
    explicit JsonReader(std::string_view text_) : text{text_} {}

    [[nodiscard]] bool Failed() const { return failed; }

    /// Calls `member(key)` for each member of the object here; `member` must consume the value.
    bool Object(const std::function<bool(std::string_view)>& member) {
        if (!Consume('{')) return false;
        if (Consume('}')) return true;
        std::string key;
        do {
            if (!String(key) || !Consume(':') || !member(key)) return Fail();
        } while (Consume(','));
        return Consume('}') || Fail();
    }

    bool String(std::string& out) {
        out.clear();
        if (!Consume('"')) return false;
        while (at < text.size()) {
            const char c = text[at++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= text.size()) break;
            switch (const char escape = text[at++]) {
            case '"': case '\\': case '/': out += escape; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t code = 0;
                if (!Hex4(code)) return Fail();
                // A high surrogate takes the low one that follows it.
                if (code >= 0xD800 && code < 0xDC00 && text.substr(at, 2) == "\\u") {
                    at += 2;
                    std::uint32_t low = 0;
                    if (!Hex4(low) || low < 0xDC00 || low >= 0xE000) return Fail();
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                AppendUtf8(out, static_cast<char32_t>(code));
                break;
            }
            default:
                return Fail();
            }
        }
        return Fail();
    }

    bool Skip(unsigned depth = 0) {
        if (depth > MaxDepth) return Fail();
        Space();
        if (at >= text.size()) return Fail();
        std::string ignored;
        switch (text[at]) {
        case '"':
            return String(ignored);
        case '{':
            return Object([&](std::string_view) { return Skip(depth + 1); });
        case '[':
            ++at;
            if (Consume(']')) return true;
            do {
                if (!Skip(depth + 1)) return false;
            } while (Consume(','));
            return Consume(']') || Fail();
        default:
            // A number, true, false or null: up to the next delimiter.
            while (at < text.size() && !std::strchr(",}] \t\r\n", text[at])) ++at;
            return true;
        }
    }

    bool End() {
        Space();
        return at == text.size();
    }

private:
    void Space() {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    }
    bool Consume(char c) {
        Space();
        if (at < text.size() && text[at] == c) {
            ++at;
            return true;
        }
        return false;
    }
    bool Hex4(std::uint32_t& value) {
        if (at + 4 > text.size() || !IsHex(text.substr(at, 4))) return false;
        for (int i = 0; i < 4; ++i) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[at++])));
            value = value * 16 + static_cast<std::uint32_t>(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        return true;
    }
    bool Fail() {
        failed = true;
        return false;
    }

    std::string_view text;
    std::size_t at{};
    bool failed{};
};
} // namespace

std::string CheatDbUrl(CheatCategory category, std::string_view title_id) {
    const std::string_view folder = category == CheatCategory::Graphics ? "cheats_gfx/" : "cheats/";
    return std::string{Repository} + std::string{folder} + std::string{title_id} + ".json";
}

std::optional<CheatDbTitle> ParseCheatDbTitle(std::string_view json, std::string_view build_id) {
    const std::string wanted = UpperBuild(build_id);
    CheatDbTitle title;
    std::vector<std::string> names; // of the cheats taken, to merge the 16 and 64 digit keys
    JsonReader reader{json};
    std::string value;
    const bool ok = reader.Object([&](std::string_view key) {
        if (key == "attribution") {
            return reader.Object([&](std::string_view) {
                if (!reader.String(value)) return false;
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                    value.pop_back();
                if (!value.empty() && std::find(title.credits.begin(), title.credits.end(), value) ==
                                          title.credits.end())
                    title.credits.push_back(value);
                return true;
            });
        }
        if (key.size() < BuildDigits || !IsHex(key)) return reader.Skip();
        std::string build = UpperBuild(key);
        const bool wanted_build = build == wanted;
        if (std::find(title.builds.begin(), title.builds.end(), build) == title.builds.end())
            title.builds.push_back(std::move(build));
        if (!wanted_build) return reader.Skip();
        return reader.Object([&](std::string_view name) {
            if (!reader.String(value)) return false;
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.emplace_back(name);
                if (!title.cheats.empty() && title.cheats.back() != '\n') title.cheats += '\n';
                title.cheats += value;
            }
            return true;
        });
    });
    if (!ok || reader.Failed() || !reader.End()) return std::nullopt;
    std::sort(title.builds.begin(), title.builds.end());
    return title;
}

} // namespace EdenXbox::Cheats
