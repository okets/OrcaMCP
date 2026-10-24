#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPJsonRpcError.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateSettings.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServerInfo.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPToolArguments.hpp"
#include "libslic3r/Model.hpp"

#include <boost/thread.hpp>

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
          "preview_views, uniform, volume_id, x, y, z.");
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
          "object_id, include_preview, preview_resolution, preview_views, uniform, volume_id, x, y, z.");
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
    CHECK("" == refusal("render_plate_view", {{"plate_index", 0}, {"views", {{{"preset", "iso"}, {"camera", {{"type", "perspective"}, {"input_frame", "bed_mm"}}}}}}}));
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

TEST_CASE("A texture layer a response reports is taken back by set_texture_displacement", "[McpToolArguments][orcamcp][tools]")
{
    // The request's own fields of layer_json (view_axes among them), sent back as an edit.
    using namespace Slic3r;
    using namespace Slic3r::GUI::OrcaMCP;
    TextureDisplacementLayer layer;
    layer.slot                   = 2;
    layer.projection_method      = TextureProjectionMethod::ViewProjected;
    layer.view_project_projective = true;
    layer.view_project_right     = Vec3f(0, 1, 0);
    layer.view_project_up        = Vec3f(0, 0, 1);
    layer.offset                 = Vec2f(0.25f, 0.5f);
    const json reported = layer_json(layer, 0, true, LayerTexture{}, std::nullopt, {}, false, {layer}, TextureDisplacementOptions{},
                                     Transform3d::Identity());
    json arguments = json::object();
    for (const auto& [key, value] : reported.items())
        if (listed_schema("set_texture_displacement").at("properties").contains(key))
            arguments[key] = value;
    REQUIRE(arguments.contains("view_axes"));
    arguments["object_id"] = 0;
    arguments["action"]    = "edit";
    CHECK(refusal("set_texture_displacement", arguments) == "");
    json bad_axes = {{"object_id", 0}, {"action", "edit"}, {"layer", 0}};
    bad_axes["view_axes"]         = json::object();
    bad_axes["view_axes"]["forward"] = json::array({0, 1, 0});
    CHECK(::contains(refusal("set_texture_displacement", bad_axes), "has no key \"forward\""));
    json cleared = {{"object_id", 0}, {"action", "edit"}, {"layer", 0}};
    cleared["projection_frame"] = false;
    CHECK(refusal("set_texture_displacement", cleared) == "");
}

TEST_CASE("The stack options get_texture_displacement reports are taken back by bake_texture_displacement", "[McpToolArguments][orcamcp][tools]")
{
    using namespace Slic3r;
    using namespace Slic3r::GUI::OrcaMCP;
    for (const bool automatic : {false, true}) {
        TextureDisplacementOptions options;
        options.pipeline_v2        = automatic;
        options.v2_refine_mm       = automatic ? 0.f : 0.3f;
        options.v2_max_triangles_k = automatic ? -1 : 200;
        json arguments             = stack_options_json(options);
        arguments["object_id"]     = 0;
        CHECK(refusal("bake_texture_displacement", arguments) == "");
        std::string error;
        CHECK(read_bake_request(arguments, error).has_value());
    }
    json bad = {{"object_id", 0}, {"smooth", {{"width", 1}}}};
    CHECK(::contains(refusal("bake_texture_displacement", bad), "has no key \"width\""));
    CHECK(::contains(refusal("prepare_texture_mesh", {{"object_id", 0}, {"operation", "remesh"}, {"radius", 1}}), "radius"));
    CHECK(::contains(refusal("prepare_texture_mesh", {{"object_id", 0}}), "operation"));
}

