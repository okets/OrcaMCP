#ifndef slic3r_Http_App_hpp_
#define slic3r_Http_App_hpp_

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <stack>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <string>
#include <set>
#include <memory>
#include <utility>

#define LOCALHOST_PORT      13618
#define LOCALHOST_URL       "http://localhost:"

namespace Slic3r { namespace GUI {

class session;

class http_headers
{
    std::string method;
    std::string url;
    std::string version;

    std::map<std::string, std::string> headers;

    friend class session;
public:
    std::string get_url() { return url; }
    std::string get_method() { return method; }
    // Orca: a header's value, by its lowercase name; nothing when the request did not send it.
    std::optional<std::string> value(const std::string& name) const
    {
        const auto it = headers.find(name);
        return it == headers.end() ? std::nullopt : std::optional<std::string>(it->second);
    }

    int content_length()
    {
        auto request = headers.find("content-length");
        if (request != headers.end()) {
            std::stringstream ssLength(request->second);
            int               content_length;
            ssLength >> content_length;
            return content_length;
        }
        return 0;
    }

    void on_read_header(std::string line)
    {
        // std::cout << "header: " << line << std::endl;

        std::stringstream ssHeader(line);
        std::string       headerName;
        std::getline(ssHeader, headerName, ':');

        // Normalize header name to lowercase (HTTP headers are case-insensitive)
        std::transform(headerName.begin(), headerName.end(), headerName.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        std::string value;
        std::getline(ssHeader, value);
        // Trim leading whitespace from value
        size_t start = value.find_first_not_of(" \t");
        if (start != std::string::npos) {
            value = value.substr(start);
        }
        headers[headerName] = value;
    }

    void on_read_request_line(std::string line)
    {
        std::stringstream ssRequestLine(line);
        ssRequestLine >> method;
        ssRequestLine >> url;
        ssRequestLine >> version;

        std::cout << "request for resource: " << url << std::endl;
    }
};

class HttpServer
{
    boost::asio::ip::port_type port;

public:
    class Response
    {
    public:
        virtual ~Response()                                   = default;
        virtual void write_response(std::stringstream& ssOut) = 0;
    };

