#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/GUI/HttpServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPLoginServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPMainThreadGate.hpp"

// The HTTP server the MCP server and the cloud login answer on: where it listens, how it stops while
// a request is still being handled, and that quitting with an MCP call in flight answers that call
// and ends in bounded time (the 2026-09-26 quit_app deadlock).
//
// Every test that blocks a handler releases it at the end, so a server that does not stop in time
// fails the test instead of hanging the suite.

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using namespace std::chrono_literals;
using boost::asio::ip::tcp;

namespace {

constexpr auto k_bound = 2s;

// A port nothing listens on right now, on the loopback address.
unsigned short free_loopback_port()
{
    boost::asio::io_context io;
    tcp::acceptor           probe(io, {boost::asio::ip::address_v4::loopback(), 0});
    return probe.local_endpoint().port();
}

// One HTTP request to 127.0.0.1:`port`, on a thread of its own. The future holds everything the
// server sent before it closed the connection, or why the connection failed.
std::future<std::string> exchange(unsigned short port, const std::string& method, const std::string& path,
                                  const std::string& body = std::string())
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
        std::string reply;
        boost::asio::read(socket, boost::asio::dynamic_buffer(reply), ec); // to the server's close
        return reply;
    });
}

std::shared_ptr<HttpServer::Response> json_response(const nlohmann::json& body)
{
    return std::make_shared<HttpServer::ResponseJson>(body.dump());
}

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

// A main thread that runs nothing until told to, like the real one while it joins the HTTP thread.
class HeldMainThread
{
public:
    MainThreadGate::Post post()
    {
        return [this](std::function<void()> task) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_tasks.push_back(std::move(task));
            }
            m_queued.open();
        };
    }
    bool wait_for_a_task() { return m_queued.wait_for(k_bound); }
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
    std::deque<std::function<void()>> m_tasks;
    Latch                             m_queued;
};

} // namespace

TEST_CASE("the HTTP server listens on this machine only", "[HttpServer]")
{
    HttpServer server(0);
    server.set_request_handler([](const std::string&) { return json_response({{"status", "ok"}}); });
    server.start();

    const tcp::endpoint where = server.local_endpoint();
    CHECK(where.address().is_loopback());
    CHECK(where.port() != 0);
    CHECK(exchange(where.port(), "GET", "/mcp").get().find("\"status\":\"ok\"") != std::string::npos);

    server.stop();
    CHECK_FALSE(server.is_started());
}

TEST_CASE("stopping the HTTP server takes at most its bound while a request is still being handled", "[HttpServer]")
{
    Latch      entered, release;
    HttpServer server(0);
    server.set_request_handler([&entered, &release](const std::string&) {
        entered.open();
        release.wait();
        return json_response({{"status", "late"}});
    });
    server.start();

    auto reply = exchange(server.local_endpoint().port(), "POST", "/mcp", "{}");
    REQUIRE(entered.wait_for(k_bound));

    const auto started  = std::chrono::steady_clock::now();
    auto       stopping = std::async(std::launch::async, [&server] { server.stop(200); });
    const bool stopped  = stopping.wait_for(k_bound) == std::future_status::ready;
    const auto took     = std::chrono::steady_clock::now() - started;

    // Lets a server that is still joining finish, so the test ends either way. Past its bound stop()
    // leaves the server's thread running inside this handler; the reply comes only after the handler
    // has returned, so `server` outlives every use that thread makes of it.
    release.open();
    stopping.wait();
    reply.wait();

    CHECK(stopped);
    CHECK(took < 1500ms);
    CHECK_FALSE(server.is_started());
}

TEST_CASE("quitting with an MCP call waiting on the main thread answers the call and stops at once",
          "[HttpServer][McpShutdown]")
{
    // The main thread never runs the call's work: it is the thread doing the quitting.
    HeldMainThread main_thread;
    MainThreadGate gate(main_thread.post());

    HttpServer server(0);
    server.set_request_handler([&gate](const std::string&, const std::string&, const std::string&) {
        try {
            return json_response(gate.call([] { return nlohmann::json{{"status", "ran"}}; }));
        } catch (const McpShuttingDown& e) {
            return json_response({{"error", e.what()}});
        }
    });
    server.start();

    auto reply = exchange(server.local_endpoint().port(), "POST", "/mcp", R"({"jsonrpc":"2.0","id":1})");
    REQUIRE(main_thread.wait_for_a_task()); // the call is waiting for the main thread

    // GUI_App::stop_http_server's order: release the waiting call, then stop the server.
    const auto started  = std::chrono::steady_clock::now();
    auto       quitting = std::async(std::launch::async, [&] {
        gate.close();
        server.stop();
    });
    const bool quit     = quitting.wait_for(k_bound) == std::future_status::ready;
    const auto took     = std::chrono::steady_clock::now() - started;
    const bool answered = reply.wait_for(k_bound) == std::future_status::ready;
    if (!quit || !answered)
        main_thread.run_all(); // what never happens in the app: lets the test fail instead of hanging
    quitting.wait();

    CHECK(quit);
    CHECK(took < 1s);
    REQUIRE(answered);
    const std::string body = reply.get();
    INFO(body);
    CHECK(body.find("OrcaMCP is quitting") != std::string::npos);
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
