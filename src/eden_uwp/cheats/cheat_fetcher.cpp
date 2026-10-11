// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/cheats/cheat_fetcher.h"

#include <chrono>
#include <map>
#include <mutex>
#include <system_error>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/logging.h"
#include "eden_uwp/atomic_file.h"
#include "eden_uwp/cheats/cheat_db.h"

namespace EdenXbox::Cheats {
namespace {
using namespace winrt::Windows::Web::Http;
using winrt::Windows::Foundation::AsyncStatus;

/// Each wait of a request; together they bound a fetch to about 20 s.
constexpr auto RequestTimeout = std::chrono::seconds{10};
/// Title files run to a few hundred KB; anything far larger is not one.
constexpr std::uint32_t MaxBytes = 8u << 20;

/// A file confirmed this recently is used as it is: pages opened again cost no request.
constexpr auto RecheckAfter = std::chrono::minutes{10};

std::mutex checked_mutex;
std::map<std::filesystem::path, std::chrono::steady_clock::time_point> checked;

bool CheckedRecently(const std::filesystem::path& file) {
    const std::scoped_lock lock{checked_mutex};
    const auto found = checked.find(file);
    return found != checked.end() && std::chrono::steady_clock::now() - found->second < RecheckAfter;
}

void MarkChecked(const std::filesystem::path& file) {
    const std::scoped_lock lock{checked_mutex};
    checked[file] = std::chrono::steady_clock::now();
}

template <typename Operation>
bool Await(const Operation& operation) {
    if (operation.wait_for(RequestTimeout) == AsyncStatus::Completed) return true;
    operation.Cancel();
    return false;
}
} // namespace

std::filesystem::path DefaultCheatCacheDirectory() {
    try {
        return std::filesystem::path{
            winrt::Windows::Storage::ApplicationData::Current().LocalCacheFolder().Path().c_str()} /
               L"cheats";
    } catch (const winrt::hresult_error&) {}
    return {};
}

FetchResult FetchCheatDb(const std::filesystem::path& cache_dir, CheatCategory category,
                         const std::string& title_id) {
    const auto dir = cache_dir / title_id;
    const std::string name = category == CheatCategory::Graphics ? "graphics" : "gameplay";
    const auto json_path = dir / (name + ".json");
    const auto etag_path = dir / (name + ".etag");
    auto cached = ReadWholeFile(json_path);
    const auto fallback = [&] {
        return cached ? FetchResult{FetchStatus::Stale, std::move(*cached)} : FetchResult{};
    };
    if (CheckedRecently(json_path)) {
        if (!cached) return {FetchStatus::Missing, {}};
        return {FetchStatus::Fresh, std::move(*cached)};
    }
    const std::string url = CheatDbUrl(category, title_id);
    try {
        // The database is cached here, with its ETag: the HTTP stack keeps no second copy.
        Filters::HttpBaseProtocolFilter filter;
        filter.CacheControl().ReadBehavior(Filters::HttpCacheReadBehavior::NoCache);
        filter.CacheControl().WriteBehavior(Filters::HttpCacheWriteBehavior::NoCache);
        HttpClient client{filter};
        HttpRequestMessage request{HttpMethod::Get(), winrt::Windows::Foundation::Uri{winrt::to_hstring(url)}};
        if (const auto etag = ReadWholeFile(etag_path); cached && etag && !etag->empty())
            request.Headers().TryAppendWithoutValidation(L"If-None-Match", winrt::to_hstring(*etag));
        const auto send = client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead);
        if (!Await(send)) {
            LOG_WARNING(Frontend, "Cheats database: {} timed out", url);
            return fallback();
        }
        const auto response = send.GetResults();
        switch (response.StatusCode()) {
        case HttpStatusCode::NotModified:
            if (!cached) return fallback();
            LOG_INFO(Frontend, "Cheats database: {} unchanged", url);
            MarkChecked(json_path);
            return {FetchStatus::Fresh, std::move(*cached)};
        case HttpStatusCode::NotFound: {
            std::error_code ec;
            std::filesystem::remove(json_path, ec);
            std::filesystem::remove(etag_path, ec);
            MarkChecked(json_path);
            return {FetchStatus::Missing, {}};
        }
        case HttpStatusCode::Ok:
            break;
        default:
            LOG_WARNING(Frontend, "Cheats database: {} answered {}", url,
                        static_cast<int>(response.StatusCode()));
            return fallback();
        }
        const auto read = response.Content().ReadAsBufferAsync();
        if (!Await(read)) return fallback();
        const auto buffer = read.GetResults();
        if (buffer.Length() > MaxBytes) return fallback();
        std::string json{reinterpret_cast<const char*>(buffer.data()), buffer.Length()};
        LOG_INFO(Frontend, "Cheats database: {} downloaded, {} bytes", url, json.size());
        WriteFileAtomically(json_path, json);
        const auto headers = response.Headers();
        if (headers.HasKey(L"ETag")) WriteFileAtomically(etag_path, winrt::to_string(headers.Lookup(L"ETag")));
        MarkChecked(json_path);
        return {FetchStatus::Fresh, std::move(json)};
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Frontend, "Cheats database: {} failed: {}", url, winrt::to_string(error.message()));
        return fallback();
    }
}

} // namespace EdenXbox::Cheats
