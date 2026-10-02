#include "OrcaMCPProtocolVersions.hpp"

#include <algorithm>

#include <boost/algorithm/string/join.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

bool is_supported(const std::string& version)
{
    const auto& versions = supported_protocol_versions();
    return std::find(versions.begin(), versions.end(), version) != versions.end();
}

} // namespace

const std::vector<std::string>& supported_protocol_versions()
{
    static const std::vector<std::string> versions{"2025-06-18", "2024-11-05"};
    return versions;
}

std::string negotiated_protocol_version(const nlohmann::json& initialize_params)
{
    if (initialize_params.is_object()) {
        const auto requested = initialize_params.find("protocolVersion");
        if (requested != initialize_params.end() && requested->is_string() && is_supported(requested->get<std::string>()))
            return requested->get<std::string>();
    }
    return supported_protocol_versions().front();
}

std::optional<std::string> protocol_version_header_refusal(const std::optional<std::string>& header)
{
    if (!header || is_supported(*header))
        return std::nullopt;
    return "OrcaMCP does not speak MCP protocol version \"" + *header + "\" (the MCP-Protocol-Version header). It speaks " +
           boost::algorithm::join(supported_protocol_versions(), ", ") + ": send initialize again to agree on one.";
}

}}} // namespace Slic3r::GUI::OrcaMCP
