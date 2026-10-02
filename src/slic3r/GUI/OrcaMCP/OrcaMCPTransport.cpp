#include "OrcaMCPTransport.hpp"

#include <algorithm>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

constexpr int k_invalid_request = -32600;

// True for a media-type parameter that gives the type a quality of 0, which refuses it: q=0, q=0.0, ...
bool is_zero_quality(const std::string& parameter)
{
    const size_t equals = parameter.find('=');
    if (equals == std::string::npos || !boost::iequals(boost::trim_copy(parameter.substr(0, equals)), "q"))
        return false;
    const std::string value = boost::trim_copy(parameter.substr(equals + 1));
    return value == "0" || (boost::starts_with(value, "0.") && std::all_of(value.begin() + 2, value.end(), [](char c) { return c == '0'; }));
}

// True when one element of an Accept header ("type/subtype; parameters") accepts text/event-stream.
bool accepts_event_stream(const std::string& element)
{
    std::vector<std::string> parts;
    boost::split(parts, element, boost::is_any_of(";"));
    return boost::iequals(boost::trim_copy(parts.front()), "text/event-stream") &&
           std::none_of(parts.begin() + 1, parts.end(), is_zero_quality);
}

} // namespace

bool is_notification(const nlohmann::json& message)
{
    return message.is_object() && message.contains("method") && !message.contains("id");
}

bool asks_for_event_stream(const std::optional<std::string>& accept)
{
    if (!accept)
        return false;
    std::vector<std::string> elements;
    boost::split(elements, *accept, boost::is_any_of(","));
    return std::any_of(elements.begin(), elements.end(), accepts_event_stream);
}

std::shared_ptr<HttpServer::Response> method_not_allowed_response(const std::string& reason)
{
    const nlohmann::json error = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", k_invalid_request}, {"message", reason}}}};
    return std::make_shared<HttpServer::ResponseJson>(error.dump(), 405, HttpServer::Response::Headers{{"Allow", "GET, POST"}});
}

}}} // namespace Slic3r::GUI::OrcaMCP
