#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

// -------------------------------------------------------------------------
// worker
// -------------------------------------------------------------------------
//
// One thread that runs jobs in the order they were pushed: what the
// interface hands off so that it does not wait (reading a model file,
// decoding an image). One thread rather than one per job, so that ten files
// dropped at once neither hold ten decoded images in memory together nor
// fight each other for the cores the render thread needs. A job reports
// back by posting to the interface thread (screen::post()).
//
// The thread starts with the first job. stop() (and the destructor) drops
// the jobs that have not started and waits for the one running, which has
// no way to be interrupted: a model file is read to the end.
//
// push() and stop() from one thread, the interface's.

namespace volt::ui
{

    class worker
    {
    public:
        worker() = default;
        ~worker() { stop(); }

        worker(const worker &) = delete;
        worker &operator=(const worker &) = delete;

        void push(std::function<void()> job)
        {
            {
                std::lock_guard lock(m_mutex);
                m_jobs.push_back(std::move(job));
            }
            if (!m_thread.joinable())
                m_thread = std::jthread([this](std::stop_token stop) { run(stop); });
            m_ready.notify_one();
        }

        void stop()
        {
            if (!m_thread.joinable())
                return;
            {
                std::lock_guard lock(m_mutex);
                m_jobs.clear();
            }
            m_thread.request_stop();
            m_thread.join();
            m_thread = {};
        }

    private:
        void run(std::stop_token stop)
        {
            while (true)
            {
                std::function<void()> job;
                {
                    std::unique_lock lock(m_mutex);
                    if (!m_ready.wait(lock, stop, [this] { return !m_jobs.empty(); }))
                        return; // stopped
                    job = std::move(m_jobs.front());
                    m_jobs.pop_front();
                }
                job();
            }
        }

        std::mutex m_mutex;
        std::condition_variable_any m_ready;
        std::deque<std::function<void()>> m_jobs;
        std::jthread m_thread;
    };

} // namespace volt::ui
