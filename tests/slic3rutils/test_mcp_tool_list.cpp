#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"

#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// The MCP tool registry is the one source of every tool's text. None of this needs the app:
// registering a tool only builds its JSON schema and its handler, and no handler is called here
// that touches the GUI.

using Slic3r::GUI::OrcaMCPServer;
using ToolCategory   = OrcaMCPServer::ToolCategory;
using ToolDefinition = OrcaMCPServer::ToolDefinition;
using ToolHandler    = OrcaMCPServer::ToolHandler;

namespace {

// True when T can be brace-initialised from Args, the way every register_tool({...}) call is.
template<class T, class... Args>
auto brace_initialisable(int) -> decltype(T{std::declval<Args>()...}, std::true_type{});
template<class T, class... Args>
std::false_type brace_initialisable(...);
template<class T, class... Args>
constexpr bool is_brace_initialisable = decltype(brace_initialisable<T, Args...>(0))::value;

} // namespace

// A registration without a category must not compile: the category sits straight after the name,
// so the old {name, description, schema, handler} form puts a string where the enum goes. If the
// field ever moves to the end, it would silently default to the first category instead.
static_assert(!is_brace_initialisable<ToolDefinition, const char*, const char*, nlohmann::json, ToolHandler>,
              "a tool registration without a category must not compile");
static_assert(is_brace_initialisable<ToolDefinition, const char*, ToolCategory, const char*, const char*, nlohmann::json, ToolHandler>,
              "the full registration form must compile");

TEST_CASE("Every tool has a category, a one-line summary and a description", "[orcamcp][tools]")
{
    const auto& tools = OrcaMCPServer::registered_tools();
    REQUIRE(tools.size() >= 79);

    for (const auto& [name, tool] : tools) {
        INFO("tool " << name << ", summary \"" << tool.summary << "\"");
        CHECK(tool.name == name);
        CHECK_FALSE(tool.summary.empty());
        CHECK(tool.summary.size() <= OrcaMCPServer::max_summary_length);
        CHECK(tool.summary.find('\n') == std::string::npos);
        CHECK_FALSE(tool.description.empty());
        CHECK(tool.input_schema.value("type", "") == "object");
        CHECK(tool.handler);
    }
}

TEST_CASE("Every tool category has a name and at least one tool", "[orcamcp][tools]")
{
    std::set<ToolCategory> used;
    for (const auto& [name, tool] : OrcaMCPServer::registered_tools())
        used.insert(tool.category);

    std::set<std::string> names;
    for (ToolCategory category : OrcaMCPServer::all_tool_categories()) {
        const std::string category_name = OrcaMCPServer::tool_category_name(category);
        INFO("category " << category_name);
        CHECK_FALSE(category_name.empty());
        CHECK(used.count(category) == 1);
        names.insert(category_name);
    }
    CHECK(names.size() == OrcaMCPServer::all_tool_categories().size());
    CHECK(used.size() == OrcaMCPServer::all_tool_categories().size());
}

TEST_CASE("A tool whose name is already registered is refused, not silently swapped in", "[orcamcp][tools]")
{
    const auto&          tools    = OrcaMCPServer::registered_tools();
    const ToolDefinition original = tools.at("undo");

    ToolDefinition impostor = original;
    impostor.description    = "a second undo";
    CHECK_THROWS_AS(OrcaMCPServer::register_tool(impostor), std::logic_error);
    CHECK(tools.at("undo").description == original.description);
}

TEST_CASE("A tool without a handler is refused", "[orcamcp][tools]")
{
    ToolDefinition no_handler{"test_tool_without_handler", ToolCategory::Info, "A test tool", "A test tool with no handler.",
                              {{"type", "object"}, {"properties", nlohmann::json::object()}}, {}};
    CHECK_THROWS_AS(OrcaMCPServer::register_tool(no_handler), std::logic_error);
    CHECK(OrcaMCPServer::registered_tools().count("test_tool_without_handler") == 0);
}
