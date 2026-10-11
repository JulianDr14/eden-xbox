// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mod_web.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#include <openssl/evp.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include "common/logging.h"
#include "eden_uwp/atomic_file.h"

namespace EdenXbox::Mods::Web {
namespace {
namespace fs = std::filesystem;
using namespace winrt::Windows::Web::Http;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Storage::Streams::Buffer;
using winrt::Windows::Storage::Streams::InputStreamOptions;

/// How long one step (the answer's headers, a piece of the body) may take before it counts as a
/// dropped connection. A download that stalls this long resumes on its next try.
constexpr auto Patience = std::chrono::seconds{20};

/// Waits for `operation`; false when it failed, the stop token fired or it took too long.
/// An operation takes one Completed handler only (wait_for sets one too, so it cannot be called
/// again): the handler is set once and the wait polls it for the stop token.
template <typename Operation>
bool Await(const Operation& operation, std::stop_token stop) {
    struct Signal {
        std::mutex mutex;
        std::condition_variable done_cv;
        bool done{};
    };
    const auto signal = std::make_shared<Signal>();
    operation.Completed([signal](auto&&, AsyncStatus) {
        {
            const std::scoped_lock lock{signal->mutex};
            signal->done = true;
        }
        signal->done_cv.notify_all();
    });
    const auto until = std::chrono::steady_clock::now() + Patience;
    std::unique_lock lock{signal->mutex};
    while (!signal->done) {
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= until) {
            lock.unlock();
            operation.Cancel();
            return false;
        }
        signal->done_cv.wait_for(lock, std::chrono::milliseconds{100});
    }
    return operation.Status() == AsyncStatus::Completed;
}

/// A client per call: they are cheap, and each lives and dies in the worker's apartment. The HTTP
/// stack keeps no cache of its own; what is worth keeping is kept here.
HttpClient MakeClient() {
    Filters::HttpBaseProtocolFilter filter;
    filter.CacheControl().ReadBehavior(Filters::HttpCacheReadBehavior::NoCache);
    filter.CacheControl().WriteBehavior(Filters::HttpCacheWriteBehavior::NoCache);
    HttpClient client{filter};
    client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"eden-xbox-mods/1.0");
    return client;
}

std::string Text(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

std::optional<std::string> Body(const HttpResponseMessage& response, std::size_t max_bytes,
                                std::stop_token stop) {
    if (const auto length = response.Content().Headers().ContentLength(); length && length.Value() > max_bytes)
        return std::nullopt;
    const auto read = response.Content().ReadAsBufferAsync();
    if (!Await(read, stop)) return std::nullopt;
    const auto buffer = read.GetResults();
    if (buffer.Length() > max_bytes) return std::nullopt;
    return std::string{reinterpret_cast<const char*>(buffer.data()), buffer.Length()};
}

std::mutex checked_mutex;
std::map<fs::path, std::chrono::steady_clock::time_point> checked;

class Md5 {
public:
    Md5() : context{EVP_MD_CTX_new(), EVP_MD_CTX_free} {
        Reset();
    }
    void Reset() {
        if (context) EVP_DigestInit_ex(context.get(), EVP_md5(), nullptr);
    }
    void Add(const void* data, std::size_t size) {
        if (context) EVP_DigestUpdate(context.get(), data, size);
    }
    std::string Hex() {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        if (!context || EVP_DigestFinal_ex(context.get(), digest, &size) != 1) return {};
        static constexpr char hex[] = "0123456789abcdef";
        std::string out;
        for (unsigned int i = 0; i < size; ++i) {
            out += hex[digest[i] >> 4];
            out += hex[digest[i] & 0xF];
        }
        return out;
    }

private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context;
};

/// Hashes what a resumed download already holds; false when stopped or unreadable.
bool HashFile(const fs::path& path, Md5& md5, std::stop_token stop) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return false;
    std::vector<char> chunk(1u << 20);
    while (in) {
        if (stop.stop_requested()) return false;
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        md5.Add(chunk.data(), static_cast<std::size_t>(in.gcount()));
    }
    return in.eof();
}

} // namespace

fs::path CacheFolder() {
    try {
        return fs::path{winrt::Windows::Storage::ApplicationData::Current().LocalCacheFolder().Path().c_str()} /
               L"mods";
    } catch (const winrt::hresult_error&) {}
    return {};
}

std::optional<std::string> Get(const std::string& url, std::stop_token stop, std::size_t max_bytes) {
    try {
        auto client = MakeClient();
        const auto send = client.GetAsync(winrt::Windows::Foundation::Uri{winrt::to_hstring(url)},
                                          HttpCompletionOption::ResponseHeadersRead);
        if (!Await(send, stop)) {
            if (!stop.stop_requested()) LOG_WARNING(Frontend, "Mods: {} timed out", url);
            return std::nullopt;
        }
        const auto response = send.GetResults();
        if (response.StatusCode() != HttpStatusCode::Ok) {
            LOG_WARNING(Frontend, "Mods: {} answered {}", url, static_cast<int>(response.StatusCode()));
            return std::nullopt;
        }
        return Body(response, max_bytes, stop);
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Frontend, "Mods: {} failed: {}", url, winrt::to_string(error.message()));
        return std::nullopt;
    }
}

