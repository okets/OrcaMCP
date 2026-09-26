#pragma once

// Thread doubles for the tests of how MCP calls wait on the main thread and how a quit releases them
// (test_mcp_shutdown.cpp, test_http_server.cpp, test_thread_cancel.cpp).

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace mcp_test {

// Far longer than any correct release takes, and short enough that a broken one fails the test
// instead of stalling the suite.
constexpr std::chrono::seconds k_bound{2};

// Shut until opened, from any thread.
class Latch
{
public:
    void open()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_open = true;
        }
        m_changed.notify_all();
    }
    void wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [&] { return m_open; });
    }
    bool wait_for(std::chrono::milliseconds bound)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, bound, [&] { return m_open; });
    }

private:
    std::mutex              m_mutex;
    std::condition_variable m_changed;
    bool                    m_open = false;
};

// A main thread that runs nothing until told to: what the real one looks like while it is blocked
// joining the HTTP thread.
class HeldMainThread
{
public:
    std::function<void(std::function<void()>)> post()
    {
        return [this](std::function<void()> task) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tasks.push_back(std::move(task));
            m_changed.notify_all();
        };
    }

    bool wait_for_tasks(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, k_bound, [&] { return m_tasks.size() >= count; });
    }

    std::size_t size()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tasks.size();
    }

    // Runs, on the calling thread, every task queued so far.
    void run_all()
    {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            tasks.swap(m_tasks);
        }
        for (auto& task : tasks)
            task();
    }

private:
    std::mutex                        m_mutex;
    std::condition_variable           m_changed;
    std::deque<std::function<void()>> m_tasks;
};

// A main thread that runs every task as it arrives, on a thread of its own.
class RunningMainThread
{
public:
    RunningMainThread() : m_thread([this] { loop(); }) {}
    ~RunningMainThread()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_changed.notify_all();
        m_thread.join();
    }

    std::function<void(std::function<void()>)> post()
    {
        return [this](std::function<void()> task) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tasks.push_back(std::move(task));
            m_changed.notify_all();
        };
    }

private:
    void loop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (true) {
            m_changed.wait(lock, [&] { return m_stopping || !m_tasks.empty(); });
            if (m_tasks.empty())
                return;
            auto task = std::move(m_tasks.front());
            m_tasks.pop_front();
            lock.unlock();
            task();
            lock.lock();
        }
    }

    std::mutex                        m_mutex;
    std::condition_variable           m_changed;
    std::deque<std::function<void()>> m_tasks;
    bool                              m_stopping = false;
    std::thread                       m_thread;
};

} // namespace mcp_test