    class ResponseNotFound : public Response
    {
    public:
        ~ResponseNotFound() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    class ResponseRedirect : public Response
    {
        const std::string location_str;

    public:
        ResponseRedirect(const std::string& location) : location_str(location) {}
        ~ResponseRedirect() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    class ResponseJson : public Response
    {
        const std::string json_str;
        int status_code;

    public:
        ResponseJson(const std::string& json, int status = 200) : json_str(json), status_code(status) {}
        ~ResponseJson() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    class ResponseHtml : public Response
    {
        const std::string html;

    public:
        explicit ResponseHtml(std::string html) : html(std::move(html)) {}
        ~ResponseHtml() override = default;
        void write_response(std::stringstream& ssOut) override;
    };

    // Request handler type that includes method, URL, and body
    using RequestHandlerFn = std::function<std::shared_ptr<Response>(const std::string& method, const std::string& url, const std::string& body)>;

    // Orca: what a request guard sees of a request, before the handler does.
    struct RequestInfo
    {
        std::string                method;
        std::string                url;        // decoded, as the handler gets it
        std::optional<std::string> origin;     // the Origin header, sent by browsers on cross-origin requests
        std::optional<std::string> host;       // the Host header
        boost::asio::ip::port_type local_port; // the port the request came in on
    };
    // Returns the response that refuses the request, or nullptr to let the handler answer it.
    using RequestGuardFn = std::function<std::shared_ptr<Response>(const RequestInfo&)>;


    HttpServer(boost::asio::ip::port_type port = LOCALHOST_PORT);
    ~HttpServer();

    boost::thread m_http_server_thread;
    bool          start_http_server = false;

    bool is_started() { return start_http_server; }
    void start();
    // Stops listening and joins the server's thread. A request still being handled is waited for up
    // to `bound` (without one, for as long as it takes), and never abandoned: its handler may be
    // using what the app destroys next. When the bound passes first this returns false, and leaves the
    // thread and the server running: the caller must then not destroy the server, and ends the process
    // instead (GUI_App::stop_http_server). A reply that is still being written gets up to
    // reply_drain_ms to reach its client before its connection is closed.
    static constexpr std::chrono::milliseconds no_bound = std::chrono::milliseconds::max();
    bool stop(std::chrono::milliseconds bound = no_bound);
    void set_port(boost::asio::ip::port_type new_port) { port = new_port; }
    boost::asio::ip::port_type get_port() const { return port; }
    // Where the server listens while it is started (the port is the real one even when it was
    // started on port 0); a default endpoint otherwise.
    boost::asio::ip::tcp::endpoint local_endpoint() const;
    // Also listens on `also_port`, with the same handler and on the same thread, until stop() or the
    // next call; 0 stops that listener. The cloud login's callback uses it, so login callbacks and MCP
    // calls are served one at a time, as when both shared one port. Returns false when the server is
    // not started; throws what binding throws.
    bool listen_also(boost::asio::ip::port_type also_port);

    static constexpr int reply_drain_ms = 2000;

    // Set request handler with full signature (method, url, body)
    void set_request_handler(const RequestHandlerFn& request_handler);

    // Legacy: Set request handler with URL only (for backward compatibility)
    void set_request_handler(const std::function<std::shared_ptr<Response>(const std::string&)>& request_handler);

    // Orca: every request is shown to `guard` before the handler, which never sees one it refuses.
    // Set it before start(). OrcaMCP's refuses MCP requests from web pages (OrcaMCPRequestGuard.hpp).
    void set_request_guard(const RequestGuardFn& guard) { m_request_guard = guard; }

    // Default handler for BBL authentication
    static std::shared_ptr<Response> bbl_auth_handle_request(const std::string& method, const std::string& url, const std::string& body);
    static std::shared_ptr<Response> auth_handle_request(const std::string& url, const std::string& provider);

private:
    // Loopback only: the MCP server can load files, change presets and start prints, and has no
    // authentication, so nothing off this machine may reach it; the login callback is local too.
    static boost::asio::ip::tcp::endpoint loopback_endpoint(boost::asio::ip::port_type port)
    {
        return {boost::asio::ip::address_v4::loopback(), port};
    }

    class IOServer
    {
    public:
        using Acceptor = boost::asio::ip::tcp::acceptor;

        HttpServer&                        server;
        boost::asio::io_service            io_service;
        Acceptor                           acceptor;
        std::set<std::shared_ptr<session>> sessions;
        std::shared_ptr<Acceptor>          also;                     // listen_also's listener
        boost::asio::steady_timer          drain_timer{io_service};  // bounds stop()'s drain
        bool                               stopping = false;

        IOServer(HttpServer& server) : server(server), acceptor(io_service, loopback_endpoint(server.port)) {}

        void accept_on(Acceptor& listener, std::shared_ptr<Acceptor> keep_alive);
        void replace_also(std::shared_ptr<Acceptor> listener);
        void begin_stop();

        void start(std::shared_ptr<session> session);
        void stop(std::shared_ptr<session> session);
        void stop_all();
    };
    friend class session;

    std::unique_ptr<IOServer> server_{nullptr};

    RequestHandlerFn m_request_handler{&HttpServer::bbl_auth_handle_request};
    RequestGuardFn   m_request_guard;
};

class session : public std::enable_shared_from_this<session>
{
    HttpServer::IOServer& server;
    boost::asio::ip::tcp::socket socket;

    boost::asio::streambuf buff;
    http_headers headers;
    std::string body;
    bool replying = false; // Orca: its reply is being written, so stop() lets it finish

    void read_first_line();
    void read_next_line();
    void read_body();
    void process_request();

public:
    session(HttpServer::IOServer& server, boost::asio::ip::tcp::socket socket) : server(server), socket(std::move(socket)) {}

    void start();
    void stop();
    bool is_replying() const { return replying; }
};

std::string url_get_param(const std::string& url, const std::string& key);

}};

#endif
