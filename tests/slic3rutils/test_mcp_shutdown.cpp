#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include "slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.hpp"

// How an MCP call waits for the main thread, and how quitting releases it. On 2026-09-26 quit_app,
// with a script polling get_slicing_status, hung the app for 15 minutes: the main thread was joining
// the HTTP thread while the HTTP thread waited for the main thread to run its work.
//
// Every test here that expects a waiter to be released also has a way to release it by force (run
// the queued work, open the latch), so a gate that fails to release it fails the test instead of
// hanging the suite.

using namespace Slic3r::GUI::OrcaMCP;
using namespace std::chrono_literals;

namespace {

constexpr auto k_bound = 2s; // far longer than any correct release takes

// A main thread that runs nothing until told to: what the real one looks like while it is blocked
// joining the HTTP thread.
class HeldMainThread
{
public:
    MainThreadGate::Post post()
    {
        return [this](std::function<void()> task) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tasks.push_back(std::move(task));
            m_changed.notify_all();
        };
    }

    bool wait_for_tasks(size_t count)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, k_bound, [&] { return m_tasks.size() >= count; });
    }

    size_t size()
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

    MainThreadGate::Post post()
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

// A gate.call made the way the HTTP thread makes it: on a thread of its own. Its outcome reads
// "value <json>", "shutting down" or "error <what>".
class BackgroundCall
{
public:
    BackgroundCall(MainThreadGate& gate, MainThreadGate::Work work)
        : m_future(m_outcome.get_future()), m_thread([this, &gate, work] { m_outcome.set_value(outcome_of(gate, work)); })
    {}
    ~BackgroundCall() { m_thread.join(); }

    bool ended_within(std::chrono::milliseconds bound) { return m_future.wait_for(bound) == std::future_status::ready; }
    std::string outcome() { return m_future.get(); }

private:
    static std::string outcome_of(MainThreadGate& gate, const MainThreadGate::Work& work)
    {
        try {
            return "value " + gate.call(work).dump();
        } catch (const McpShuttingDown&) {
            return "shutting down";
        } catch (const std::exception& e) {
            return std::string("error ") + e.what();
        }
    }

    std::promise<std::string> m_outcome;
    std::future<std::string>  m_future;
    std::thread               m_thread;
};

// A gate that can be held shut and opened from another thread.
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

private:
    std::mutex              m_mutex;
    std::condition_variable m_changed;
    bool                    m_open = false;
};

} // namespace

TEST_CASE("a call waiting on a main thread that never runs its work is released when the gate closes",
          "[McpShutdown][orcamcp]")
{
    HeldMainThread main_thread;
    MainThreadGate gate(main_thread.post());
    std::atomic<bool> ran{false};

    BackgroundCall call(gate, [&] { ran = true; return nlohmann::json("ran"); });
    REQUIRE(main_thread.wait_for_tasks(1));

    gate.close();
    const bool released = call.ended_within(k_bound);
    if (!released)
        main_thread.run_all(); // a gate that did not release it: end the call so the test can fail

    CHECK(released);
    CHECK(call.outcome() == "shutting down");
    CHECK_FALSE(ran);
}

TEST_CASE("a closed gate refuses a call at once and queues nothing", "[McpShutdown][orcamcp]")
{
    HeldMainThread main_thread;
    MainThreadGate gate(main_thread.post());
    gate.close();

    CHECK(gate.is_closed());
    CHECK_THROWS_AS(gate.call([] { return nlohmann::json("ran"); }), McpShuttingDown);
    CHECK(main_thread.size() == 0);

    gate.close(); // closing twice is harmless
    CHECK(gate.is_closed());
}

TEST_CASE("work its caller was released from is not run when the main thread reaches it", "[McpShutdown][orcamcp]")
{
    HeldMainThread main_thread;
    MainThreadGate gate(main_thread.post());
    std::atomic<bool> ran{false};
    {
        BackgroundCall call(gate, [&] { ran = true; return nlohmann::json("ran"); });
        REQUIRE(main_thread.wait_for_tasks(1));
        gate.close();
        if (!call.ended_within(k_bound))
            main_thread.run_all();
        CHECK(call.outcome() == "shutting down");
    }

    // The queued task outlives the caller it served, and must not touch what that caller owned.
    main_thread.run_all();
    CHECK_FALSE(ran);
}

TEST_CASE("work that has started runs to its end, and its caller waits for it, even if the gate closes",
          "[McpShutdown][orcamcp]")
{
    // The work may be using what its caller owns, so the caller must not return while it runs.
    RunningMainThread main_thread;
    MainThreadGate    gate(main_thread.post());
    Latch             started, finish;

    BackgroundCall call(gate, [&] {
        started.open();
        finish.wait();
        return nlohmann::json("finished");
    });
    started.wait();

    gate.close();
    const bool returned_early = call.ended_within(100ms);
    finish.open();

    CHECK_FALSE(returned_early);
    REQUIRE(call.ended_within(k_bound));
    CHECK(call.outcome() == "value \"finished\"");
}

TEST_CASE("a call returns what its work returned, and rethrows what it threw", "[McpShutdown][orcamcp]")
{
    RunningMainThread main_thread;
    MainThreadGate    gate(main_thread.post());

    CHECK(gate.call([] { return nlohmann::json{{"status", "success"}}; }) == nlohmann::json{{"status", "success"}});
    try {
        gate.call([]() -> nlohmann::json { throw std::runtime_error("no plater"); });
        FAIL("the work's exception was lost");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) == "no plater");
    }
}

TEST_CASE("work that reaches the main thread while the app is quitting is not run", "[McpShutdown][orcamcp]")
{
    // The main frame's close handler tears the GUI down before the gate is closed; work queued then
    // must not run against it.
    RunningMainThread main_thread;
    std::atomic<bool> quitting{false};
    MainThreadGate    gate(main_thread.post(), [&] { return quitting.load(); });
    std::atomic<bool> ran{false};

    quitting = true;
    CHECK_THROWS_AS(gate.call([&] { ran = true; return nlohmann::json("ran"); }), McpShuttingDown);
    CHECK_FALSE(ran);
}
