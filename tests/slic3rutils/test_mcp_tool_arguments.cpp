#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPJsonRpcError.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServerInfo.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPToolArguments.hpp"
#include "libslic3r/Model.hpp"

#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// A tools/call's arguments are checked against the tool's published inputSchema before its handler
// runs: an argument (or a key of a nested object whose schema says additionalProperties: false) the
// schema does not declare, a required one left out, and arguments that are not an object are refused
// with JSON-RPC -32602. None of this needs the app: every refusal comes before the handler, and the
// handlers that would run otherwise need the GUI.

using Slic3r::GUI::OrcaMCPServer;
using Slic3r::GUI::OrcaMCP::JsonRpcError;
using Slic3r::GUI::OrcaMCP::tool_arguments_error;
using json = nlohmann::json;

namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The tool's inputSchema as tools/list publishes it, which is the one tools/call checks against.
json listed_schema(const std::string& tool)
{
    return OrcaMCPServer::tool_list_entry(OrcaMCPServer::registered_tools().at(tool)).at("inputSchema");
}

// Why the published schema refuses `arguments` for `tool`; empty when it takes them.
std::string refusal(const std::string& tool, const json& arguments)
{
    return tool_arguments_error(tool, listed_schema(tool), arguments).value_or("");
}

// The JSON-RPC error tools/call refuses the call with, or nothing when it was answered.
std::optional<JsonRpcError> call_refusal(const json& params)
{
    try {
        OrcaMCPServer::handle_tools_call(params);
    } catch (const JsonRpcError& e) {
        return e;
    }
    return std::nullopt;
}

// Every required argument of `tool`, set to null: present, as far as the check is concerned.
json required_placeholders(const OrcaMCPServer::ToolDefinition& tool)
{
    json arguments = json::object();
    for (const json& name : listed_schema(tool.name).value("required", json::array()))
        arguments[name.get<std::string>()] = nullptr;
    return arguments;
}

} // namespace

TEST_CASE("An argument a tool does not take is refused, naming it and every argument the tool takes",
          "[McpToolArguments][orcamcp][tools]")
{
    CHECK(refusal("scale_object", {{"object_id", 0}, {"scale", 0.5}}) ==
          "scale_object has no argument \"scale\". Its arguments: object_id, include_preview, preview_resolution, "
          "preview_views, uniform, x, y, z.");
}

TEST_CASE("A required argument left out is refused, naming it", "[McpToolArguments][orcamcp][tools]")
{
    CHECK(refusal("set_filament_color", {{"slot", 1}}) ==
          "set_filament_color is missing its required argument \"color\". Its arguments: slot, color.");
}

TEST_CASE("A misspelled required argument is refused as both unknown and missing", "[McpToolArguments][orcamcp][tools]")
{
    CHECK(refusal("scale_object", {{"objectid", 0}, {"x", 2}}) ==
          "scale_object has no argument \"objectid\" and is missing its required argument \"object_id\". Its arguments: "
          "object_id, include_preview, preview_resolution, preview_views, uniform, x, y, z.");
}

TEST_CASE("Every unknown and every missing argument is named at once", "[McpToolArguments][orcamcp][tools]")
{
    CHECK(refusal("clone_preset", {{"type", "print"}, {"from", "a"}, {"to", "b"}}) ==
          "clone_preset has no arguments \"from\", \"to\" and is missing its required arguments \"source_name\", "
          "\"new_name\". Its arguments: type, source_name, new_name.");
}

TEST_CASE("A tool that takes no arguments says so", "[McpToolArguments][orcamcp][tools]")
{
    CHECK(refusal("get_slicing_status", {{"plate", 0}}) == "get_slicing_status has no argument \"plate\". It takes no arguments.");
}

TEST_CASE("A call with only the arguments a tool takes passes the check", "[McpToolArguments][orcamcp][tools]")
{
    CHECK("" == refusal("scale_object", {{"object_id", 0}, {"x", 0.5}}));
    CHECK("" == refusal("get_slicing_status", json::object()));
    CHECK("" == refusal("set_object_config", {{"object_id", 0}, {"settings", {{{"key", "wall_loops"}, {"value", 3}}}}}));
}

TEST_CASE("Arguments that are not an object are refused", "[McpToolArguments][orcamcp][tools]")
{
    const auto [arguments, kind] = GENERATE(table<json, std::string>({
        {json::array({1}), "array"}, {json("object_id"), "string"}, {json(3), "number"}, {json(true), "boolean"}}));
    const std::optional<JsonRpcError> refused = call_refusal({{"name", "get_server_info"}, {"arguments", arguments}});
    REQUIRE(refused.has_value());
    CHECK(refused->code == -32602);
    CHECK(std::string(refused->what()) == "get_server_info's arguments must be a JSON object of named arguments; got " + kind + ".");
}

