// HttpServer.hpp first: it pulls in boost/asio, which on Windows must see <windows.h> before the
// libslic3r headers do (test_mcp_model_load.cpp, Build all run 36243478905).
#include "slic3r/GUI/HttpServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPLoginServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <sstream>
#include <string>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "mcp_thread_test_utils.hpp"

// The HTTP server the MCP server and the cloud login answer on: where it listens, how it stops while
// a request is still being handled or a reply is still being written, and that quitting with an MCP
// call in flight answers that call (the 2026-09-26 quit_app deadlock).
//
// Every test that blocks a handler releases it at the end, so a server that does not stop fails the
// test instead of hanging the suite.

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using namespace mcp_test;
using namespace std::chrono_literals;
using boost::asio::ip::tcp;
using Clock = std::chrono::steady_clock;

namespace {

// A port nothing listens on right now, on the loopback address.
unsigned short free_loopback_port()
{
    boost::asio::io_context io;
    tcp::acceptor           probe(io, {boost::asio::ip::address_v4::loopback(), 0});
    return probe.local_endpoint().port();
}

// One HTTP request to 127.0.0.1:`port`, on a thread of its own. The future holds everything the
// server sent before it closed the connection, or why the connection failed. With `read_after`, the
// reply is not read until that latch opens.
std::future<std::string> exchange(unsigned short port, const std::string& method, const std::string& path,
                                  const std::string& body = std::string(), Latch* read_after = nullptr)
{
    return std::async(std::launch::async, [=] {
        boost::asio::io_context   io;
        tcp::socket               socket(io);
        boost::system::error_code ec;
        socket.connect({boost::asio::ip::address_v4::loopback(), port}, ec);
        if (ec)
            return "connect failed: " + ec.message();
        const std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n" +
                                    "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        boost::asio::write(socket, boost::asio::buffer(request), ec);
        if (read_after != nullptr)
            read_after->wait();
        std::string reply;
        boost::asio::read(socket, boost::asio::dynamic_buffer(reply), ec); // to the server's close
        return reply;
    });
}

std::shared_ptr<HttpServer::Response> json_response(const nlohmann::json& body)
{
    return std::make_shared<HttpServer::ResponseJson>(body.dump());
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

TEST_CASE("the HTTP server listens on this machine only", "[HttpServer]")
{
    HttpServer server(0);
    server.set_request_handler([](const std::string&) { return json_response({{"status", "ok"}}); });
    server.start();

    const tcp::endpoint where = server.local_endpoint();
    CHECK(where.address().is_loopback());
    CHECK(where.port() != 0);
    CHECK(contains(exchange(where.port(), "GET", "/mcp").get(), "\"status\":\"ok\""));

    server.stop();
    CHECK_FALSE(server.is_started());
}

TEST_CASE("stopping the HTTP server waits for a request still being handled, and never abandons it", "[HttpServer]")
{
    // A handler still running may use what the app destroys next, so stop() must not return before it
    // does -- not even past the point where it reports a slow stop.
    Latch             entered;
    std::atomic<bool> handler_returned{false};
    HttpServer        server(0);
    server.set_request_handler([&](const std::string&) {
        entered.open();
        std::this_thread::sleep_for(std::chrono::milliseconds(HttpServer::slow_stop_warning_ms + 300));
        handler_returned = true;
        return json_response({{"status", "late"}});
    });
    server.start();

    auto reply = exchange(server.local_endpoint().port(), "POST", "/mcp", "{}");
    REQUIRE(entered.wait_for(k_bound));
    server.stop();
    const bool handler_had_returned = handler_returned;

    CHECK(handler_had_returned);
    CHECK_FALSE(server.is_started());
    CHECK(contains(reply.get(), "\"status\":\"late\""));
}

TEST_CASE("a reply still being written when the server stops reaches its client whole", "[HttpServer]")
{
    // The stop is asked for while the handler is still running, as a quit does: the handler returns
    // afterwards, and its reply is far larger than the socket's buffers, with a client slow to read.
    const std::string blob(8 << 20, 'x');
    Latch             entered, release, client_reads;
    HttpServer        server(0);
    server.set_request_handler([&](const std::string&) {
        entered.open();
        release.wait();
        return json_response({{"blob", blob}});
    });
    server.start();

    auto reply = exchange(server.local_endpoint().port(), "POST", "/mcp", "{}", &client_reads);
    REQUIRE(entered.wait_for(k_bound));
    auto stopping = std::async(std::launch::async, [&server] { server.stop(); });
    std::this_thread::sleep_for(100ms); // the stop is queued behind the handler
    release.open();
    std::this_thread::sleep_for(300ms); // the reply is part-written and the stop has run
    client_reads.open();

    const bool stopped = stopping.wait_for(k_bound) == std::future_status::ready;
    CHECK(stopped);
    const std::string received = reply.get();
    CHECK(received.size() > blob.size());
    CHECK(contains(received, "\"}"));
}

TEST_CASE("quitting with an MCP call waiting on the main thread answers the call with -32002",
          "[HttpServer][McpShutdown]")
{
    // The main thread never runs the call's work: it is the thread doing the quitting.
    HeldMainThread main_thread;
    MainThreadGate gate(main_thread.post());

    HttpServer server(0);
    server.set_request_handler([&gate](const std::string&, const std::string&, const std::string&) {
        try {
            return json_response(gate.call([] { return nlohmann::json{{"status", "ran"}}; }));
        } catch (const JsonRpcError& e) {
            return json_response({{"error", {{"code", e.code}, {"message", e.what()}}}});
        }
    });
    server.start();

    auto reply = exchange(server.local_endpoint().port(), "POST", "/mcp", R"({"jsonrpc":"2.0","id":1})");
    REQUIRE(main_thread.wait_for_tasks(1)); // the call is waiting for the main thread

    // The quit's order: close the gate (the main frame's close handler), then stop the server.
    const auto started  = Clock::now();
    auto       quitting = std::async(std::launch::async, [&] {
        gate.close();
        server.stop();
    });
    const bool quit     = quitting.wait_for(k_bound) == std::future_status::ready;
    const auto took     = Clock::now() - started;
    const bool answered = reply.wait_for(k_bound) == std::future_status::ready;
    if (!quit || !answered)
        main_thread.run_all(); // what never happens in the app: lets the test fail instead of hanging
    quitting.wait();

    CHECK(quit);
    CHECK(took < 1s);
    REQUIRE(answered);
    const std::string body = reply.get();
    INFO(body);
    CHECK(contains(body, "\"code\":-32002"));
    CHECK(contains(body, "OrcaMCP is quitting"));
}

TEST_CASE("a cloud login's callback server never stops, moves or re-routes the MCP server", "[HttpServer][Login]")
{
    const unsigned short mcp_port = free_loopback_port();
    HttpServer           mcp(mcp_port);
    LoginCallbackServer  login(mcp,
                              [](const std::string& url, const std::string& provider) {
                                  return json_response({{"login", provider}, {"url", url}});
                              },
                              "orca");
    mcp.set_request_handler([&login](const std::string&, const std::string& url, const std::string&) {
        return url.find("/mcp") != std::string::npos ? json_response({{"mcp", true}}) : login.answer(url);
    });
    mcp.start();
    auto check_mcp_untouched = [&] {
        CHECK(mcp.is_started());
        CHECK(mcp.local_endpoint().port() == mcp_port);
        CHECK(exchange(mcp_port, "GET", "/mcp").get().find("\"mcp\":true") != std::string::npos);
    };

    SECTION("on a port of its own, the login answers there")
    {
        const unsigned short first = free_loopback_port();
        login.listen(first, "bbl");
        CHECK(exchange(first, "GET", "/callback?code=1").get().find("\"login\":\"bbl\"") != std::string::npos);
        check_mcp_untouched();

        const unsigned short second = free_loopback_port();
        login.listen(second, "orca");
        CHECK(exchange(second, "GET", "/callback?code=2").get().find("\"login\":\"orca\"") != std::string::npos);
        CHECK(exchange(first, "GET", "/callback").get().find("connect failed") == 0);
        check_mcp_untouched();
    }

    SECTION("on the MCP server's port, the MCP server answers the callback for the login's provider")
    {
        login.listen(mcp_port, "bbl");
        CHECK(exchange(mcp_port, "GET", "/callback?code=3").get().find("\"login\":\"bbl\"") != std::string::npos);
        check_mcp_untouched();
    }

    login.stop();
    check_mcp_untouched();
    mcp.stop();
}
