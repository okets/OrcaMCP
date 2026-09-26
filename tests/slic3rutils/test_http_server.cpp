// HttpServer.hpp first: it pulls in boost/asio, which on Windows must see <windows.h> before the
// libslic3r headers do (test_mcp_model_load.cpp, Build all run 36243478905).
#include "slic3r/GUI/HttpServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPLoginServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRequestGuard.hpp"

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
// reply is not read until that latch opens. `headers` are sent as they are, one "Name: value\r\n"
// each; left empty, the request names localhost:`port`, as the bridge's and curl's do.
std::future<std::string> exchange(unsigned short port, const std::string& method, const std::string& path,
                                  const std::string& body = std::string(), Latch* read_after = nullptr,
                                  const std::string& headers = std::string())
{
    return std::async(std::launch::async, [=] {
        boost::asio::io_context   io;
        tcp::socket               socket(io);
        boost::system::error_code ec;
        socket.connect({boost::asio::ip::address_v4::loopback(), port}, ec);
        if (ec)
            return "connect failed: " + ec.message();
        const std::string head    = headers.empty() ? "Host: localhost:" + std::to_string(port) + "\r\n" : headers;
        const std::string request = method + " " + path + " HTTP/1.1\r\n" + head + "Content-Type: application/json\r\n" +
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

// A sign-in handler that records what it was asked, and answers with its provider.
struct FakeSignIn
{
    std::atomic<int> calls{0};
    LoginCallbackServer::AuthHandler handler()
    {
        return [this](const std::string& url, const std::string& provider) {
            ++calls;
            return json_response({{"login", provider}, {"url", url}});
        };
    }
};

// GUI_App::start_http_server's routes: /mcp is MCP, anything else a login callback.
HttpServer::RequestHandlerFn app_routes(LoginCallbackServer& login, std::function<std::shared_ptr<HttpServer::Response>()> mcp)
{
    return [&login, mcp](const std::string&, const std::string& url, const std::string&) {
        return contains(url, "/mcp") ? mcp() : login.answer(url);
    };
}

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

TEST_CASE("a cloud login's callbacks are served one at a time with MCP calls", "[HttpServer][Login]")
{
    // They share the MCP server's thread, as they did when both shared one port: a sign-in never runs
    // while an MCP call is in progress.
    HeldMainThread quit_queue;
    MainThreadGate quit_gate(quit_queue.post());
    FakeSignIn     sign_in;
    Latch          mcp_entered, mcp_release;

    const unsigned short mcp_port = free_loopback_port();
    HttpServer           mcp(mcp_port);
    LoginCallbackServer  login(mcp, sign_in.handler(), "orca", quit_gate);
    mcp.set_request_handler(app_routes(login, [&] {
        mcp_entered.open();
        mcp_release.wait();
        return json_response({{"mcp", true}});
    }));
    mcp.start();
    const unsigned short login_port = free_loopback_port();
    REQUIRE(login.listen(login_port, "bbl"));

    auto mcp_reply = exchange(mcp_port, "POST", "/mcp", "{}");
    REQUIRE(mcp_entered.wait_for(k_bound));
    auto login_reply = exchange(login_port, "GET", "/callback?code=1");
    const bool signed_in_early = login_reply.wait_for(300ms) == std::future_status::ready;
    const int  calls_while_mcp = sign_in.calls;
    mcp_release.open();

    CHECK_FALSE(signed_in_early);
    CHECK(calls_while_mcp == 0);
    CHECK(contains(mcp_reply.get(), "\"mcp\":true"));
    CHECK(contains(login_reply.get(), "\"login\":\"bbl\""));
    mcp.stop();
}

TEST_CASE("a cloud login moves between ports without stopping, moving or re-routing the MCP server", "[HttpServer][Login]")
{
    HeldMainThread quit_queue;
    MainThreadGate quit_gate(quit_queue.post());
    FakeSignIn     sign_in;

    const unsigned short mcp_port = free_loopback_port();
    HttpServer           mcp(mcp_port);
    LoginCallbackServer  login(mcp, sign_in.handler(), "orca", quit_gate);
    mcp.set_request_handler(app_routes(login, [] { return json_response({{"mcp", true}}); }));
    mcp.start();
    auto check_mcp_untouched = [&] {
        CHECK(mcp.is_started());
        CHECK(mcp.local_endpoint().port() == mcp_port);
        CHECK(contains(exchange(mcp_port, "GET", "/mcp").get(), "\"mcp\":true"));
    };

    const unsigned short first = free_loopback_port();
    CHECK_FALSE(login.listens_on(first)); // no login yet: the guard answers no callback anywhere
    REQUIRE(login.listen(first, "bbl"));
    CHECK(login.listens_on(first));
    CHECK_FALSE(login.listens_on(mcp_port));
    CHECK(contains(exchange(first, "GET", "/callback?code=1").get(), "\"login\":\"bbl\""));
    check_mcp_untouched();

    const unsigned short second = free_loopback_port();
    REQUIRE(login.listen(second, "orca"));
    CHECK(login.listens_on(second));
    CHECK_FALSE(login.listens_on(first));
    CHECK(contains(exchange(second, "GET", "/callback?code=2").get(), "\"login\":\"orca\""));
    CHECK(exchange(first, "GET", "/callback").get().find("connect failed") == 0);
    check_mcp_untouched();

    mcp.stop();
}

TEST_CASE("a cloud login that asks again for the port it listens on keeps its listener", "[HttpServer][Login]")
{
    // The login dialog asks on every message that needs its callback URL; binding the port a second
    // time, while its listener holds it, failed with EADDRINUSE inside the page's script handler.
    HeldMainThread quit_queue;
    MainThreadGate quit_gate(quit_queue.post());
    FakeSignIn     sign_in;

    HttpServer          mcp(free_loopback_port());
    LoginCallbackServer login(mcp, sign_in.handler(), "orca", quit_gate);
    mcp.set_request_handler(app_routes(login, [] { return json_response({{"mcp", true}}); }));
    mcp.start();

    const unsigned short port = free_loopback_port();
    bool first = false, second = false;
    CHECK_NOTHROW(first = login.listen(port, "bbl"));
    CHECK_NOTHROW(second = login.listen(port, "orca"));
    CHECK(first);
    CHECK(second);
    CHECK(contains(exchange(port, "GET", "/callback?code=5").get(), "\"login\":\"orca\""));
    mcp.stop();
}

#ifndef _WIN32 // Windows' SO_REUSEADDR lets a second socket take a bound port (prompt 09 settles that)
TEST_CASE("a cloud login that cannot bind its port says so instead of throwing", "[HttpServer][Login]")
{
    HeldMainThread quit_queue;
    MainThreadGate quit_gate(quit_queue.post());
    FakeSignIn     sign_in;

    boost::asio::io_context io;
    tcp::acceptor           squatter(io, {boost::asio::ip::address_v4::loopback(), 0}); // another app holds it
    HttpServer              mcp(free_loopback_port());
    LoginCallbackServer     login(mcp, sign_in.handler(), "orca", quit_gate);
    mcp.start();

    bool listening = true;
    CHECK_NOTHROW(listening = login.listen(squatter.local_endpoint().port(), "bbl"));
    CHECK_FALSE(listening);
    CHECK_FALSE(login.listens_on(squatter.local_endpoint().port()));
    mcp.stop();
}
#endif

TEST_CASE("a cloud login's callback route closes when the login ends", "[HttpServer][Login][McpRequestGuard]")
{
    // A page can make the browser deliver a forged callback; a Bambu callback (access_token, ticket)
    // carries no state to check it against, so the route must be closed whenever no login waits.
    HeldMainThread quit_queue;
    MainThreadGate quit_gate(quit_queue.post());
    FakeSignIn     sign_in;

    const unsigned short mcp_port = free_loopback_port();
    HttpServer           mcp(mcp_port);
    LoginCallbackServer  login(mcp, sign_in.handler(), "orca", quit_gate);
    mcp.set_request_guard(app_request_guard(mcp_port, [&login](boost::asio::ip::port_type port) { return login.listens_on(port); }));
    mcp.set_request_handler(app_routes(login, [] { return json_response({{"mcp", true}}); }));
    mcp.start();
    auto refused = [](const std::string& reply) { return reply.rfind("HTTP/1.1 404", 0) == 0 || reply.rfind("connect failed", 0) == 0; };

    const unsigned short login_port = free_loopback_port();
    REQUIRE(login.listen(login_port, "bbl"));
    CHECK(contains(exchange(login_port, "GET", "/callback?code=6").get(), "\"login\":\"bbl\""));
    login.stop_listening();
    CHECK_FALSE(login.listens_on(login_port));
    CHECK(refused(exchange(login_port, "GET", "/callback?access_token=x").get()));

    // The fallback onto the MCP port closes the same way.
    REQUIRE(login.listen(mcp_port, "bbl"));
    CHECK(contains(exchange(mcp_port, "GET", "/callback?code=7").get(), "\"login\":\"bbl\""));
    login.stop_listening();
    CHECK(refused(exchange(mcp_port, "GET", "/callback?access_token=x").get()));
    CHECK(contains(exchange(mcp_port, "GET", "/mcp").get(), "\"mcp\":true"));
    const int sign_ins = sign_in.calls;
    CHECK(sign_ins == 2);
    mcp.stop();
}

TEST_CASE("a cloud login that asks for the MCP server's port never binds it", "[HttpServer][Login]")
{
    // A login can be told LOCALHOST_PORT: the MCP server answers it there, and a login before the MCP
    // server is up binds nothing, so the MCP server can still start on its own port.
    HeldMainThread quit_queue;
    MainThreadGate quit_gate(quit_queue.post());
    FakeSignIn     sign_in;

    const unsigned short mcp_port = free_loopback_port();
    HttpServer           mcp(mcp_port);
    LoginCallbackServer  login(mcp, sign_in.handler(), "orca", quit_gate);
    mcp.set_request_handler(app_routes(login, [] { return json_response({{"mcp", true}}); }));

    CHECK_FALSE(login.listen(mcp_port, "bbl")); // before the MCP server is up
    CHECK_FALSE(login.listens_on(mcp_port));
    mcp.start();
    CHECK(mcp.local_endpoint().port() == mcp_port);

    REQUIRE(login.listen(mcp_port, "bbl"));
    CHECK(login.listens_on(mcp_port));
    CHECK(contains(exchange(mcp_port, "GET", "/callback?code=3").get(), "\"login\":\"bbl\""));
    CHECK(contains(exchange(mcp_port, "GET", "/mcp").get(), "\"mcp\":true"));
    mcp.stop();
}

TEST_CASE("a cloud login's callback that arrives while the app is quitting never reaches the sign-in", "[HttpServer][Login]")
{
    // A sign-in makes network calls that a quit would then have to wait for.
    HeldMainThread quit_queue;
    MainThreadGate quit_gate(quit_queue.post());
    FakeSignIn     sign_in;

    HttpServer          mcp(0);
    LoginCallbackServer login(mcp, sign_in.handler(), "orca", quit_gate);
    quit_gate.close();

    std::stringstream page;
    login.answer("/callback?code=4")->write_response(page);
    CHECK(contains(page.str(), "OrcaSlicer is quitting"));
    CHECK(sign_in.calls == 0);
}

TEST_CASE("the MCP server refuses a web page's request and a rebound one before any tool sees them", "[HttpServer][McpRequestGuard]")
{
    // What GUI_App installs, on a real server: the headers come off the wire.
    std::atomic<int>     mcp_calls{0};
    const unsigned short mcp_port = free_loopback_port();
    HttpServer           mcp(mcp_port);
    mcp.set_request_guard(app_request_guard(mcp_port, [](boost::asio::ip::port_type) { return false; }));
    mcp.set_request_handler([&](const std::string&, const std::string&, const std::string&) {
        ++mcp_calls;
        return json_response({{"mcp", true}});
    });
    mcp.start();
    const std::string port = std::to_string(mcp_port);
    const std::string call = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";

    const std::string from_page = exchange(mcp_port, "POST", "/mcp", call, nullptr,
                                           "Host: localhost:" + port + "\r\nOrigin: http://evil.example\r\n").get();
    CHECK(from_page.rfind("HTTP/1.1 403 Forbidden", 0) == 0);
    CHECK(contains(from_page, "\"code\":-32003"));

    const std::string rebound = exchange(mcp_port, "POST", "/mcp", call, nullptr, "Host: evil.example:" + port + "\r\n").get();
    CHECK(rebound.rfind("HTTP/1.1 403 Forbidden", 0) == 0);
    CHECK(mcp_calls == 0);

    const std::string from_bridge = exchange(mcp_port, "POST", "/mcp", call).get();
    CHECK(contains(from_bridge, "\"mcp\":true"));
    const std::string from_curl = exchange(mcp_port, "POST", "/mcp", call, nullptr, "Host: 127.0.0.1:" + port + "\r\n").get();
    CHECK(contains(from_curl, "\"mcp\":true"));
    CHECK(mcp_calls == 2);

    // No login is listening, so the MCP port answers no login callback.
    CHECK(exchange(mcp_port, "GET", "/callback?access_token=x").get().rfind("HTTP/1.1 404", 0) == 0);
    mcp.stop();
}