// ==================== NESTED OBJECTS ====================

TEST_CASE("A settings item with a key the tool does not take is refused, naming the item", "[McpToolArguments][orcamcp][tools]")
{
    CHECK(refusal("set_object_config", {{"object_id", 0}, {"settings", {{{"key", "wall_loops"}, {"value", 2}, {"unit", "mm"}}}}}) ==
          "set_object_config: settings[0] has no key \"unit\". Its keys: key, value.");
}

TEST_CASE("A nested item that leaves out a required key is refused, naming its path", "[McpToolArguments][orcamcp][tools]")
{
    CHECK(refusal("set_object_config", {{"configs", {{{"object_id", 0}, {"settings", {{{"key", "wall_loops"}}}}}}}}) ==
          "set_object_config: configs[0].settings[0] is missing its required key \"value\". Its keys: key, value.");
    CHECK(refusal("apply_config", {{"settings", {{{"key", "wall_loops"}, {"value", 2}}}}}) ==
          "apply_config: settings[0] is missing its required key \"type\". Its keys: type, key, value.");
}

TEST_CASE("A key a nested object does not take is refused, however deep", "[McpToolArguments][orcamcp][tools]")
{
    CHECK(refusal("transform_objects", {{"transforms", {{{"object_id", 0}, {"positon", {{"x", 1}}}}}}}) ==
          "transform_objects: transforms[0] has no key \"positon\". Its keys: object_id, position, rotation, scale.");
    CHECK(refusal("transform_objects", {{"transforms", {{{"object_id", 0}, {"position", {{"x", 1}, {"w", 2}}}}}}}) ==
          "transform_objects: transforms[0].position has no key \"w\". Its keys: x, y, z.");
}

TEST_CASE("A misspelled nozzle key is refused rather than sent to the printer as no change", "[McpToolArguments][orcamcp][tools]")
{
    // Only through the check: printer_control's handler talks to a real printer.
    CHECK(refusal("printer_control", {{"action", "set_temperature"}, {"nozzles", {{{"tool", 0}, {"temperature", 200}}}}}) ==
          "printer_control: nozzles[0] has no key \"temperature\" and is missing its required key \"temp\". Its keys: tool, temp.");
}

TEST_CASE("Nested objects whose handlers read only what they declare refuse any other key", "[McpToolArguments][orcamcp][tools]")
{
    const auto [tool, arguments, path] = GENERATE(table<std::string, json, std::string>({
        {"apply_config", {{"settings", {{{"type", "print"}, {"key", "wall_loops"}, {"value", 2}, {"object_id", 0}}}}}, "settings[0]"},
        {"set_brim_ears", {{"object_id", 0}, {"points", {{{"x", 1}, {"y", 2}, {"r", 3}}}}}, "points[0]"},
        {"printer_control", {{"action", "set_temperature"}, {"nozzles", {{{"tool", 0}, {"temp", 200}, {"wait", true}}}}}, "nozzles[0]"},
        {"send_to_printer", {{"material_mappings", {{{"tool_id", 0}, {"slot_id", 1}, {"color", "#FFFFFF"}}}}}, "material_mappings[0]"},
        {"print_printer_file", {{"file_name", "a.gcode"}, {"material_mappings", {{{"tool_id", 0}, {"slot_id", 1}, {"slot", 1}}}}}, "material_mappings[0]"},
        {"paint_object", {{"object_id", 0}, {"selection", "box"}, {"box", {{"min", {0, 0, 0}}, {"max", {1, 1, 1}}, {"center", {0, 0, 0}}}}}, "box"},
        {"paint_object", {{"object_id", 0}, {"selection", "sphere"}, {"sphere", {{"center", {0, 0, 0}}, {"radius", 1}, {"r", 1}}}}, "sphere"},
        {"pick_facet", {{"object_id", 0}, {"ray", {{"origin", {0, 0, 0}}, {"direction", {0, 0, 1}}, {"length", 5}}}}, "ray"}}));
    INFO(tool << " " << arguments.dump());
    const std::string refused = refusal(tool, arguments);
    CHECK(refused.rfind(tool + ": " + path + " has no key ", 0) == 0);
}

