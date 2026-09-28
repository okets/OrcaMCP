#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServerInfo.hpp"
#include "libslic3r_version.h"
#include "mcp_tool_references.hpp"

#include <algorithm>

#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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
        // An app tool has a handler; a bridge-only one has none, since the app never runs it.
        CHECK(static_cast<bool>(tool.handler) != tool.bridge_only);
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

// ==================== GET_SERVER_INFO ====================
//
// Its catalogue is built from the registry on every call, so it names every tool; the rest of its
// documentation is fetched one section at a time, so the default stays small enough to call.

namespace {

nlohmann::json get_server_info(const nlohmann::json& params = nlohmann::json::object())
{
    return OrcaMCPServer::registered_tools().at("get_server_info").handler(params);
}

// name -> category, as the catalogue lists them.
std::map<std::string, std::string> catalogue_entries(const nlohmann::json& catalogue)
{
    std::map<std::string, std::string> entries;
    for (auto category = catalogue.begin(); category != catalogue.end(); ++category)
        for (auto tool = category->begin(); tool != category->end(); ++tool)
            entries[tool.key()] = category.key();
    return entries;
}

} // namespace

TEST_CASE("get_server_info's catalogue names every tool once, under its category, with its summary", "[orcamcp][tools]")
{
    const nlohmann::json info    = get_server_info();
    const auto&          tools   = OrcaMCPServer::registered_tools();
    const auto           entries = catalogue_entries(info.at("tools"));

    size_t listed = 0;
    for (auto category = info.at("tools").begin(); category != info.at("tools").end(); ++category)
        listed += category->size();
    CHECK(listed == tools.size());

    for (const auto& [name, tool] : tools) {
        INFO("tool " << name);
        REQUIRE(entries.count(name) == 1);
        CHECK(entries.at(name) == OrcaMCPServer::tool_category_name(tool.category));
        CHECK(info.at("tools").at(entries.at(name)).at(name) == tool.summary);
    }
}

TEST_CASE("get_server_info's default response stays under 6 KB", "[orcamcp][tools]")
{
    // An agent that has to spend 6k tokens to learn what exists stops asking. Each new tool adds
    // about 55 bytes; when this fails, shorten summaries or move content into a section.
    const std::string response = get_server_info().dump();
    INFO("default response is " << response.size() << " bytes");
    CHECK(response.size() <= 6 * 1024);
}

TEST_CASE("get_server_info reports the build's version", "[orcamcp][tools]")
{
    CHECK(OrcaMCPServer::version() == SoftFever_VERSION);
    CHECK(get_server_info().at("server").at("version") == SoftFever_VERSION);
}

TEST_CASE("get_server_info's section index lists every section with the size it will fetch", "[orcamcp][tools]")
{
    const nlohmann::json index = get_server_info().at("sections");
    REQUIRE(index.size() >= 5);

    std::vector<std::string> expected_enum;
    for (auto entry = index.begin(); entry != index.end(); ++entry) {
        INFO("section " << entry.key());
        const nlohmann::json section = get_server_info({{"section", entry.key()}});
        REQUIRE(section.size() == 1);
        REQUIRE(section.contains(entry.key()));
        CHECK(section.at(entry.key()).dump().size() == entry.value().get<size_t>());
        expected_enum.push_back(entry.key());
    }

    // The schema offers exactly those sections, plus "all".
    const nlohmann::json schema_enum =
        OrcaMCPServer::registered_tools().at("get_server_info").input_schema.at("properties").at("section").at("enum");
    std::set<std::string> offered(schema_enum.begin(), schema_enum.end());
    std::set<std::string> indexed(expected_enum.begin(), expected_enum.end());
    indexed.insert("all");
    CHECK(offered == indexed);
}

