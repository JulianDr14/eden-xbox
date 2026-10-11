// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One background thread that runs posted jobs in order, in a multithreaded apartment (WinRT's
// HttpClient blocks on it). The store's requests, the thumbnails and the downloads each have one,
// so a slow download never holds up a page of results. Destroying the queue stops it: the running
// job sees its stop token and the queued ones are dropped.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

#include <winrt/base.h>

namespace EdenXbox::Mods {

class WorkQueue {
public:
    using Job = std::function<void(std::stop_token)>;

    WorkQueue() : thread{[this](std::stop_token stop) { Run(stop); }} {}
    ~WorkQueue() {
        thread.request_stop();
        if (thread.joinable()) thread.join();
    }
    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;

    /// Runs `job` after the queued ones, or before them when `urgent`.
    void Post(Job job, bool urgent = false) {
        {
            const std::scoped_lock lock{mutex};
            if (urgent) jobs.push_front(std::move(job));
            else jobs.push_back(std::move(job));
        }
        wake.notify_one();
    }

    /// Drops the jobs that have not started.
    void Clear() {
        const std::scoped_lock lock{mutex};
        jobs.clear();
    }

    /// Nothing runs and nothing waits.
    [[nodiscard]] bool Idle() const {
        const std::scoped_lock lock{mutex};
        return jobs.empty() && !running;
    }

private:
    void Run(std::stop_token stop) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        while (true) {
            Job job;
            {
                std::unique_lock lock{mutex};
                if (!wake.wait(lock, stop, [&] { return !jobs.empty(); })) break;
                job = std::move(jobs.front());
                jobs.pop_front();
                running = true;
            }
            try {
                job(stop);
            } catch (...) {
                // A job reports its own failures; one that throws anyway must not end the queue.
            }
            const std::scoped_lock lock{mutex};
            running = false;
        }
        winrt::uninit_apartment();
    }

    mutable std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<Job> jobs;
    bool running{};
    std::jthread thread; ///< Last: it starts once everything it uses exists.
};

} // namespace EdenXbox::Mods
