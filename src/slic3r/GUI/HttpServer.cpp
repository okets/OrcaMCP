#include "HttpServer.hpp"
#include <boost/log/trivial.hpp>
#include "GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

namespace Slic3r {
namespace GUI {

std::string url_get_param(const std::string& url, const std::string& key)
{
    size_t start = url.find(key);
    if (start == std::string::npos) return "";
    size_t eq = url.find('=', start);
    if (eq == std::string::npos) return "";
    std::string key_str = url.substr(start, eq - start);
    if (key_str != key)
        return "";
    start += key.size() + 1;
    size_t end = url.find('&', start);
    if (end == std::string::npos) end = url.length(); // Last param
    std::string result = url.substr(start, end - start);
    return result;
}

void session::start()
{
    read_first_line();
}

void session::stop()
{
    boost::system::error_code ignored_ec;
    socket.shutdown(boost::asio::socket_base::shutdown_both, ignored_ec);
    socket.close(ignored_ec);
}

void session::read_first_line()
{
    auto self(shared_from_this());

    async_read_until(socket, buff, '\r', [this, self](const boost::beast::error_code& e, std::size_t s) {
        if (!e) {
            std::string  line, ignore;
            std::istream stream{&buff};
            std::getline(stream, line, '\r');
            std::getline(stream, ignore, '\n');
            headers.on_read_request_line(line);
            read_next_line();
        } else if (e != boost::asio::error::operation_aborted) {
            server.stop(self);
        }
    });
}

void session::read_body()
{
    auto self(shared_from_this());

    int content_len = headers.content_length();
    if (content_len <= 0) {
        process_request();
        return;
    }

    // Allocate buffer for body
    auto body_buffer = std::make_shared<std::vector<char>>(content_len);

    // Check if we already have some body data in the streambuf
    size_t already_read = buff.size();
    if (already_read > 0) {
        std::istream stream{&buff};
        size_t to_copy = std::min(already_read, static_cast<size_t>(content_len));
        stream.read(body_buffer->data(), to_copy);
        body.assign(body_buffer->data(), to_copy);

        if (to_copy >= static_cast<size_t>(content_len)) {
            process_request();
            return;
        }
    }

    // Read remaining body
    size_t remaining = content_len - body.size();
    auto remaining_buffer = std::make_shared<std::vector<char>>(remaining);

    async_read(socket, boost::asio::buffer(*remaining_buffer, remaining),
               [this, self, remaining_buffer](const boost::beast::error_code& e, std::size_t bytes_read) {
                   if (!e) {
                       body.append(remaining_buffer->data(), bytes_read);
                       process_request();
                   } else if (e != boost::asio::error::operation_aborted) {
                       server.stop(self);
                   }
               });
}

void session::process_request()
{
    auto self(shared_from_this());

    std::cout << "Request received: " << headers.get_method() << " " << headers.get_url();
    if (!body.empty()) {
        std::cout << " (body: " << body.size() << " bytes)";
    }
    std::cout << std::endl;

    const std::string url_str = Http::url_decode(headers.get_url());
    const auto resp = server.server.m_request_handler(headers.get_method(), url_str, body);

    std::stringstream ssOut;
    resp->write_response(ssOut);
    std::shared_ptr<std::string> str = std::make_shared<std::string>(ssOut.str());

    async_write(socket, boost::asio::buffer(str->c_str(), str->length()),
                [this, self, str](const boost::beast::error_code& e, std::size_t s) {
        std::cout << "done" << std::endl;
        server.stop(self);
    });
}

void session::read_next_line()
{
    auto self(shared_from_this());

    async_read_until(socket, buff, '\r', [this, self](const boost::beast::error_code& e, std::size_t s) {
        if (!e) {
            std::string  line, ignore;
            std::istream stream{&buff};
            std::getline(stream, line, '\r');
            std::getline(stream, ignore, '\n');
            headers.on_read_header(line);

            if (line.length() == 0) {
                // Handle OPTIONS preflight requests for CORS
                if (headers.get_method() == "OPTIONS") {
                    server.stop(self);
                    return;
                }

                // If there's a body to read, read it; otherwise process immediately
                if (headers.content_length() > 0) {
                    read_body();
                } else {
                    process_request();
                }
            } else {
                read_next_line();
            }
        } else if (e != boost::asio::error::operation_aborted) {
            server.stop(self);
        }
    });
}

void HttpServer::IOServer::do_accept()
{
    acceptor.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
        if (!acceptor.is_open()) {
            return;
        }

        if (!ec) {
            const auto ss = std::make_shared<session>(*this, std::move(socket));
            start(ss);
        }

        do_accept();
    });
}

