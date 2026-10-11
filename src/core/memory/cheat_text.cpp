// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <unordered_map>
#include "core/memory/cheat_text.h"

namespace Core::Memory {
namespace {
constexpr std::string_view ByteOrderMark = "\xEF\xBB\xBF";
constexpr std::size_t NoEntry = std::numeric_limits<std::size_t>::max();

bool IsSpace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool IsHexWord(std::string_view token) {
    return token.size() == 8 && std::all_of(token.begin(), token.end(), [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c)) != 0;
           });
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// Copies `name` into the fixed field, keeping the terminator and never cutting a UTF-8 sequence.
void SetName(CheatDefinition& definition, std::string_view name) {
    std::size_t size = std::min(name.size(), definition.readable_name.size() - 1);
    if (size < name.size()) {
        while (size > 0 && (static_cast<unsigned char>(name[size]) & 0xC0) == 0x80) {
            --size;
        }
    }
    std::memcpy(definition.readable_name.data(), name.data(), size);
}

u32 ParseWord(std::string_view token) {
    u32 value = 0;
    for (const char c : token) {
        const auto digit = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
        value = (value << 4) | (digit <= '9' ? digit - '0' : digit - 'a' + 10);
    }
    return value;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}
} // Anonymous namespace

CheatParser::~CheatParser() = default;

TextCheatParser::~TextCheatParser() = default;

std::vector<CheatEntry> TextCheatParser::Parse(std::string_view data) const {
    std::vector<CheatEntry> out(1);
    std::vector<bool> dropped(1, false);
    std::size_t current = NoEntry;
    bool master_seen = false;

    const auto open_entry = [&](bool drop) {
        out.emplace_back();
        dropped.push_back(drop);
        return out.size() - 1;
    };

    std::size_t i = 0;
    while (i < data.size()) {
        const char c = data[i];
        if (IsSpace(c)) {
            ++i;
        } else if (data.substr(i).starts_with(ByteOrderMark)) {
            i += ByteOrderMark.size();
        } else if (c == '#' || data.substr(i).starts_with("//")) {
            i = std::min(data.find('\n', i), data.size());
        } else if (c == '[' || c == '{') {
            const bool master = c == '{';
            const std::size_t close = data.find(master ? '}' : ']', i + 1);
            if (close == std::string_view::npos) {
                // An unterminated header: nothing after it can be told apart.
                break;
            }
            const std::string_view name = data.substr(i + 1, close - i - 1);
            if (master) {
                // A file holds one master cheat; a second one is left out with its opcodes.
                current = master_seen ? open_entry(true) : 0;
                master_seen = true;
            } else {
                current = open_entry(false);
            }
            if (name.empty()) {
                dropped[current] = true;
            }
            SetName(out[current].definition, name);
            i = close + 1;
        } else {
            std::size_t end = i;
            while (end < data.size() && !IsSpace(data[end])) {
                ++end;
            }
            const std::string_view token = data.substr(i, end - i);
            // Text before the first header (a title line, for instance) is not part of a cheat.
            if (current != NoEntry) {
                CheatDefinition& definition = out[current].definition;
                if (IsHexWord(token) && definition.num_opcodes < definition.opcodes.size()) {
                    definition.opcodes[definition.num_opcodes++] = ParseWord(token);
                } else {
                    dropped[current] = true;
                }
            }
            i = end;
        }
    }

    if (dropped[0]) {
        out[0] = {};
    }
    std::size_t kept = 1;
    for (std::size_t index = 1; index < out.size(); ++index) {
        // A header with no opcodes is only a label for the cheats below it.
        if (!dropped[index] && out[index].definition.num_opcodes > 0) {
            out[kept++] = out[index];
        }
    }
    out.resize(kept);

    for (u32 index = 0; index < out.size(); ++index) {
        out[index].enabled = out[index].definition.num_opcodes > 0;
        out[index].cheat_id = index;
    }
    return out;
}

std::string_view CheatName(const CheatEntry& entry) {
    const auto& name = entry.definition.readable_name;
    return {name.data(), ::strnlen(name.data(), name.size())};
}

std::size_t EnabledOpcodeCount(std::span<const CheatEntry> cheats) {
    std::size_t count = 0;
    for (const CheatEntry& entry : cheats) {
        if (entry.enabled) {
            count += entry.definition.num_opcodes;
        }
    }
    return count;
}

std::vector<CheatToggle> ParseCheatToggles(std::string_view text) {
    std::vector<CheatToggle> toggles;
    std::string_view pending;
    bool has_pending = false;
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = Trim(text.substr(0, end));
        text.remove_prefix(std::min(end + 1, text.size()));
        if (line.starts_with(ByteOrderMark)) {
            line = Trim(line.substr(ByteOrderMark.size()));
        }
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            pending = line.substr(1, line.size() - 2);
            has_pending = true;
            continue;
        }
        if (!has_pending) {
            continue;
        }
        if (EqualsNoCase(line, "true") || EqualsNoCase(line, "on") || line == "1") {
            toggles.push_back({std::string(pending), true});
        } else if (EqualsNoCase(line, "false") || EqualsNoCase(line, "off") || line == "0") {
            toggles.push_back({std::string(pending), false});
        }
        has_pending = false;
    }
    return toggles;
}

void SyncMasterCheat(std::span<CheatEntry> cheats) {
    if (cheats.empty()) {
        return;
    }
    const bool any = std::any_of(cheats.begin() + 1, cheats.end(),
                                 [](const CheatEntry& entry) { return entry.enabled; });
    cheats[0].enabled = any && cheats[0].definition.num_opcodes > 0;
}

void ApplyCheatToggles(std::span<CheatEntry> cheats, std::span<const CheatToggle> toggles) {
    // The n-th toggle of a name sets the n-th cheat of that name, so files that repeat a name
    // round-trip through FormatCheatToggles.
    std::unordered_map<std::string_view, std::size_t> next_index;
    for (const CheatToggle& toggle : toggles) {
        std::size_t& from = next_index.try_emplace(toggle.name, 1).first->second;
        for (std::size_t index = from; index < cheats.size(); ++index) {
            if (CheatName(cheats[index]) == toggle.name) {
                cheats[index].enabled =
                    toggle.enabled && cheats[index].definition.num_opcodes > 0;
                from = index + 1;
                break;
            }
        }
    }
    SyncMasterCheat(cheats);
}

std::string FormatCheatToggles(std::span<const CheatEntry> cheats) {
    std::string text;
    for (std::size_t index = 1; index < cheats.size(); ++index) {
        text += '[';
        text += CheatName(cheats[index]);
        text += cheats[index].enabled ? "]\ntrue\n" : "]\nfalse\n";
    }
    return text;
}

} // namespace Core::Memory