std::optional<std::string> GetCached(const std::string& url, const fs::path& file, int recheck_minutes,
                                     std::stop_token stop) {
    auto etag_path = file;
    etag_path += ".etag";
    auto cached = ReadWholeFile(file);
    {
        const std::scoped_lock lock{checked_mutex};
        const auto found = checked.find(file);
        if (cached && found != checked.end() &&
            std::chrono::steady_clock::now() - found->second < std::chrono::minutes{recheck_minutes})
            return cached;
    }
    const auto mark = [&] {
        const std::scoped_lock lock{checked_mutex};
        checked[file] = std::chrono::steady_clock::now();
    };
    try {
        auto client = MakeClient();
        HttpRequestMessage request{HttpMethod::Get(), winrt::Windows::Foundation::Uri{winrt::to_hstring(url)}};
        if (const auto etag = ReadWholeFile(etag_path); cached && etag && !etag->empty())
            request.Headers().TryAppendWithoutValidation(L"If-None-Match", winrt::to_hstring(*etag));
        const auto send = client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead);
        if (!Await(send, stop)) return cached;
        const auto response = send.GetResults();
        if (response.StatusCode() == HttpStatusCode::NotModified && cached) {
            mark();
            return cached;
        }
        if (response.StatusCode() != HttpStatusCode::Ok) {
            LOG_WARNING(Frontend, "Mods: {} answered {}", url, static_cast<int>(response.StatusCode()));
            return cached;
        }
        auto body = Body(response, 16u << 20, stop);
        if (!body) return cached;
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        WriteFileAtomically(file, *body);
        if (response.Headers().HasKey(L"ETag"))
            WriteFileAtomically(etag_path, winrt::to_string(response.Headers().Lookup(L"ETag")));
        mark();
        return body;
    } catch (const winrt::hresult_error& error) {
        LOG_WARNING(Frontend, "Mods: {} failed: {}", url, winrt::to_string(error.message()));
        return cached;
    }
}

DownloadError Download(const std::string& url, const fs::path& part, const std::string& md5,
                       std::uint64_t max_bytes, std::stop_token stop, const DownloadProgress& progress,
                       std::string& detail) {
    std::error_code ec;
    fs::create_directories(part.parent_path(), ec);
    const auto stopped = [&] { return stop.stop_requested() ? DownloadError::Cancelled : DownloadError::Network; };
    try {
        std::uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
        if (ec) have = 0;
        Md5 hash;
        if (have > 0 && !md5.empty() && !HashFile(part, hash, stop)) {
            if (stop.stop_requested()) return DownloadError::Cancelled;
            fs::remove(part, ec);
            have = 0;
            hash.Reset();
        }
        auto client = MakeClient();
        HttpRequestMessage request{HttpMethod::Get(), winrt::Windows::Foundation::Uri{winrt::to_hstring(url)}};
        if (have > 0)
            request.Headers().TryAppendWithoutValidation(L"Range", L"bytes=" + std::to_wstring(have) + L"-");
        const auto send = client.SendRequestAsync(request, HttpCompletionOption::ResponseHeadersRead);
        if (!Await(send, stop)) {
            detail = "no answer";
            return stopped();
        }
        const auto response = send.GetResults();
        const auto status = response.StatusCode();
        // A server that ignores Range sends the whole file again: start over.
        bool append = status == HttpStatusCode::PartialContent;
        if (status == HttpStatusCode::RequestedRangeNotSatisfiable && have > 0) {
            // Nothing past what is here: the file was complete when the page closed.
        } else if (status != HttpStatusCode::Ok && !append) {
            detail = "HTTP " + std::to_string(static_cast<int>(status));
            return DownloadError::Network;
        } else {
            if (!append) {
                have = 0;
                hash.Reset();
            }
            std::uint64_t total = 0;
            if (const auto length = response.Content().Headers().ContentLength()) total = have + length.Value();
            if (total > max_bytes) {
                detail = std::to_string(total) + " bytes";
                fs::remove(part, ec);
                return DownloadError::TooLarge;
            }
            std::ofstream out{part, std::ios::binary | (append ? std::ios::app : std::ios::trunc)};
            if (!out) {
                detail = "cannot write " + Text(part);
                return DownloadError::Write;
            }
            const auto open = response.Content().ReadAsInputStreamAsync();
            if (!Await(open, stop)) return stopped();
            const auto stream = open.GetResults();
            Buffer buffer{1u << 18};
            while (true) {
                const auto read = stream.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial);
                if (!Await(read, stop)) {
                    detail = "connection lost at " + std::to_string(have);
                    return stopped(); // What arrived is kept for the next try.
                }
                const auto chunk = read.GetResults();
                if (chunk.Length() == 0) break;
                out.write(reinterpret_cast<const char*>(chunk.data()), chunk.Length());
                if (!out) {
                    detail = "cannot write " + Text(part);
                    return DownloadError::Write;
                }
                hash.Add(chunk.data(), chunk.Length());
                have += chunk.Length();
                if (have > max_bytes) {
                    out.close();
                    fs::remove(part, ec);
                    return DownloadError::TooLarge;
                }
                if (progress) progress(have, std::max(total, have));
            }
            out.close();
            if (total > 0 && have < total) {
                detail = "ended at " + std::to_string(have) + " of " + std::to_string(total);
                return DownloadError::Network;
            }
        }
        if (!md5.empty()) {
            const auto got = hash.Hex();
            if (got != md5) {
                detail = "md5 " + got + " instead of " + md5;
                fs::remove(part, ec);
                return DownloadError::Checksum;
            }
        }
        return DownloadError::None;
    } catch (const winrt::hresult_error& error) {
        detail = winrt::to_string(error.message());
        return stopped();
    }
}

} // namespace EdenXbox::Mods::Web
