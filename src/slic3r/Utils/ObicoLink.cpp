#include "ObicoLink.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

std::string trimmed_option(const DynamicPrintConfig& config, const char* key)
{
    const auto* option = config.option<ConfigOptionString>(key);
    if (option == nullptr)
        return {};
    std::string value = option->value;
    boost::algorithm::trim(value);
    return value;
}

std::string base_url(const DynamicPrintConfig& config)
{
    std::string url = trimmed_option(config, OBICO_URL_KEY);
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return url;
}

} // namespace

nlohmann::json obico_link_json(const DynamicPrintConfig& config)
{
    const std::string url   = base_url(config);
    const std::string token = trimmed_option(config, OBICO_TOKEN_KEY);
    if (url.empty() || token.empty())
        return nullptr;
    return nlohmann::json{{"url", url}, {"token", token}};
}

nlohmann::json obico_status_json(const DynamicPrintConfig& config)
{
    const nlohmann::json link = obico_link_json(config);
    if (link.is_null())
        return nlohmann::json{{"configured", false}, {"url", nullptr}};
    return nlohmann::json{{"configured", true}, {"url", link["url"]}};
}

} // namespace Slic3r
