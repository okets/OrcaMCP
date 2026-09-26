#ifndef slic3r_OrcaMCPServerInfo_hpp_
#define slic3r_OrcaMCPServerInfo_hpp_

#include "OrcaMCPServer.hpp"

#include <map>
#include <string>
#include <vector>
#include "nlohmann/json.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// get_server_info's content. The tool catalogue is generated from the registry on every call, so it
// cannot miss a tool; the documentation sections are hand-written and fetched one at a time.

using ToolMap = std::map<std::string, OrcaMCPServer::ToolDefinition>;

// What get_server_info's `section` parameter accepts: each documentation section, then "all".
const std::vector<std::string>& server_info_section_names();

// get_server_info's response. With no section: the server, quick_start, every tool's summary by
// category, and an index of the other sections with their sizes. With a section, that section, or
// everything for "all".
nlohmann::json server_info(const nlohmann::json& params, const ToolMap& tools);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif // slic3r_OrcaMCPServerInfo_hpp_