TEST_CASE("get_server_info section=all returns the default content and every section", "[orcamcp][tools]")
{
    const nlohmann::json everything = get_server_info({{"section", "all"}});
    const nlohmann::json summary    = get_server_info();
    CHECK(everything.at("tools") == summary.at("tools"));
    CHECK(everything.at("quick_start") == summary.at("quick_start"));
    for (auto entry = summary.at("sections").begin(); entry != summary.at("sections").end(); ++entry) {
        INFO("section " << entry.key());
        CHECK(everything.contains(entry.key()));
    }
}

TEST_CASE("get_server_info rejects an unknown section and names the valid ones", "[orcamcp][tools]")
{
    for (const nlohmann::json& bad : {nlohmann::json("tools_by_category"), nlohmann::json(3)}) {
        INFO("section " << bad.dump());
        const nlohmann::json response = get_server_info({{"section", bad}});
        CHECK(response.value("status", "") == "error");
        const std::string message = response.value("message", "");
        CHECK(message.find("concepts") != std::string::npos);
        CHECK(message.find("all") != std::string::npos);
    }
}

// ==================== TOOL NAMES IN THE DOCUMENTATION ====================

TEST_CASE("Every tool get_server_info mentions is a real tool", "[orcamcp][tools]")
{
    const mcp_tool_references::ToolNames names(OrcaMCPServer::registered_tools());
    const auto unknown = mcp_tool_references::unknown_in_json(get_server_info({{"section", "all"}}), names);
    INFO("get_server_info names tools that do not exist:\n" << mcp_tool_references::describe(unknown));
    CHECK(unknown.empty());
}

namespace {

// Response fields and status words tool descriptions name that look like tool names ("slice_run",
// "arrange_started"). A response has no schema to read them from, so they are listed here.
const std::set<std::string> response_words_named_in_descriptions{"slice_run", "reset_count", "arrange_started", "export_started"};

} // namespace

TEST_CASE("Every tool name a tool's own text mentions is a real tool", "[orcamcp][tools]")
{
    // An agent reads a description only once it has chosen the tool, and follows what it names: a
    // name no tool answers to sends it nowhere.
    const mcp_tool_references::ToolNames names(OrcaMCPServer::registered_tools(), response_words_named_in_descriptions);
    std::vector<mcp_tool_references::Reference> unknown;
    for (const auto& [name, tool] : OrcaMCPServer::registered_tools()) {
        const nlohmann::json text = {{"summary", tool.summary}, {"description", tool.description}, {"inputSchema", tool.input_schema}};
        for (mcp_tool_references::Reference& reference : mcp_tool_references::unknown_in_json(text, names))
            unknown.push_back({name + reference.where, reference.token});
    }
    INFO("tool text names tools that do not exist:\n" << mcp_tool_references::describe(unknown));
    CHECK(unknown.empty());
}

TEST_CASE("The tool-name check takes a schema's enum values for what they are", "[orcamcp][tools]")
{
    // printer_control's action "set_temperature" is a value to send, not a tool to call.
    const mcp_tool_references::ToolNames names(OrcaMCPServer::registered_tools());
    CHECK(names.is_other_name("set_temperature"));
    CHECK(mcp_tool_references::unknown_in_text("send action set_temperature, then set_temperatures", "text", names).size() == 1);
}

TEST_CASE("The tool-name check tells tool references from config keys and parameters", "[orcamcp][tools]")
{
    const mcp_tool_references::ToolNames names(OrcaMCPServer::registered_tools());
    const nlohmann::json content = {
        {"a_flow", {
            {"tool", "get_scene_infos"},                        // a step naming a tool that does not exist
            {"tools", "move_object, rotate_objects"},           // one real, one not
            {"note", "Call get_scene_info, then set support_type and enable_support, keep "
                     "print_sequence, pass save_to_file=true and start_print=false, then load_modle."}
        }},
        {"tool_examples", {{"load_models", {{"tip", "undo when unsure"}}}}},
        {"get_bed_bounds", "An object key is a topic name, not a tool reference: never read."}
    };

    std::set<std::string> found;
    for (const auto& reference : mcp_tool_references::unknown_in_json(content, names))
        found.insert(reference.token);
    CHECK(found == std::set<std::string>{"get_scene_infos", "rotate_objects", "load_modle", "load_models"});
}