void HttpServer::IOServer::start(std::shared_ptr<session> session)
{
    sessions.insert(session);
    session->start();
}

void HttpServer::IOServer::stop(std::shared_ptr<session> session)
{
    sessions.erase(session);
    session->stop();
}

void HttpServer::IOServer::stop_all()
{
    for (auto s : sessions) {
        s->stop();
    }
    sessions.clear();
}


HttpServer::HttpServer(boost::asio::ip::port_type port) : port(port) {}

void HttpServer::start()
{
    BOOST_LOG_TRIVIAL(info) << "start_http_service...";
    start_http_server    = true;
    m_http_server_thread = create_thread([this] {
        set_current_thread_name("http_server");
        server_ = std::make_unique<IOServer>(*this);
        server_->acceptor.listen();

        server_->do_accept();

        server_->io_service.run();
    });
}

void HttpServer::stop()
{
    start_http_server = false;
    if (server_) {
        server_->acceptor.close();
        server_->stop_all();
        server_->io_service.stop();
    }
    if (m_http_server_thread.joinable())
        m_http_server_thread.join();
    server_.reset();
}

void HttpServer::set_request_handler(const RequestHandlerFn& request_handler)
{
    this->m_request_handler = request_handler;
}

void HttpServer::set_request_handler(const std::function<std::shared_ptr<Response>(const std::string&)>& request_handler)
{
    // Wrap legacy handler in new signature
    this->m_request_handler = [request_handler](const std::string& method, const std::string& url, const std::string& body) {
        return request_handler(url);
    };
}

std::shared_ptr<HttpServer::Response> HttpServer::bbl_auth_handle_request(const std::string& method, const std::string& url, const std::string& body)
{
    BOOST_LOG_TRIVIAL(info) << "thirdparty_login: get_response";

    const std::string auth_code = url_get_param(url, "code");
    if (!auth_code.empty()) {
        std::string state = url_get_param(url, "state");
        NetworkAgent* agent = wxGetApp().getAgent();
        if (!agent) {
            return std::make_shared<ResponseNotFound>();
        }

        json payload;
        payload["command"] = "user_login";
        payload["data"]["code"] = auth_code;
        payload["data"]["state"] = state;

        agent->change_user(payload.dump());
        const bool login_ok = agent->is_user_login();
        if (login_ok) {
            wxGetApp().request_user_login(1);
            GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
        }

        const std::string title = login_ok ? "Authentication complete" : "Authentication failed";
        const std::string message = login_ok
            ? "You can return to OrcaSlicer. This window will close automatically."
            : "Something went wrong. Please return to OrcaSlicer and try again.";
        const std::string html =
            "<html><head><meta charset=\"utf-8\">"
            "<style>body{font-family:Arial,sans-serif;background:#f7f7f7;color:#222;margin:32px;}"
            "a.button{display:inline-block;padding:10px 16px;margin-top:12px;background:#0f8bff;color:#fff;text-decoration:none;border-radius:6px;}"
            "</style></head><body><div class=\"container\">"
            "<h2>" + title + "</h2>"
            "<p>" + message + "</p>"
            "<script>setTimeout(function(){try{window.close();}catch(e){}},1500);</script>"
            "</div></body></html>";
        return std::make_shared<ResponseHtml>(html);
    }

    if (boost::contains(url, "access_token")) {
        std::string   redirect_url           = url_get_param(url, "redirect_url");
        std::string   access_token           = url_get_param(url, "access_token");
        std::string   refresh_token          = url_get_param(url, "refresh_token");
        std::string   expires_in_str         = url_get_param(url, "expires_in");
        std::string   refresh_expires_in_str = url_get_param(url, "refresh_expires_in");
        NetworkAgent* agent                  = wxGetApp().getAgent();

        unsigned int http_code;
        std::string  http_body;
        int          result = agent->get_my_profile(access_token, &http_code, &http_body);
        if (result == 0) {
            std::string user_id;
            std::string user_name;
            std::string user_account;
            std::string user_avatar;
            try {
                json user_j = json::parse(http_body);
                if (user_j.contains("uidStr"))
                    user_id = user_j["uidStr"].get<std::string>();
                if (user_j.contains("name"))
                    user_name = user_j["name"].get<std::string>();
                if (user_j.contains("avatar"))
                    user_avatar = user_j["avatar"].get<std::string>();
                if (user_j.contains("account"))
                    user_account = user_j["account"].get<std::string>();
            } catch (...) {
                ;
            }
            json j;
            j["data"]["refresh_token"]      = refresh_token;
            j["data"]["token"]              = access_token;
            j["data"]["expires_in"]         = expires_in_str;
            j["data"]["refresh_expires_in"] = refresh_expires_in_str;
            j["data"]["user"]["uid"]        = user_id;
            j["data"]["user"]["name"]       = user_name;
            j["data"]["user"]["account"]    = user_account;
            j["data"]["user"]["avatar"]     = user_avatar;
            agent->change_user(j.dump());
            if (agent->is_user_login()) {
                wxGetApp().request_user_login(1);
            }
            GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
            std::string location_str = (boost::format("%1%?result=success") % redirect_url).str();
            return std::make_shared<ResponseRedirect>(location_str);
        } else {
            std::string error_str    = "get_user_profile_error_" + std::to_string(result);
            std::string location_str = (boost::format("%1%?result=fail&error=%2%") % redirect_url % error_str).str();
            return std::make_shared<ResponseRedirect>(location_str);
        }
    } else {
        return std::make_shared<ResponseNotFound>();
    }
}

