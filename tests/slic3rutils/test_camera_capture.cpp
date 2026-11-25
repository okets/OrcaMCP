#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/CameraCapture.hpp"
#include <chrono>
#include "slic3r/GUI/OrcaMCP/OrcaMCPBambuCamera.hpp"

using namespace Slic3r::GUI;
using namespace std::chrono_literals;

TEST_CASE("Camera capture stops only playback it owns", "[CameraCapture]")
{
    CameraCaptureLease lease;
    auto owned = lease.begin(false);
    REQUIRE(lease.valid(owned));
    REQUIRE(lease.finish(owned));
    REQUIRE_FALSE(lease.finish(owned));
    auto borrowed = lease.begin(true);
    REQUIRE_FALSE(lease.finish(borrowed));
    auto taken_over = lease.begin(false);
    lease.invalidate();
    REQUIRE_FALSE(lease.valid(taken_over));
    REQUIRE_FALSE(lease.finish(taken_over));
}

TEST_CASE("Camera capture admits only a newly decoded fresh frame", "[CameraCapture]")
{
    auto start = std::chrono::steady_clock::now();
    REQUIRE(camera_frame_fresh(start + 1ms, start, start + 2s));
    REQUIRE_FALSE(camera_frame_fresh(start - 1ms, start, start + 1s));
    REQUIRE_FALSE(camera_frame_fresh(start + 1ms, start, start + 2002ms));
    REQUIRE_FALSE(camera_frame_fresh(start + 1s, start, start));
    REQUIRE(camera_capture_expired(start, start + 8s));
    REQUIRE_FALSE(camera_capture_expired(start, start + 7999ms));
    REQUIRE(camera_capture_expired(start, start + 500ms, 500ms));
    REQUIRE(camera_capture_expired(start, start, 0ms));
}


TEST_CASE("Camera leases cannot stop a replacement player", "[CameraCapture]")
{
    CameraCaptureLease first;
    CameraCaptureLease replacement;
    auto old = first.begin(false);
    auto current = replacement.begin(false);
    REQUIRE_FALSE(replacement.valid(old));
    REQUIRE_FALSE(replacement.finish(old));
    REQUIRE(replacement.valid(current));
    REQUIRE(replacement.finish(current));
}

TEST_CASE("Camera metadata and image share one MCP response", "[CameraCapture]")
{
    using nlohmann::json;
    using Slic3r::GUI::OrcaMCP::camera_frame_content;
    auto response = camera_frame_content({{"status", "success"}, {"printer_id", "test-printer"},
        {"age_ms", 100}, {"image_base64", "aW1hZ2U="}});
    REQUIRE(response.at("content").size() == 2);
    auto metadata = json::parse(response.at("content").at(0).at("text").get<std::string>());
    REQUIRE(metadata.at("printer_id") == "test-printer");
    REQUIRE_FALSE(metadata.contains("image_base64"));
    REQUIRE(response.at("content").at(1).at("type") == "image");
    REQUIRE(response.at("content").at(1).at("mimeType") == "image/jpeg");
    REQUIRE(response.at("content").at(1).at("data") == "aW1hZ2U=");
    auto refused = camera_frame_content({{"status", "error"}, {"code", "camera_stale"}, {"image_base64", "aW1hZ2U="}});
    REQUIRE(refused.at("content").size() == 1);
    REQUIRE_FALSE(json::parse(refused.at("content").at(0).at("text").get<std::string>()).contains("image_base64"));
}


#include "slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.hpp"
#include "mcp_thread_test_utils.hpp"
#include <atomic>
#include <future>
#include <exception>
#include <memory>
#include <stdexcept>

TEST_CASE("A camera GUI phase times out in a held queue and never starts late", "[CameraCapture][QueuedCall]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    mcp_test::HeldMainThread queue;
    MainThreadGate gate(queue.post());
    auto started = std::make_shared<std::atomic<bool>>(false);
    const auto result = call_owned_until(gate, [started] {
        *started = true;
        return nlohmann::json{{"lease", 1}};
    }, std::chrono::steady_clock::now() + 20ms);
    REQUIRE_FALSE(result.has_value());
    queue.run_all();
    REQUIRE_FALSE(started->load());
}