TEST_CASE("The tool-name check also reads plain text", "[orcamcp][tools]")
{
    const mcp_tool_references::ToolNames names(OrcaMCPServer::registered_tools());
    const auto unknown = mcp_tool_references::unknown_in_text("Use render_plate_view, then export_gcodes.", "text", names);
    REQUIRE(unknown.size() == 1);
    CHECK(unknown.front().token == "export_gcodes");
    CHECK(unknown.front().where == "text");
}

// ==================== SERVER INSTRUCTIONS ====================
//
// The one text a client shows an agent before it has loaded any tool: Claude Code loads tool schemas
// only once an agent picks a tool by name, so the instructions are where the names it should pick are.

TEST_CASE("initialize carries the server instructions", "[orcamcp][tools]")
{
    const nlohmann::json result = OrcaMCPServer::handle_initialize(nlohmann::json::object());
    REQUIRE(result.contains("instructions"));
    CHECK(result.at("instructions") == Slic3r::GUI::OrcaMCP::server_instructions());
}

TEST_CASE("The server instructions fit the 2048 characters Claude Code shows, in plain ASCII", "[orcamcp][tools]")
{
    // Claude Code cuts a server's instructions after 2048 characters ("... [truncated]"), measured on
    // another server's on 2026-09-28. ASCII keeps characters and bytes the same count.
    const std::string& instructions = Slic3r::GUI::OrcaMCP::server_instructions();
    INFO("the instructions are " << instructions.size() << " characters");
    CHECK_FALSE(instructions.empty());
    CHECK(instructions.size() <= 2048);
    CHECK(std::all_of(instructions.begin(), instructions.end(), [](char c) { return c == '\n' || (c >= 0x20 && c < 0x7f); }));
}

TEST_CASE("Every tool the server instructions name is a real tool", "[orcamcp][tools]")
{
    const mcp_tool_references::ToolNames names(OrcaMCPServer::registered_tools());
    const auto unknown = mcp_tool_references::unknown_in_text(Slic3r::GUI::OrcaMCP::server_instructions(), "instructions", names);
    INFO("the instructions name tools that do not exist:\n" << mcp_tool_references::describe(unknown));
    CHECK(unknown.empty());
}

TEST_CASE("The server instructions name the tools agents did not find without them", "[orcamcp][tools]")
{
    // The 2026-09-26 session: mesh errors diagnosed in another program, support painted there, a slice
    // polled 27 times, toolpaths "not rendered", get_server_info called only after the user pushed back.
    const std::string& instructions = Slic3r::GUI::OrcaMCP::server_instructions();
    for (const char* tool : {"get_mesh_health", "get_object_components", "paint_object", "wait_for_slice", "render_plate_view",
                             "get_server_info"}) {
        INFO("tool " << tool);
        CHECK(instructions.find(tool) != std::string::npos);
    }
}

// ==================== BRIDGE-ONLY TOOLS ====================
//
// start_orca launches the app, so the app cannot serve it, and wait_for_slice waits on it, which the
// app's one request thread cannot do without stalling every other call; orcamcp-bridge.py answers
// both. Their text still lives in the registry, with every other tool's, so it exists in one place.

TEST_CASE("start_orca and wait_for_slice are registered as bridge-only tools", "[orcamcp][tools]")
{
    const auto& tools = OrcaMCPServer::registered_tools();
    REQUIRE(tools.count("start_orca") == 1);
    REQUIRE(tools.count("wait_for_slice") == 1);
    CHECK(tools.at("start_orca").bridge_only);
    CHECK(tools.at("start_orca").category == ToolCategory::Info);
    CHECK(tools.at("wait_for_slice").bridge_only);
    CHECK(tools.at("wait_for_slice").category == ToolCategory::Slicing);

    std::set<std::string> bridge_only;
    for (const auto& [name, tool] : tools)
        if (tool.bridge_only)
            bridge_only.insert(name);
    CHECK(bridge_only == std::set<std::string>{"start_orca", "wait_for_slice"});
}

