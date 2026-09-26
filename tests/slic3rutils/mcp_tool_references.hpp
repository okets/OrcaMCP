#ifndef slic3rutils_mcp_tool_references_hpp_
#define slic3rutils_mcp_tool_references_hpp_

// Finds tool names in MCP text that no tool answers to. Written for get_server_info's content;
// the same two entry points take the server instructions and the tool descriptions.
//
// Structured fields are checked strictly. A flow step's "tool" / "toolN", each item of a "tools"
// string ("move_object, rotate_object"), the keys of "tool_examples" and "avoid_heavy_tools", the
// items of "supported_tools" and "bridge_only", and the tool names of the generated catalogue
// ("tools": {category: {name: summary}}) must all be real tools.
//
// Prose is checked by shape, because it also names config keys (support_type) and parameters
// (save_to_file) that look just like tool names. Only string values are read, never object keys,
// which in this content are topic names such as "get_bed_bounds". A token is a candidate when it is
// snake_case and starts with a word some tool's name starts with ("get", "set", "load", ...). A
// candidate that is a tool passes; one that is a config key or any tool's parameter name is not a
// tool reference and is skipped; anything else is reported. Single-word tools (undo, redo) have no
// shape to find in prose, so only the structured fields check them.

#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace mcp_tool_references {

using ToolMap = std::map<std::string, Slic3r::GUI::OrcaMCPServer::ToolDefinition>;

struct Reference
{
    std::string where; // path of the string it was found in, e.g. /suggested_flows/printer_workflow/steps/tool4
    std::string token;
};

class ToolNames
{
public:
    explicit ToolNames(const ToolMap& tools)
    {
        for (const auto& [name, tool] : tools) {
            m_tools.insert(name);
            m_first_words.insert(first_word(name));
            collect_parameters(tool.input_schema);
        }
    }

    bool is_tool(const std::string& token) const { return m_tools.count(token) != 0; }

    // Shaped like a tool name: snake_case, starting with a word some tool name starts with.
    bool looks_like_tool(const std::string& token) const
    {
        static const std::regex snake_case("[a-z][a-z0-9]*(_[a-z0-9]+)+");
        return std::regex_match(token, snake_case) && m_first_words.count(first_word(token)) != 0;
    }

    // Shaped like a tool name, but a config key or a tool parameter instead.
    bool is_other_name(const std::string& token) const
    {
        return m_parameters.count(token) != 0 || Slic3r::print_config_def.has(token);
    }

private:
    static std::string first_word(const std::string& name) { return name.substr(0, name.find('_')); }

    void collect_parameters(const nlohmann::json& schema)
    {
        if (!schema.is_object())
            return;
        if (auto properties = schema.find("properties"); properties != schema.end() && properties->is_object()) {
            for (auto it = properties->begin(); it != properties->end(); ++it) {
                m_parameters.insert(it.key());
                collect_parameters(it.value());
            }
        }
        if (auto items = schema.find("items"); items != schema.end())
            collect_parameters(*items);
    }

    std::set<std::string> m_tools;
    std::set<std::string> m_first_words;
    std::set<std::string> m_parameters;
};

// Tool-shaped tokens in free text that are neither tools nor other known names.
inline std::vector<Reference> unknown_in_text(const std::string& text, const std::string& where, const ToolNames& names)
{
    static const std::regex word("[A-Za-z0-9_]+");
    std::vector<Reference> unknown;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), word); it != std::sregex_iterator(); ++it) {
        const std::string token = it->str();
        if (names.looks_like_tool(token) && !names.is_tool(token) && !names.is_other_name(token))
            unknown.push_back({where, token});
    }
    return unknown;
}

namespace detail {

inline void require_tool(const std::string& name, const std::string& where, const ToolNames& names, std::vector<Reference>& unknown)
{
    if (!names.is_tool(name))
        unknown.push_back({where, name});
}

inline bool is_step_tool_key(const std::string& key) { return std::regex_match(key, std::regex("tool[0-9]*")); }

// The fields whose content is, by construction, a tool name.
inline void check_structured(const std::string& key, const nlohmann::json& value, const std::string& where, const ToolNames& names,
                             std::vector<Reference>& unknown)
{
    if (is_step_tool_key(key) && value.is_string()) {
        require_tool(value.get<std::string>(), where, names, unknown);
    } else if (key == "tools" && value.is_string()) {
        static const std::regex item("[^,\\s]+");
        const std::string list = value.get<std::string>();
        for (auto it = std::sregex_iterator(list.begin(), list.end(), item); it != std::sregex_iterator(); ++it)
            require_tool(it->str(), where, names, unknown);
    } else if (key == "tools" && value.is_object()) {
        for (auto category = value.begin(); category != value.end(); ++category)
            if (category->is_object())
                for (auto tool = category->begin(); tool != category->end(); ++tool)
                    require_tool(tool.key(), where + "/" + category.key(), names, unknown);
    } else if ((key == "tool_examples" || key == "avoid_heavy_tools") && value.is_object()) {
        for (auto tool = value.begin(); tool != value.end(); ++tool)
            require_tool(tool.key(), where, names, unknown);
    } else if ((key == "supported_tools" || key == "bridge_only") && value.is_array()) {
        for (const auto& tool : value)
            if (tool.is_string())
                require_tool(tool.get<std::string>(), where, names, unknown);
    }
}

inline void walk(const nlohmann::json& node, const std::string& where, const ToolNames& names, std::vector<Reference>& unknown)
{
    if (node.is_string()) {
        for (Reference& reference : unknown_in_text(node.get<std::string>(), where, names))
            unknown.push_back(std::move(reference));
    } else if (node.is_array()) {
        for (size_t i = 0; i < node.size(); ++i)
            walk(node[i], where + "/" + std::to_string(i), names, unknown);
    } else if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string path = where + "/" + it.key();
            check_structured(it.key(), it.value(), path, names, unknown);
            walk(it.value(), path, names, unknown);
        }
    }
}

} // namespace detail

// Every tool reference in a JSON document, structured fields and prose, that no tool answers to.
inline std::vector<Reference> unknown_in_json(const nlohmann::json& content, const ToolNames& names)
{
    std::vector<Reference> unknown;
    detail::walk(content, "", names, unknown);
    return unknown;
}

inline std::string describe(const std::vector<Reference>& references)
{
    std::string out;
    for (const Reference& reference : references)
        out += "  " + reference.token + " at " + reference.where + "\n";
    return out;
}

} // namespace mcp_tool_references

#endif // slic3rutils_mcp_tool_references_hpp_