TEST_CASE("Nested objects without additionalProperties false take any key", "[McpToolArguments][orcamcp][tools]")
{
    // A render's own camera or a paint call's bands may be passed back with the extra fields its response
    // carries; a paint seed is a point or pick_facet's answer.
    CHECK("" == refusal("render_plate_view", {{"plate_index", 0}, {"views", {{{"preset", "iso"}, {"input_frame", "bed_mm"}}}}}));
    CHECK("" == refusal("paint_object", {{"object_id", 0}, {"selection", "bands"}, {"axis", "z"},
                                         {"bands", {{{"from", 0}, {"to", 1}, {"filament", 1}, {"facet_count", 7}}}}}));
    CHECK("" == refusal("paint_object", {{"object_id", 0}, {"selection", "connected"}, {"seed", {{"facet", 3}, {"normal", {0, 0, 1}}}}}));
}

TEST_CASE("A value of another kind than its schema describes is left to the handler", "[McpToolArguments][orcamcp][tools]")
{
    // overlays is a boolean or an object, and settings may arrive as the JSON text of its list: no type
    // is checked here, and nothing is walked but an object against properties and an array against items.
    CHECK("" == refusal("render_plate_view", {{"plate_index", 0}, {"overlays", true}}));
    CHECK("" == refusal("set_object_config", {{"object_id", 0}, {"settings", R"([{"key": "wall_loops", "value": 3, "unit": 1}])"}}));
}

TEST_CASE("A nested object whose additionalProperties is a schema takes any key", "[McpToolArguments][orcamcp][tools]")
{
    CHECK("" == refusal("remap_paint", {{"object_id", 0}, {"mapping", {{"1", 2}, {"2", 3}}}}));
}

// ==================== WHAT A TOOL RETURNS, SENT BACK ====================
//
// Anything a tool returns in the shape a request takes is accepted back: an agent edits a list by
// sending back the one a response gave it. Where the response is built by a function a test can
// call, the entry here is that function's.

TEST_CASE("The brim ears a response lists are taken back as set_brim_ears' points", "[McpToolArguments][orcamcp][tools]")
{
    // No tool removes one ear: an agent drops it from get_object_paint's brim_ears and sends the rest
    // back with append false.
    Slic3r::Model        model;
    Slic3r::ModelObject* object = model.add_object();
    object->add_instance()->set_offset(Slic3r::Vec3d(100.0, 50.0, 0.0));
    object->brim_points.emplace_back(Slic3r::Vec3f(1.f, 2.f, -0.0001f), 3.f);
    const json ears = Slic3r::GUI::OrcaMCP::brim_ears_json(*object, 0);
    REQUIRE(ears.size() == 1);
    CHECK(refusal("set_brim_ears", {{"object_id", 0}, {"points", ears}, {"append", false}}) == "");
}

TEST_CASE("The material mappings a send reports are taken back by both printer tools", "[McpToolArguments][orcamcp][tools]")
{
    // An upload with start_print false, then print_printer_file with the mapping the upload reported.
    Slic3r::FlashforgeApi::MaterialSlot slot;
    slot.slot_id        = 1;
    slot.has_filament   = true;
    slot.material_name  = "PLA";
    slot.material_color = "#FFFFFF";
    const json project_filaments = json::array({{{"tool_id", 0}, {"type", "PLA"}, {"color", "#FFFFFF"}}});
    json       payload, error, report;
    REQUIRE(Slic3r::GUI::OrcaMCP::resolve_material_mappings(json::array({{{"tool_id", 0}, {"slot_id", 1}}}), {slot},
                                                            project_filaments, payload, error, &report));
    REQUIRE(report.size() == 1);
    CHECK(refusal("send_to_printer", {{"start_print", false}, {"material_mappings", report}}) == "");
    CHECK(refusal("print_printer_file", {{"file_name", "cube.gcode"}, {"material_mappings", report}}) == "");
}

TEST_CASE("The position, rotation and scale an object reports are taken back by transform_objects",
          "[McpToolArguments][orcamcp][tools]")
{
    // get_object_info, get_scene_info and the transform tools report position, rotation_degrees and
    // scale in this shape, built in their handlers.
    const json reported = {{"x", 128.0}, {"y", 128.0}, {"z", 10.0}};
    CHECK(refusal("transform_objects",
                  {{"transforms", {{{"object_id", 0}, {"position", reported}, {"rotation", reported}, {"scale", reported}}}}}) == "");
}

TEST_CASE("A printer status nozzle entry is refused as a nozzles target, not read as no change",
          "[McpToolArguments][orcamcp][tools]")
{
    // get_printer_status reports each nozzle as {tool, current, target}: not the shape nozzles takes,
    // whose target is temp, so sending the status back names what is missing.
    CHECK(refusal("printer_control", {{"action", "set_temperature"}, {"nozzles", {{{"tool", 0}, {"current", 25.0}, {"target", 0.0}}}}}) ==
          "printer_control: nozzles[0] has no keys \"current\", \"target\" and is missing its required key \"temp\". "
          "Its keys: tool, temp.");
}