void HttpServer::ResponseNotFound::write_response(std::stringstream& ssOut)
{
    const std::string sHTML = "<html><body><h1>404 Not Found</h1><p>There's nothing here.</p></body></html>";
    ssOut << "HTTP/1.1 404 Not Found" << std::endl;
    ssOut << "content-type: text/html" << std::endl;
    ssOut << "content-length: " << sHTML.length() << std::endl;
    ssOut << std::endl;
    ssOut << sHTML;
}

void HttpServer::ResponseRedirect::write_response(std::stringstream& ssOut)
{
    const std::string sHTML =
        "<html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"0;url=" + location_str + "\">"
        "<style>body{font-family:Arial,sans-serif;background:#f7f7f7;color:#222;margin:32px;}"
        "a.button{display:inline-block;padding:10px 16px;margin-top:12px;background:#0f8bff;color:#fff;text-decoration:none;border-radius:6px;}"
        "</style></head><body><div class=\"container\">"
        "<h2>Authentication complete</h2>"
        "<p>You can return to OrcaSlicer. If your browser does not redirect automatically, use the button below.</p>"
        "<a class=\"button\" href=\"" + location_str + "\">Continue</a>"
        "<script>setTimeout(function(){try{window.close();}catch(e){}},1500);</script>"
        "</div></body></html>";
    ssOut << "HTTP/1.1 302 Found" << std::endl;
    ssOut << "Location: " << location_str << std::endl;
    ssOut << "content-type: text/html" << std::endl;
    ssOut << "content-length: " << sHTML.length() << std::endl;
    ssOut << std::endl;
    ssOut << sHTML;
}

void HttpServer::ResponseJson::write_response(std::stringstream& ssOut)
{
    std::string status_text;
    switch (status_code) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 400: status_text = "Bad Request"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 500: status_text = "Internal Server Error"; break;
        default: status_text = "OK"; break;
    }

    ssOut << "HTTP/1.1 " << status_code << " " << status_text << std::endl;
    ssOut << "Content-Type: application/json" << std::endl;
    ssOut << "Access-Control-Allow-Origin: *" << std::endl;
    ssOut << "Access-Control-Allow-Methods: GET, POST, OPTIONS" << std::endl;
    ssOut << "Access-Control-Allow-Headers: Content-Type" << std::endl;
    ssOut << "Content-Length: " << json_str.length() << std::endl;
    ssOut << std::endl;
    ssOut << json_str;
}

void HttpServer::ResponseHtml::write_response(std::stringstream& ssOut)
{
    ssOut << "HTTP/1.1 200 OK" << std::endl;
    ssOut << "content-type: text/html" << std::endl;
    ssOut << "content-length: " << html.length() << std::endl;
    ssOut << std::endl;
    ssOut << html;
}

} // GUI
} //Slic3r