TEST_CASE("tools/list leaves bridge-only tools to the bridge", "[orcamcp][tools]")
{
    const nlohmann::json  tools_list = OrcaMCPServer::handle_tools_list();
    std::set<std::string> listed;
    for (const auto& tool : tools_list.at("tools"))
        listed.insert(tool.at("name").get<std::string>());

    for (const auto& [name, tool] : OrcaMCPServer::registered_tools()) {
        INFO("tool " << name);
        CHECK(listed.count(name) == (tool.bridge_only ? 0 : 1));
    }
}

TEST_CASE("A bridge-only tool called on the app is a JSON-RPC error that points at the bridge", "[orcamcp][tools]")
{
    try {
        OrcaMCPServer::handle_tools_call({{"name", "start_orca"}, {"arguments", nlohmann::json::object()}});
        FAIL("start_orca was answered by the app");
    } catch (const Slic3r::GUI::OrcaMCP::JsonRpcError& e) {
        CHECK(e.code == -32602);
        CHECK(std::string(e.what()).find("bridge") != std::string::npos);
    }
}

TEST_CASE("A call to an unknown tool is a JSON-RPC invalid-params error, not an internal one", "[orcamcp][tools]")
{
    for (const nlohmann::json& params : {nlohmann::json{{"name", "no_such_tool"}}, nlohmann::json::object()}) {
        INFO("params " << params.dump());
        try {
            OrcaMCPServer::handle_tools_call(params);
            FAIL("the call was answered");
        } catch (const Slic3r::GUI::OrcaMCP::JsonRpcError& e) {
            CHECK(e.code == -32602);
        }
    }
}

TEST_CASE("get_server_info names the bridge-only tools", "[orcamcp][tools]")
{
    const nlohmann::json bridge_tools = nlohmann::json::array({"start_orca", "wait_for_slice"});
    CHECK(get_server_info().at("bridge_only") == bridge_tools);
    CHECK(get_server_info({{"section", "all"}}).at("bridge_only") == bridge_tools);
}

// ==================== THE GOLDEN TOOLS FILE ====================
//
// scripts/orcamcp_tools.json is what the bridge serves while the app is not running, and where it
// reads its own tools' text from. It must say exactly what the registry says, or an agent sees one
// tool list before the app starts and another after.

namespace {

const char* const regenerate_variable = "ORCAMCP_UPDATE_TOOLS_GOLDEN";

bool regeneration_requested()
{
    const char* value = std::getenv(regenerate_variable);
    return value != nullptr && std::string(value) != "" && std::string(value) != "0";
}

std::string regenerate_hint()
{
    return std::string("Regenerate it after building slic3rutils_tests:\n  ") + regenerate_variable +
           "=1 slic3rutils_tests \"[orcamcp][tools]\"\nthen commit scripts/orcamcp_tools.json.";
}

std::map<std::string, nlohmann::json> by_name(const nlohmann::json& tools)
{
    std::map<std::string, nlohmann::json> out;
    if (tools.is_array())
        for (const auto& tool : tools)
            out[tool.value("name", "")] = tool;
    return out;
}

// One line per disagreement, naming the tool and the field.
std::vector<std::string> manifest_differences(const nlohmann::json& file, const nlohmann::json& registry)
{
    std::vector<std::string> differences;
    for (const char* list : {"server_tools", "bridge_tools"}) {
        const auto in_file     = by_name(file.value(list, nlohmann::json::array()));
        const auto in_registry = by_name(registry.at(list));
        for (const auto& [name, tool] : in_registry) {
            const auto found = in_file.find(name);
            if (found == in_file.end()) {
                differences.push_back(std::string(list) + ": " + name + " is registered but not in the file");
                continue;
            }
            for (const char* field : {"category", "summary", "description", "inputSchema"})
                if (found->second.value(field, nlohmann::json()) != tool.at(field))
                    differences.push_back(std::string(list) + ": " + name + "'s " + field + " differs");
        }
        for (const auto& [name, tool] : in_file)
            if (in_registry.count(name) == 0)
                differences.push_back(std::string(list) + ": " + name + " is in the file but not registered");
    }
    // The bridge answers initialize itself, with the file's copy of the server instructions.
    if (file.value("instructions", nlohmann::json()) != registry.at("instructions"))
        differences.push_back("instructions differ");
    return differences;
}

std::string joined(const std::vector<std::string>& lines)
{
    std::string out;
    for (const std::string& line : lines)
        out += "  " + line + "\n";
    return out;
}

} // namespace