TEST_CASE("A running owned GUI phase releases its caller at the deadline safely", "[CameraCapture][QueuedCall]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    mcp_test::RunningMainThread queue;
    MainThreadGate gate(queue.post());
    auto started = std::make_shared<mcp_test::Latch>();
    auto finish = std::make_shared<mcp_test::Latch>();
    auto ended = std::make_shared<mcp_test::Latch>();
    auto state = std::make_shared<int>(0);
    auto caller = std::async(std::launch::async, [&gate, started, finish, ended, state] {
        return call_owned_until(gate, [started, finish, ended, state] {
            started->open();
            finish->wait();
            *state = 42;
            ended->open();
            return nlohmann::json(*state);
        }, std::chrono::steady_clock::now() + 200ms);
    });
    const bool began = started->wait_for(mcp_test::k_bound);
    const bool returned = caller.wait_for(1s) == std::future_status::ready;
    finish->open(); // always release a faulty gate before asserting
    REQUIRE(began);
    REQUIRE(returned);
    REQUIRE_FALSE(caller.get().has_value());
    REQUIRE(ended->wait_for(mcp_test::k_bound));
    REQUIRE(*state == 42);
}

TEST_CASE("Owned camera cleanup can outlive its caller and remains idempotent", "[CameraCapture][QueuedCall]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    mcp_test::HeldMainThread queue;
    MainThreadGate gate(queue.post());
    auto lease = std::make_shared<CameraCaptureLease>();
    auto stops = std::make_shared<int>(0);
    auto errors = std::make_shared<int>(0);
    const auto token = lease->begin(false);
    auto cleanup = [lease, stops, token] { if (lease->finish(token)) ++*stops; };
    auto timed = call_owned_until(gate, [cleanup] { cleanup(); return nlohmann::json::object(); }, std::chrono::steady_clock::now() + 20ms);
    REQUIRE_FALSE(timed.has_value());
    gate.post_owned(cleanup, [errors](std::exception_ptr) { ++*errors; });
    queue.run_all();
    REQUIRE(*stops == 1);
    REQUIRE(*errors == 0);
    gate.post_owned(cleanup, [errors](std::exception_ptr) { ++*errors; });
    queue.run_all();
    REQUIRE(*stops == 1);
}

TEST_CASE("A failed asynchronous cleanup reports its error and shutdown cancels queued cleanup", "[CameraCapture][QueuedCall]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    mcp_test::HeldMainThread queue;
    MainThreadGate gate(queue.post());
    int errors = 0;
    gate.post_owned([] { throw std::runtime_error("cleanup failed"); }, [&errors](std::exception_ptr error) {
        if (error) ++errors;
    });
    queue.run_all();
    REQUIRE(errors == 1);
    bool ran = false;
    gate.post_owned([&ran] { ran = true; }, [&errors](std::exception_ptr) { ++errors; });
    gate.close();
    queue.run_all();
    REQUIRE_FALSE(ran);
    REQUIRE(errors == 1);
}


TEST_CASE("A late camera startup cannot create a session after construction stalls", "[CameraCapture][QueuedCall]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    mcp_test::RunningMainThread queue;
    MainThreadGate gate(queue.post());
    auto request = std::make_shared<CameraRequestState>();
    request->deadline = std::chrono::steady_clock::now() + 200ms;
    auto started = std::make_shared<mcp_test::Latch>();
    auto resume = std::make_shared<mcp_test::Latch>();
    auto released = std::make_shared<mcp_test::Latch>();
    auto sessions = std::make_shared<std::atomic<int>>(0);
    auto caller = std::async(std::launch::async, [&gate, request, started, resume, released, sessions] {
        return camera_phase_until(gate, request, [request, started, resume, sessions] {
            started->open();
            resume->wait(); // the real Device page's construction may stall here
            if (camera_request_expired(request)) return camera_deadline_failure();
            ++*sessions;
            return nlohmann::json{{"ready", true}};
        }, [released] { released->open(); });
    });
    const bool began = started->wait_for(mcp_test::k_bound);
    const bool returned = caller.wait_for(1s) == std::future_status::ready;
    resume->open();
    REQUIRE(began);
    REQUIRE(returned);
    REQUIRE_FALSE(caller.get().has_value());
    REQUIRE(released->wait_for(mcp_test::k_bound));
    REQUIRE(sessions->load() == 0);
}

