#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.hpp"
#include "mcp_thread_test_utils.hpp"

// How an MCP call waits for the main thread, and how quitting releases it. On 2026-09-26 quit_app,
// with a script polling get_slicing_status, hung the app for 15 minutes: the main thread was joining
// the HTTP thread while the HTTP thread waited for the main thread to run its work.
//
// Every test here that expects a waiter to be released also has a way to release it by force (run
// the queued work, open the latch), so a gate that fails to release it fails the test instead of
// hanging the suite.

using namespace Slic3r::GUI::OrcaMCP;
using namespace mcp_test;
using namespace std::chrono_literals;

namespace {

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
    // The main frame's close handler closes the gate before it tears the GUI down, so work still
    // queued then must not run against the half-closed GUI.
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

TEST_CASE("a close that arrives inside a call's work waits until the work has returned", "[McpShutdown][orcamcp]")
{
    // The main frame's close handler defers itself this way when the work pumped the event loop into
    // it, so the plater is never reset and the frame never torn down under the running work.
    RunningMainThread main_thread;
    MainThreadGate    gate(main_thread.post());
    Latch             started, finish, closed;
    std::atomic<bool> work_returned{false};
    std::atomic<bool> closed_after_work{false};

    BackgroundCall call(gate, [&] {
        started.open();
        finish.wait();
        work_returned = true;
        return nlohmann::json("finished");
    });
    started.wait();

    const bool deferred = gate.defer_until_work_ends([&] {
        closed_after_work = work_returned.load();
        closed.open();
    });
    const bool closed_early = closed.wait_for(100ms);
    finish.open();

    CHECK(deferred);
    CHECK_FALSE(closed_early);
    REQUIRE(closed.wait_for(k_bound));
    CHECK(closed_after_work);
    REQUIRE(call.ended_within(k_bound));
}

TEST_CASE("a close with no call's work running is not deferred", "[McpShutdown][orcamcp]")
{
    RunningMainThread main_thread;
    MainThreadGate    gate(main_thread.post());
    std::atomic<bool> ran{false};

    CHECK_FALSE(gate.defer_until_work_ends([&] { ran = true; }));
    CHECK(gate.call([] { return nlohmann::json("done"); }) == "done"); // a call that ran and returned
    CHECK_FALSE(gate.defer_until_work_ends([&] { ran = true; }));
    CHECK_FALSE(ran); // the caller closes at once instead
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

TEST_CASE("a call refused because the app is quitting is a JSON-RPC error with its own code", "[McpShutdown][orcamcp]")
{
    // handle_request answers every JsonRpcError the same way, with its code: -32002, not -32603.
    try {
        throw McpShuttingDown();
    } catch (const JsonRpcError& e) {
        CHECK(e.code == -32002);
        CHECK(std::string(e.what()).find("OrcaMCP is quitting") == 0);
    }
}