// ==================== ARGUMENTS THAT WERE DROPPED ====================

TEST_CASE("clone_object no longer reads target_plate, and names destination_plate instead", "[McpToolArguments][orcamcp][tools]")
{
    const std::string refused = refusal("clone_object", {{"object_id", 0}, {"target_plate", 1}});
    CHECK(contains(refused, "has no argument \"target_plate\""));
    CHECK(contains(refused, "destination_plate"));
}

TEST_CASE("save_project no longer takes save_as, so save_as without output_path cannot save in place",
          "[McpToolArguments][orcamcp][tools]")
{
    CHECK(refusal("save_project", {{"save_as", true}}) == "save_project has no argument \"save_as\". Its arguments: output_path.");
}

// ==================== THE WHOLE REGISTRY, THROUGH tools/call ====================

TEST_CASE("Every app tool refuses an argument it does not take, before its handler runs", "[McpToolArguments][orcamcp][tools]")
{
    for (const auto& [name, tool] : OrcaMCPServer::registered_tools()) {
        if (tool.bridge_only)
            continue;
        INFO("tool " << name);
        json arguments                          = required_placeholders(tool);
        arguments["not_an_argument_of_this_tool"] = 1;
        // A handler that ran would need the GUI this test does not have.
        const std::optional<JsonRpcError> refused = call_refusal({{"name", name}, {"arguments", arguments}});
        REQUIRE(refused.has_value());
        CHECK(refused->code == -32602);
        const std::string message = refused->what();
        CHECK(message.rfind(name + " has no argument \"not_an_argument_of_this_tool\". ", 0) == 0);
        const json properties = listed_schema(name).at("properties");
        for (const auto& [argument, schema] : properties.items())
            CHECK(contains(message, argument));
    }
}

TEST_CASE("Every app tool refuses a call that leaves out a required argument, naming it", "[McpToolArguments][orcamcp][tools]")
{
    size_t checked = 0;
    for (const auto& [name, tool] : OrcaMCPServer::registered_tools()) {
        if (tool.bridge_only)
            continue;
        const json required_arguments = listed_schema(name).at("required");
        for (const json& required : required_arguments) {
            const std::string left_out = required.get<std::string>();
            INFO("tool " << name << ", leaving out " << left_out);
            json arguments = required_placeholders(tool);
            arguments.erase(left_out);
            const std::optional<JsonRpcError> refused = call_refusal({{"name", name}, {"arguments", arguments}});
            REQUIRE(refused.has_value());
            CHECK(refused->code == -32602);
            CHECK(contains(refused->what(), "is missing its required argument \"" + left_out + "\""));
            ++checked;
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("get_server_info refuses an extra argument instead of answering", "[McpToolArguments][orcamcp][tools]")
{
    // The one handler that answers without the app, so a refusal here is the check coming first.
    const std::optional<JsonRpcError> refused =
        call_refusal({{"name", "get_server_info"}, {"arguments", {{"section", "all"}, {"verbose", true}}}});
    REQUIRE(refused.has_value());
    CHECK(refused->code == -32602);
    CHECK(contains(refused->what(), "has no argument \"verbose\""));
}

TEST_CASE("A call with only declared arguments reaches the handler", "[McpToolArguments][orcamcp][tools]")
{
    const std::string section = Slic3r::GUI::OrcaMCP::server_info_section_names().front();
    for (const json& params : {json{{"name", "get_server_info"}, {"arguments", {{"section", section}}}},
                               json{{"name", "get_server_info"}, {"arguments", json::object()}},
                               json{{"name", "get_server_info"}, {"arguments", nullptr}}, // null is no arguments
                               json{{"name", "get_server_info"}}}) {
        INFO("params " << params.dump());
        const json result = OrcaMCPServer::handle_tools_call(params);
        CHECK(result.at("content").at(0).at("type") == "text");
    }
}

TEST_CASE("Every required argument and nested required key is a declared property", "[McpToolArguments][orcamcp][tools]")
{
    // A required name the schema does not declare could never be sent without also being refused.
    std::function<void(const json&, const std::string&)> check = [&check](const json& schema, const std::string& path) {
        if (!schema.is_object())
            return;
        const json properties = schema.value("properties", json::object());
        for (const json& required : schema.value("required", json::array())) {
            INFO(path << " requires " << required.dump());
            CHECK(properties.contains(required.get<std::string>()));
        }
        for (const auto& [key, property] : properties.items())
            check(property, path + "." + key);
        if (schema.contains("items"))
            check(schema.at("items"), path + "[]");
    };
    for (const auto& [name, tool] : OrcaMCPServer::registered_tools())
        check(listed_schema(name), name);
}