TEST_CASE("scripts/orcamcp_tools.json matches the tool registry", "[orcamcp][tools]")
{
    const nlohmann::json registry = OrcaMCPServer::tools_manifest();
    const std::string    path     = ORCAMCP_TOOLS_GOLDEN_FILE;

    if (regeneration_requested()) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << registry.dump(2) << "\n";
        REQUIRE(out.good());
        WARN("Rewrote " << path << " from the registry");
    }

    std::ifstream in(path, std::ios::binary);
    INFO("Reading " << path);
    REQUIRE(in.good());
    const nlohmann::json file = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(file.is_discarded());

    const std::vector<std::string> differences = manifest_differences(file, registry);
    INFO(path << " is out of date:\n" << joined(differences) << regenerate_hint());
    CHECK(differences.empty());
    CHECK(file.value("generated_from", "") == registry.at("generated_from").get<std::string>());
}

TEST_CASE("The golden file lists app tools and bridge-only tools apart", "[orcamcp][tools]")
{
    const nlohmann::json manifest = OrcaMCPServer::tools_manifest();
    std::set<std::string> server_names, bridge_names;
    for (const auto& tool : manifest.at("server_tools"))
        server_names.insert(tool.at("name").get<std::string>());
    for (const auto& tool : manifest.at("bridge_tools"))
        bridge_names.insert(tool.at("name").get<std::string>());

    CHECK(server_names.count("get_scene_info") == 1);
    CHECK(bridge_names == std::set<std::string>{"start_orca", "wait_for_slice"});
    for (const std::string& name : bridge_names)
        CHECK(server_names.count(name) == 0);
    CHECK(server_names.size() + bridge_names.size() == OrcaMCPServer::registered_tools().size());
}

// ==================== START-UP ====================
//
// A tool table that fails to build is a programming error a rebuild fixes, not a retry. So the
// server's start-up runs once: its failure is remembered and every later request gets the same
// reason at once, instead of redoing the preview cleanup and the whole registration each time.

TEST_CASE("Start-up work that succeeds runs once", "[orcamcp][tools]")
{
    Slic3r::GUI::RunOnce once;
    int                  calls = 0;
    CHECK(once.run([&] { ++calls; }).empty());
    CHECK(once.run([&] { ++calls; }).empty());
    CHECK(calls == 1);
}

TEST_CASE("Start-up work that fails is not retried, and its reason is kept", "[orcamcp][tools]")
{
    Slic3r::GUI::RunOnce once;
    int                  calls = 0;
    const std::string    first = once.run([&] {
        ++calls;
        throw std::logic_error("OrcaMCPServer: tool 'undo' is registered twice");
    });
    CHECK(first == "OrcaMCPServer: tool 'undo' is registered twice");
    CHECK(once.run([&] { ++calls; }) == first);
    CHECK(calls == 1);
}

TEST_CASE("Start-up work that throws something other than a std::exception is caught too", "[orcamcp][tools]")
{
    Slic3r::GUI::RunOnce once;
    const std::string    failure = once.run([] { throw 42; });
    CHECK_FALSE(failure.empty());
    CHECK(once.run([] {}) == failure);
}
