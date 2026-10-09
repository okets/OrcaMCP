#include "ObicoAgentHandoff.hpp"

#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/format.hpp>
#include <nlohmann/json.hpp>

#include "Http.hpp"

namespace Slic3r { namespace ObicoAgentHandoff {

namespace {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
using tcp           = boost::asio::ip::tcp;

constexpr std::chrono::seconds kStepTimeout{5};

// An http:// URL's host and port ("80" when it names none), and its path; nothing for any other URL.
struct HttpAddress
{
    std::string host, port, path;
};

std::optional<HttpAddress> http_address(const std::string& url)
{
    const std::string scheme = "http://";
    if (!boost::istarts_with(url, scheme))
        return std::nullopt;
    const std::string rest      = url.substr(scheme.size());
    const size_t      slash     = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    HttpAddress       address;
    address.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (!authority.empty() && authority.front() == '[') { // [::1]:port
        const size_t close = authority.find(']');
        if (close == std::string::npos)
            return std::nullopt;
        address.host = authority.substr(1, close - 1);
        address.port = close + 1 < authority.size() && authority[close + 1] == ':' ? authority.substr(close + 2) : "80";
    } else {
        const size_t colon = authority.rfind(':');
        address.host       = authority.substr(0, colon);
        address.port       = colon == std::string::npos ? "80" : authority.substr(colon + 1);
    }
    if (address.host.empty() || address.port.empty() || address.port.find_first_not_of("0123456789") != std::string::npos)
        return std::nullopt;
    return address;
}

std::string authority_of(const HttpAddress& address)
{
    const bool ipv6 = address.host.find(':') != std::string::npos;
    return (ipv6 ? "[" + address.host + "]" : address.host) + ":" + address.port;
}

} // namespace

std::optional<std::string> agent_url(const nlohmann::json& obico_document)
{
    const nlohmann::json* webcams = nullptr;
    if (obico_document.is_object() && obico_document.contains("settings") && obico_document["settings"].is_object() &&
        obico_document["settings"].contains("webcams") && obico_document["settings"]["webcams"].is_array())
        webcams = &obico_document["settings"]["webcams"];
    if (webcams == nullptr)
        return std::nullopt;

    // The primary camera first, as the Device page takes it; then the others in order.
    for (const bool primary : {true, false})
        for (const nlohmann::json& webcam : *webcams) {
            if (!webcam.is_object() || webcam.value("is_primary_camera", false) != primary)
                continue;
            const auto url = webcam.find("stream_url");
            if (url == webcam.end() || !url->is_string())
                continue;
            if (const auto address = http_address(url->get<std::string>()))
                return "http://" + authority_of(*address);
        }
    return std::nullopt;
}

std::string slice_table_body(const std::string& file_name, const FlashforgeJobProgress::SliceTable& table)
{
    return nlohmann::json{{"file_name", file_name}, {"table", nlohmann::json::parse(FlashforgeJobProgress::to_json(table))}}.dump();
}

std::string obico_websocket_target(const std::string& token)
{
    // encodeURIComponent's unreserved set.
    std::ostringstream out;
    out << "/ws/token/web/";
    for (const unsigned char c : token) {
        if (std::isalnum(c) || std::string("-_.!~*'()").find(static_cast<char>(c)) != std::string::npos)
            out << c;
        else
            out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::nouppercase
                << std::dec;
    }
    out << '/';
    return out.str();
}

bool read_obico_document(const std::string& obico_url, const std::string& token, nlohmann::json& document, std::string& error)
{
    if (obico_url.empty() || token.empty()) {
        error = "no Obico server in the printer settings";
        return false;
    }
    const auto address = http_address(obico_url);
    if (!address) {
        error = "the Obico URL is not an http:// address";
        return false;
    }

    // Asynchronous steps under one deadline: beast applies timeouts to asynchronous operations only,
    // and a server that accepts and then says nothing must not hold the upload thread.
    boost::asio::io_context                ioc;
    tcp::resolver                          resolver(ioc);
    websocket::stream<beast::tcp_stream>   ws(ioc);
    beast::flat_buffer                     buffer;
    const std::string                      target = obico_websocket_target(token);
    bool                                   done = false;

    const auto fail = [&](const char* step, const beast::error_code& ec) { error = std::string(step) + ": " + ec.message(); };
    resolver.async_resolve(address->host, address->port, [&](beast::error_code ec, tcp::resolver::results_type results) {
        if (ec)
            return fail("resolve", ec);
        beast::get_lowest_layer(ws).expires_after(kStepTimeout);
        beast::get_lowest_layer(ws).async_connect(results, [&](beast::error_code ec, tcp::endpoint) {
            if (ec)
                return fail("connect", ec);
            beast::get_lowest_layer(ws).expires_never();
            ws.set_option(websocket::stream_base::timeout{kStepTimeout, kStepTimeout, false});
            ws.async_handshake(authority_of(*address), target, [&](beast::error_code ec) {
                if (ec)
                    return fail("handshake", ec);
                ws.async_read(buffer, [&](beast::error_code ec, std::size_t) {
                    if (ec)
                        return fail("read", ec);
                    done = true;
                });
            });
        });
    });
    ioc.run_for(3 * kStepTimeout);
    if (!done) {
        if (error.empty())
            error = "Obico did not answer in time";
        return false;
    }

    document = nlohmann::json::parse(beast::buffers_to_string(buffer.data()), nullptr, false);
    beast::error_code ignored;
    beast::get_lowest_layer(ws).socket().close(ignored);
    if (!document.is_object()) {
        error = "Obico's answer is not a printer document";
        return false;
    }
    return true;
}

std::string hand_over_slice_table(const std::string&                       obico_url,
                                  const std::string&                       token,
                                  const std::string&                       file_name,
                                  const FlashforgeJobProgress::SliceTable& table)
{
    nlohmann::json document;
    std::string    error;
    if (!read_obico_document(obico_url, token, document, error))
        return "not handed to the Obico agent: " + error;
    const auto agent = agent_url(document);
    if (!agent)
        return "not handed to the Obico agent: Obico lists no camera address of the agent";

    std::string outcome;
    Http::post(*agent + "/api/slice-table")
        .header("Content-Type", "application/json")
        .set_post_body(slice_table_body(file_name, table))
        .timeout_connect(static_cast<long>(kStepTimeout.count()))
        .timeout_max(static_cast<long>(2 * kStepTimeout.count()))
        .on_complete([&](std::string body, unsigned status) {
            outcome = (boost::format("handed to the Obico agent at %1%: HTTP %2% %3%") % *agent % status % body).str();
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            outcome = (boost::format("not handed to the Obico agent at %1%: HTTP %2% %3% %4%") % *agent % status % err % body).str();
        })
        .perform_sync();
    return outcome;
}

}} // namespace Slic3r::ObicoAgentHandoff