TEST_CASE("A camera poll that completes late releases its lease and exports no image", "[CameraCapture][QueuedCall]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    mcp_test::RunningMainThread queue;
    MainThreadGate gate(queue.post());
    auto request = std::make_shared<CameraRequestState>();
    request->deadline = std::chrono::steady_clock::now() + 200ms;
    auto started = std::make_shared<mcp_test::Latch>();
    auto resume = std::make_shared<mcp_test::Latch>();
    auto released = std::make_shared<mcp_test::Latch>();
    auto caller = std::async(std::launch::async, [&gate, request, started, resume, released] {
        return camera_phase_until(gate, request, [started, resume] {
            started->open();
            resume->wait(); // native frame copying/encoding already started
            return nlohmann::json{{"status", "success"}, {"image_base64", "aW1hZ2U="}};
        }, [released] { released->open(); });
    });
    const bool began = started->wait_for(mcp_test::k_bound);
    const bool returned = caller.wait_for(1s) == std::future_status::ready;
    resume->open();
    REQUIRE(began);
    REQUIRE(returned);
    REQUIRE_FALSE(caller.get().has_value());
    REQUIRE(released->wait_for(mcp_test::k_bound));
}


TEST_CASE("A camera response includes pending cleanup and no image when its cleanup queue stalls", "[CameraCapture][QueuedCall]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    mcp_test::HeldMainThread queue;
    MainThreadGate gate(queue.post());
    auto request = std::make_shared<CameraRequestState>();
    request->deadline = std::chrono::steady_clock::now() + 20ms;
    auto lease = std::make_shared<CameraCaptureLease>();
    auto stops = std::make_shared<int>(0);
    auto errors = std::make_shared<int>(0);
    const auto token = lease->begin(false);
    auto result = finish_camera_request(gate, request, {{"status", "success"}, {"image_base64", "aW1hZ2U="}},
        [lease, stops, token] { if (lease->finish(token)) ++*stops; },
        [errors](std::exception_ptr) { ++*errors; });
    REQUIRE(result.at("code") == "camera_timeout");
    REQUIRE(result.at("cleanup_pending") == true);
    REQUIRE_FALSE(result.contains("image_base64"));
    REQUIRE(camera_frame_content(result).at("content").size() == 1);
    REQUIRE(*stops == 0);
    queue.run_all();
    REQUIRE(*stops == 1);
    REQUIRE(*errors == 0);
}

TEST_CASE("A camera response does not wait forever for cleanup that already started", "[CameraCapture][QueuedCall]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    mcp_test::RunningMainThread queue;
    MainThreadGate gate(queue.post());
    auto request = std::make_shared<CameraRequestState>();
    request->deadline = std::chrono::steady_clock::now() + 200ms;
    auto lease = std::make_shared<CameraCaptureLease>();
    const auto token = lease->begin(false);
    auto started = std::make_shared<mcp_test::Latch>();
    auto resume = std::make_shared<mcp_test::Latch>();
    auto ended = std::make_shared<mcp_test::Latch>();
    auto caller = std::async(std::launch::async, [&gate, request, lease, token, started, resume, ended] {
        return finish_camera_request(gate, request, {{"status", "success"}, {"image_base64", "aW1hZ2U="}},
            [lease, token, started, resume, ended] {
                started->open(); resume->wait();
                lease->finish(token);
                ended->open();
            }, [](std::exception_ptr) {});
    });
    const bool began = started->wait_for(mcp_test::k_bound);
    const bool returned = caller.wait_for(1s) == std::future_status::ready;
    resume->open();
    REQUIRE(began);
    REQUIRE(returned);
    const auto response = caller.get();
    REQUIRE(response.at("code") == "camera_timeout");
    REQUIRE(response.at("cleanup_pending") == true);
    REQUIRE_FALSE(response.contains("image_base64"));
    REQUIRE(ended->wait_for(mcp_test::k_bound));
}

TEST_CASE("Native shutdown owns camera teardown when the gate closes", "[CameraCapture][QueuedCall]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    mcp_test::HeldMainThread queue;
    MainThreadGate gate(queue.post());
    gate.close();
    auto request = std::make_shared<CameraRequestState>();
    request->deadline = std::chrono::steady_clock::now() + 1s;
    bool released = false;
    const auto response = finish_camera_request(gate, request, {{"status", "success"}, {"image_base64", "aW1hZ2U="}},
        [&released] { released = true; }, [](std::exception_ptr) {});
    REQUIRE(response.at("code") == "camera_unavailable");
    REQUIRE(response.at("cleanup_pending") == false);
    REQUIRE_FALSE(response.contains("image_base64"));
    REQUIRE_FALSE(released);
    REQUIRE(queue.size() == 0);
}
