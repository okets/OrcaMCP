#ifndef slic3r_OrcaMCPServerInfo_hpp_
#define slic3r_OrcaMCPServerInfo_hpp_

#include "OrcaMCPServer.hpp"

#include <map>
#include <string>
#include <vector>
#include "nlohmann/json.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// get_server_info's content. The tool catalogue -- every tool's name by category, and the
// tool_summaries section with each one's summary -- is generated from the registry on every call, so it
// cannot miss a tool; the documentation sections are hand-written and fetched one at a time.

using ToolMap = std::map<std::string, OrcaMCPServer::ToolDefinition>;

// The server instructions: what initialize answers, the app's and the bridge's (which reads its copy
// from scripts/orcamcp_tools.json). The one text a client shows before any tool is loaded, so it names
// the key tool of each job. At most 2048 characters, which is where Claude Code cuts it off.
const std::string& server_instructions();

// What get_server_info's `section` parameter accepts: "tool_summaries", each documentation section,
// then "all".
const std::vector<std::string>& server_info_section_names();

// get_server_info's response. With no section: the server, quick_start, every tool's name by category,
// and an index of the sections with their sizes. With a section, that section (tool_summaries: every
// tool's one-line summary by category), or everything for "all".
nlohmann::json server_info(const nlohmann::json& params, const ToolMap& tools);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif // slic3r_OrcaMCPServerInfo_hpp_
