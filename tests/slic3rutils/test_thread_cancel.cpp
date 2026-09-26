// HttpServer.hpp first: it pulls in boost/asio, which on Windows must see <windows.h> before the
// libslic3r headers do.
#include "slic3r/GUI/HttpServer.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/ThreadCancel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "mcp_thread_test_utils.hpp"

// The cancellation a quit applies to the blocking calls the HTTP server's thread makes for a request:
// a sign-in exchange, a Flashforge request, printer discovery. The app joins that thread when it
// quits, so such a call must give up rather than hold the quit, and must report that it gave up.

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace mcp_test;
using namespace std::chrono_literals;

namespace {

// What one synchronous Http GET on this thread ended with: "complete <body>" or "error <error>".
std::string get_on_this_thread(const std::string& url)
{
    std::string outcome = "neither callback ran";
    Http::get(url)
        .on_complete([&](std::string body, unsigned) { outcome = "complete " + body; })
        .on_error([&](std::string, std::string error, unsigned) { outcome = "error " + error; })
        .perform_sync();
    return outcome;
}

} // namespace

TEST_CASE("a thread's cancel check holds only in its scope, and restores the one it replaced", "[ThreadCancel]")
{
    CHECK_FALSE(this_thread_cancelled());
    {
        const ScopedThreadCancelCheck outer([] { return false; });
        CHECK_FALSE(this_thread_cancelled());
        {
            const ScopedThreadCancelCheck inner([] { return true; });
            CHECK(this_thread_cancelled());

            // Another thread has no check in scope.
            const bool elsewhere = std::async(std::launch::async, [] { return this_thread_cancelled(); }).get();
            CHECK_FALSE(elsewhere);
        }
        CHECK_FALSE(this_thread_cancelled());
    }
    CHECK_FALSE(this_thread_cancelled());
}

TEST_CASE("a blocking HTTP request gives up once its thread is cancelled, and reports it as an error", "[ThreadCancel]")
{
    // A server that does not answer until the test lets it: what a busy printer or cloud looks like.
    Latch      release;
    HttpServer server(0);
    server.set_request_handler([&release](const std::string&) {
        release.wait();
        return std::make_shared<HttpServer::ResponseJson>(R"({"late":true})");
    });
    server.start();
    const std::string url = "http://127.0.0.1:" + std::to_string(server.local_endpoint().port()) + "/status";

    std::atomic<bool> quitting{false};
    auto              request = std::async(std::launch::async, [&] {
        const ScopedThreadCancelCheck check([&] { return quitting.load(); });
        return get_on_this_thread(url);
    });
    std::this_thread::sleep_for(200ms); // connected, and waiting for the answer
    quitting = true;
    const bool gave_up = request.wait_for(3s) == std::future_status::ready;
    release.open(); // lets a request that did not give up end, so the test fails instead of hanging

    CHECK(gave_up);
    CHECK(request.get() == "error Request cancelled");
    server.stop();
}

TEST_CASE("an HTTP request whose thread is not cancelled completes as before", "[ThreadCancel]")
{
    HttpServer server(0);
    server.set_request_handler([](const std::string&) { return std::make_shared<HttpServer::ResponseJson>(R"({"ok":true})"); });
    server.start();
    const std::string url = "http://127.0.0.1:" + std::to_string(server.local_endpoint().port()) + "/status";

    const ScopedThreadCancelCheck check([] { return false; });
    CHECK(get_on_this_thread(url) == R"(complete {"ok":true})");
    server.stop();
}