TEST_CASE("An unwrap get_texture_unwrap reports is taken back by set_texture_unwrap", "[McpToolArguments][orcamcp][tools]")
{
    using namespace Slic3r;
    using namespace Slic3r::GUI::OrcaMCP;
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    TextureDisplacementLayer   layer;
    layer.slot                   = 1;
    layer.projection_method      = TextureProjectionMethod::LSCM;
    layer.lscm_uv_overrides.emplace_back(lscm_uv_override_key(2), Vec2f(0.5f, 1.5f));
    const PatchUnwrap unwrap   = layer_unwrap(cube, layer);
    const json        reported = unwrap_json(unwrap, layer, true);
    REQUIRE(reported["islands"].size() > 1);
    REQUIRE(reported["uv_edits"].size() == 1);

    // Every request field the answer carries: islands (with their extra fields), vertex edits, the seams as seam pairs.
    json arguments = {{"object_id", 0}, {"layer", 1}};
    for (const char* key : {"islands", "uv_edits", "seam_angle_deg", "auto_connect_islands"})
        arguments[key] = reported[key];
    arguments["add_seams"] = json::array({json::array({0, 1})});
    CHECK(refusal("set_texture_unwrap", arguments) == "");
    std::string error;
    CHECK(read_unwrap_request(arguments, error).has_value());

    json bad = {{"object_id", 0}, {"layer", 1}, {"islands", json::array({{{"chart", 0}, {"size", 2}}})}};
    CHECK(::contains(refusal("set_texture_unwrap", bad), "has no key \"size\""));
    json lonely = {{"object_id", 0}};
    CHECK(::contains(refusal("set_texture_unwrap", lonely), "layer"));
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

TEST_CASE("The settings a plate reports are taken back by set_plate_settings", "[McpToolArguments][orcamcp][tools]")
{
    // get_scene_info and set_plate_settings report a plate's settings in set_plate_settings' own shape.
    Slic3r::GUI::OrcaMCP::PlateSettings settings;
    settings.name               = "Left";
    settings.first_layer_order  = {2, 1};
    settings.other_layers_order = {{{2, 9}, {1, 2}}, {{10, Slic3r::GUI::OrcaMCP::k_last_layer}, {2, 1}}};
    json arguments              = Slic3r::GUI::OrcaMCP::plate_settings_json(settings);
    arguments["plate_index"]    = 0;
    CHECK(refusal("set_plate_settings", arguments) == "");
    CHECK(contains(refusal("set_plate_settings", {{"other_layers_filament_order", {{{"from_layer", 2}, {"layers", 3}, {"order", {1}}}}}}),
                   "has no key \"layers\""));
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

// ==================== HOW DEEP THE WALK GOES ====================

TEST_CASE("An array whose schema gives no items is not looked into, however deep it is", "[McpToolArguments][orcamcp][tools]")
{
    // The check runs on the HTTP server's thread, whose stack is small; a walk that followed the value
    // down would let a call's nesting decide how deep it recurses. So it runs here on a small stack.
    const json schema = {{"type", "object"}, {"properties", {{"list", {{"type", "array"}}}}}, {"additionalProperties", false}};
    json       deep   = json::array();
    for (int i = 0; i < 100000; ++i) {
        json outer = json::array();
        outer.push_back(std::move(deep));
        deep = std::move(outer);
    }
    json arguments    = json::object();
    arguments["list"] = std::move(deep);

    std::optional<std::string>  refused = std::string("not run");
    boost::thread::attributes   small_stack;
    small_stack.set_stack_size(256 * 1024);
    boost::thread walk(small_stack, [&]() { refused = tool_arguments_error("tool", schema, arguments); });
    walk.join();
    CHECK(refused.value_or("") == "");
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

TEST_CASE("render_plate_view views are strict, and a camera and a fit a render returned are taken back", "[McpToolArguments][orcamcp][tools]")
{
    // A view with a key the schema does not declare (fov, zoom, distance) is refused, not ignored.
    const json fov = {{"plate_index", 0}, {"views", json::array({{{"preset", "iso"}, {"fov", 30}}})}};
    CHECK(::contains(refusal("render_plate_view", fov), "has no key \"fov\""));

    // What a render returns in a view's shape goes back in: the fit it echoed (object_id or object_index) and the
    // camera object (view_matrix, projection_matrix, viewport and the informational keys).
    const json camera = {{"view_matrix", json::array({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1})},
                         {"projection_matrix", json::array({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1})},
                         {"viewport", json::array({0, 0, 512, 512})},
                         {"type", "perspective"}, {"pixel_origin", "top_left"}, {"preset", nullptr}, {"fit", {{"object_id", 1}}}};
    json view = {{"preset", "iso"}, {"fit", {{"object_id", 1}}}, {"frame", "bed_mm"}, {"camera", camera}};
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({view})}}) == "");
    view["fit"] = {{"object_index", 1}};
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({view})}}) == "");

    // The camera a render answers with now (camera_mode, projection, fov_deg, mm_per_pixel, moved, requested_position,
    // replayed...) goes back as a view's camera, unchanged.
    json answered = camera;
    answered.update({{"camera_mode", "fit"}, {"projection", "perspective"}, {"fov_deg", 30.0}, {"mm_per_pixel", 0.05},
                     {"moved", true}, {"requested_position", {1, 2, 3}}, {"camera_position", {4, 5, 6}}, {"target", {0, 0, 0}},
                     {"replayed", false}, {"input_frame", "bed_mm"}});
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({{{"camera", answered}}})}}) == "");

    // The whole camera object, as the view itself.
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({answered})}}) == "");

    // color_by relief / detail with their ranges are declared; a misspelt range is not.
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"color_by", "relief"}, {"relief_range_mm", 0.2}, {"texture_preview", true}}) == "");
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"color_by", "detail"}, {"detail_range_deg", 10}}) == "");
    CHECK(::contains(refusal("render_plate_view", {{"plate_index", 0}, {"color_by", "relief"}, {"relief_range", 0.2}}), "has no argument \"relief_range\""));

    // compare_with is a view key, alone or with the earlier picture's own camera; a misspelling is not.
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({{{"compare_with", "/tmp/orcamcp_render_1_2_3_0.png"}}})}}) == "");
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({{{"compare_with", "/tmp/orcamcp_render_1_2_3_0.png"}, {"camera", answered}}})}}) == "");
    CHECK(::contains(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({{{"compare", "x"}}})}}), "has no key \"compare\""));

    // The new view keys, and the fit shapes, are declared; a misspelling is not.
    const json exact = {{"camera_position", {0, -60, 20}}, {"target", {0, 0, 10}}, {"camera_mode", "exact"},
                        {"projection", "perspective"}, {"fov_deg", 40}, {"ortho_height_mm", 20}};
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({exact})}}) == "");
    const json region = {{"preset", "iso"}, {"frame", "plate_local"},
                         {"fit", {{"box", {{"min", {0, 0, 0}}, {"max", {10, 10, 10}}}}}}};
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({region})}}) == "");
    const json sphere = {{"preset", "iso"}, {"fit", {{"point", {5, 5, 5}}, {"radius_mm", 3}}}};
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({sphere})}}) == "");
    const json radius = {{"preset", "iso"}, {"fit", {{"point", {5, 5, 5}}, {"radius", 3}}}};
    CHECK(::contains(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({radius})}}), "has no key \"radius\""));
    // The light an answer returns (with what a fallback adds) is taken back; an unknown light key is refused.
    const json light = {{"direction", {-0.8, -0.4, -0.2}}, {"azimuth_deg", 26.57}, {"elevation_deg", 12.0}, {"applied", true}};
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"light", light}}) == "");
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"light", "default"}}) == "");
    const json relative = {{"from", "left"}, {"elevation_deg", 12.0}, {"direction", {1, 0, 0}}, {"azimuth_deg", 0.0}, {"bed_elevation_deg", 12.0}};
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"light", relative}}) == "");
    CHECK(::contains(refusal("render_plate_view", {{"plate_index", 0}, {"light", {{"brightness", 2}}}}), "has no key \"brightness\""));
    // The paint options an answer echoes (paint, color_by) go back; texture_layers and the others are declared.
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"paint", {"support", "texture"}}, {"texture_layers", {0, 3}}, {"color_by", "filament"}}) == "");
    const json corner = {{"preset", "iso"}, {"fit", {{"box", {{"min", {0, 0, 0}}, {"centre", {1, 1, 1}}}}}}};
    CHECK(::contains(refusal("render_plate_view", {{"plate_index", 0}, {"views", json::array({corner})}}), "has no key \"centre\""));
}

TEST_CASE("render_plate_view section: every form, and an answer's section, goes back in", "[McpToolArguments][orcamcp][tools]")
{
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"section", "app"}}) == "");
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"section", {{"plane", "z"}, {"at_mm", 15}, {"remove", "below"}, {"frame", "plate_local"}}}}) == "");
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"section", {{"normal", {1, 0, 0}}, {"ratio", 0.5}}}}) == "");
    // What section_json answers with: the normal and point are the form, the rest is ignored.
    const json answered = {{"normal", {0, 0, 1}}, {"point", {0, 0, 15}}, {"source", "plane"}, {"removes", "the side the normal points to"},
                           {"ratio_used", 0.5}, {"cap_color", "#404040"}, {"applied", true}, {"empty", false},
                           {"objects", json::array({{{"object_id", 0}, {"section_area_mm2", 900.0}, {"islands", 1}, {"effect", "cut"}}})},
                           {"notes", json::array({"a note"})}, {"warning", "w"}};
    CHECK(refusal("render_plate_view", {{"plate_index", 0}, {"section", answered}}) == "");
    CHECK(::contains(refusal("render_plate_view", {{"plate_index", 0}, {"section", {{"plane", "z"}, {"height", 3}}}}), "has no key \"height\""));
}

