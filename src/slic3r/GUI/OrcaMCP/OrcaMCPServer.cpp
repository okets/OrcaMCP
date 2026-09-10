#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPresetConfigUtils.hpp"
#include "OrcaMCPPlateUtils.hpp"
#include "OrcaMCPConfigKeys.hpp"
#include "OrcaMCPFilamentUtils.hpp"
#include "OrcaMCPSliceEstimate.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <cmath>
#include <future>

namespace Slic3r { namespace GUI {

using namespace Slic3r::GUI::OrcaMCP;

// Static member initialization
std::map<std::string, OrcaMCPServer::ToolDefinition> OrcaMCPServer::s_tools;
bool OrcaMCPServer::s_initialized = false;

void OrcaMCPServer::init()
{
    if (s_initialized) return;

    BOOST_LOG_TRIVIAL(info) << "OrcaMCPServer: Initializing MCP server";

    // Clean up old preview files from previous sessions
    OrcaMCPPlateUtils::CleanupPreviews();

    register_builtin_tools();
    s_initialized = true;
}

void OrcaMCPServer::register_tool(const ToolDefinition& tool)
{
    s_tools[tool.name] = tool;
    BOOST_LOG_TRIVIAL(debug) << "OrcaMCPServer: Registered tool '" << tool.name << "'";
}

namespace {

// Every tool handler dereferences wxGetApp().plater() / preset_bundle unconditionally -- a json
// type_error at best, a crash of the whole process at worst if either is missing -- so the check
// that they exist lives here, in one place.
//
// The window that actually matters is recreate_GUI() (a language or theme change): it destroys the
// main frame and builds a new one, and MainFrame::shutdown() -> GUI_App::shutdown() deliberately
// returns early without stopping the HTTP server, because the app is not exiting. Meanwhile
// recreate_GUI's ProgressDialog pumps the event loop, so work queued by run_on_main_thread() can
// run against a half-rebuilt frame and a preset bundle in the middle of load_current_presets().
// At the app's two ends there is no such window: the server is only started at the end of
// post_init(), and GUI_App::shutdown() stops it before the frame is torn down. The startup checks
// below are kept anyway -- they are two pointer reads, and they keep this honest if the server is
// ever started earlier (WebUserLoginDialog already starts it on another port).
//
// tools/list, initialize and ping are deliberately *not* gated: an MCP client sends them while
// connecting, and answering them early is harmless (the tool table is static).
bool mcp_gui_ready(std::string& reason)
{
    if (wxApp::GetInstance() == nullptr) {
        reason = "the application object does not exist yet";
        return false;
    }
    GUI_App& app = wxGetApp();
    if (!app.post_initialized()) {
        reason = "the GUI has not finished starting up";
        return false;
    }
    if (app.is_recreating_gui()) {
        reason = "the GUI is being rebuilt (language or theme change)";
        return false;
    }
    if (app.plater() == nullptr) {
        reason = "the plater is not available";
        return false;
    }
    if (app.preset_bundle == nullptr) {
        reason = "the preset bundle is not loaded";
        return false;
    }
    return true;
}

} // namespace

std::shared_ptr<HttpServer::Response> OrcaMCPServer::handle_request(
    const std::string& method,
    const std::string& url,
    const std::string& body)
{
    // Only handle /mcp endpoint
    if (url.find("/mcp") == std::string::npos) {
        return nullptr;  // Not for us
    }

    // Ensure initialized
    if (!s_initialized) {
        init();
    }

    BOOST_LOG_TRIVIAL(debug) << "OrcaMCPServer: Handling " << method << " " << url;

    // Handle GET /mcp for server info
    if (method == "GET") {
        nlohmann::json info = {
            {"name", "orca-slicer"},
            {"version", "1.0.0"},
            {"protocol", "mcp"},
            {"description", "OrcaSlicer 3D Slicer MCP Server for Claude Code integration"}
        };
        return std::make_shared<HttpServer::ResponseJson>(info.dump());
    }

    // Handle POST /mcp for JSON-RPC requests
    if (method != "POST") {
        auto error = make_error_response(nlohmann::json(nullptr), -32600, "Method not allowed. Use POST for MCP requests.");
        return std::make_shared<HttpServer::ResponseJson>(error.dump(), 405);
    }

    // Parse JSON-RPC request
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
        auto error = make_error_response(nlohmann::json(nullptr), -32700, std::string("Parse error: ") + e.what());
        return std::make_shared<HttpServer::ResponseJson>(error.dump(), 400);
    }

    // Get the id field (can be number, string, or null)
    nlohmann::json id = request.contains("id") ? request["id"] : nlohmann::json(nullptr);

    // Validate JSON-RPC structure
    if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0") {
        auto error = make_error_response(id, -32600, "Invalid JSON-RPC version");
        return std::make_shared<HttpServer::ResponseJson>(error.dump(), 400);
    }

    if (!request.contains("method")) {
        auto error = make_error_response(id, -32600, "Missing method");
        return std::make_shared<HttpServer::ResponseJson>(error.dump(), 400);
    }

    std::string rpc_method = request["method"];
    nlohmann::json params = request.contains("params") ? request["params"] : nlohmann::json::object();

    BOOST_LOG_TRIVIAL(debug) << "OrcaMCPServer: RPC method: " << rpc_method;

    // Route to appropriate handler
    nlohmann::json result;
    try {
        if (rpc_method == "initialize") {
            result = handle_initialize(params);
        } else if (rpc_method == "tools/list") {
            result = handle_tools_list();
        } else if (rpc_method == "tools/call") {
            std::string not_ready_reason;
            if (!mcp_gui_ready(not_ready_reason)) {
                BOOST_LOG_TRIVIAL(warning) << "OrcaMCPServer: tools/call rejected, " << not_ready_reason;
                auto error = make_error_response(id, -32001, "OrcaMCP is starting up: " + not_ready_reason +
                                                             ". Retry in a few seconds.");
                return std::make_shared<HttpServer::ResponseJson>(error.dump(), 200);
            }
            result = handle_tools_call(params);
        } else if (rpc_method == "ping") {
            result = nlohmann::json::object();  // Empty response for ping
        } else {
            auto error = make_error_response(id, -32601, "Method not found: " + rpc_method);
            return std::make_shared<HttpServer::ResponseJson>(error.dump(), 400);
        }

        auto response = make_success_response(id, result);
        return std::make_shared<HttpServer::ResponseJson>(response.dump());

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "OrcaMCPServer: Error handling " << rpc_method << ": " << e.what();
        auto error = make_error_response(id, -32603, std::string("Internal error: ") + e.what());
        // Return HTTP 200 with JSON-RPC error (per MCP protocol spec)
        // HTTP 500 causes clients to interpret this as a connection failure
        return std::make_shared<HttpServer::ResponseJson>(error.dump(), 200);
    } catch (...) {
        // Nothing may escape onto the HTTP worker thread: an unhandled exception there takes the
        // whole process down instead of failing one call.
        BOOST_LOG_TRIVIAL(error) << "OrcaMCPServer: Unknown error handling " << rpc_method;
        auto error = make_error_response(id, -32603, "Internal error: unknown exception in " + rpc_method);
        return std::make_shared<HttpServer::ResponseJson>(error.dump(), 200);
    }
}

nlohmann::json OrcaMCPServer::handle_initialize(const nlohmann::json& params)
{
    BOOST_LOG_TRIVIAL(info) << "OrcaMCPServer: Initialize handshake";

    return {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", {
            {"tools", nlohmann::json::object()}
        }},
        {"serverInfo", {
            {"name", "orca-slicer"},
            {"version", "1.0.0"}
        }}
    };
}

nlohmann::json OrcaMCPServer::handle_tools_list()
{
    nlohmann::json tools_array = nlohmann::json::array();

    for (const auto& [name, tool] : s_tools) {
        // Ensure schema is valid JSON Schema draft 2020-12
        nlohmann::json schema = tool.input_schema;

        // Ensure additionalProperties is set (required for valid schema)
        if (!schema.contains("additionalProperties")) {
            schema["additionalProperties"] = false;
        }

        // Ensure required array exists
        if (!schema.contains("required")) {
            schema["required"] = nlohmann::json::array();
        }

        nlohmann::json tool_def = {
            {"name", tool.name},
            {"description", tool.description},
            {"inputSchema", schema}
        };
        tools_array.push_back(tool_def);
    }

    return {{"tools", tools_array}};
}

nlohmann::json OrcaMCPServer::handle_tools_call(const nlohmann::json& params)
{
    if (!params.contains("name")) {
        throw std::runtime_error("Missing tool name");
    }

    std::string tool_name = params["name"];
    nlohmann::json arguments = params.value("arguments", nlohmann::json::object());

    auto it = s_tools.find(tool_name);
    if (it == s_tools.end()) {
        throw std::runtime_error("Unknown tool: " + tool_name);
    }

    BOOST_LOG_TRIVIAL(info) << "OrcaMCPServer: Calling tool '" << tool_name << "'";

    // Execute the tool handler. Every failure inside a handler is reported as a JSON-RPC error for
    // that one call, naming the tool, rather than propagating further up the HTTP thread.
    nlohmann::json tool_result;
    try {
        tool_result = it->second.handler(arguments);
    } catch (const std::exception& e) {
        throw std::runtime_error("Tool '" + tool_name + "' failed: " + e.what());
    } catch (...) {
        throw std::runtime_error("Tool '" + tool_name + "' failed with an unknown exception");
    }

    // Format as MCP tool result
    return {
        {"content", {{
            {"type", "text"},
            {"text", tool_result.dump()}
        }}}
    };
}

nlohmann::json OrcaMCPServer::make_success_response(const nlohmann::json& id, const nlohmann::json& result)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
}

nlohmann::json OrcaMCPServer::make_error_response(const nlohmann::json& id, int code, const std::string& message)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {
            {"code", code},
            {"message", message}
        }}
    };
}

// Handler for get_preview_base64 tool - converts preview image to base64
nlohmann::json OrcaMCPServer::handle_get_preview_base64(const nlohmann::json& params)
{
    std::string path = params.at("path").get<std::string>();

    // Security: Only allow orcamcp preview/render files
    if (path.find("orcamcp_preview_") == std::string::npos &&
        path.find("orcamcp_render_") == std::string::npos) {
        return {
            {"status", "error"},
            {"message", "Invalid path: only orcamcp preview files are allowed"}
        };
    }

    // Load image
    wxImage image;
    if (!image.LoadFile(wxString::FromUTF8(path))) {
        return {
            {"status", "error"},
            {"message", "Failed to load image file: " + path}
        };
    }

    // Encode to JPEG in memory
    wxMemoryOutputStream stream;
    image.SaveFile(stream, wxBITMAP_TYPE_JPEG);

    // Get binary data
    wxStreamBuffer* buf = stream.GetOutputStreamBuffer();
    size_t size = buf->GetBufferSize();
    std::vector<unsigned char> buffer(size);
    std::memcpy(buffer.data(), buf->GetBufferStart(), size);

    // Convert to base64 using wxWidgets
    wxString base64_wx = wxBase64Encode(buffer.data(), size);
    std::string base64_data = base64_wx.ToStdString();

    return {
        {"status", "success"},
        {"preview_base64", "data:image/jpeg;base64," + base64_data},
        {"source_path", path}
    };
}

void OrcaMCPServer::register_builtin_tools()
{
    // ==================== SERVER INFO ====================

    // get_server_info - Get comprehensive server documentation
    register_tool({
        "get_server_info",
        "Get documentation about tools, concepts, and workflows",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return nlohmann::json{
                {"server", {
                    {"name", "OrcaSlicer MCP Server"},
                    {"version", "1.0.0"},
                    {"description", "Model Context Protocol server for controlling OrcaSlicer from Claude Code CLI"},
                    {"protocol", "JSON-RPC 2.0 over HTTP"},
                    {"endpoint", "http://localhost:13618/mcp"}
                }},

                // ==================== QUICK START ====================
                {"quick_start", {
                    {"first_steps", {
                        "1. Call get_scene_info to understand current project state",
                        "2. Use render_plate_view with save_to_file=true to visualize",
                        "3. Use get_server_info (this) for full documentation"
                    }},
                    {"common_tasks", {
                        {"load_and_slice", "load_model -> arrange_objects -> slice_all -> poll get_slicing_status -> export_gcode"},
                        {"change_settings", "apply_config with settings array"},
                        {"modify_object", "get_scene_info (get object_id) -> transform tools"},
                        {"visualize", "render_plate_view with save_to_file=true, then Read the file"}
                    }}
                }},

                // ==================== SUGGESTED TOOL FLOWS ====================
                {"suggested_flows", {
                    {"basic_print_workflow", {
                        {"description", "Load a model and prepare it for printing"},
                        {"steps", {
                            {"step", "1. Load model"},
                            {"tool", "load_model"},
                            {"example", R"({"file_path": "/path/to/model.stl"})"},
                            {"next", "2. Arrange on plate"},
                            {"tool2", "arrange_objects"},
                            {"example2", "{}"},
                            {"next2", "3. Start slicing"},
                            {"tool3", "slice_all"},
                            {"example3", "{}"},
                            {"next3", "4. Wait for completion (poll every 2-3 seconds)"},
                            {"tool4", "get_slicing_status"},
                            {"example4", "{} -> repeat until is_slicing=false"},
                            {"next4", "5. Export G-code"},
                            {"tool5", "export_gcode"},
                            {"example5", R"({"output_path": "/path/to/output.gcode"})"}
                        }}
                    }},
                    {"visual_inspection_workflow", {
                        {"description", "View the model before making changes"},
                        {"steps", {
                            {"step", "1. Render views to temp files (saves tokens!)"},
                            {"tool", "render_plate_view"},
                            {"example", R"({"plate_index": 0, "save_to_file": true, "views": [{"camera_position": [300, -200, 150], "target": [155, 155, 30]}]})"},
                            {"next", "2. Read the image file with Claude's Read tool"},
                            {"note", "The file_path returned can be read directly by Claude"}
                        }}
                    }},
                    {"settings_modification_workflow", {
                        {"description", "Change print settings"},
                        {"steps", {
                            {"step", "1. Apply settings (can batch multiple)"},
                            {"tool", "apply_config"},
                            {"example", R"({"settings": [{"type": "print", "key": "layer_height", "value": "0.15"}, {"type": "print", "key": "sparse_infill_density", "value": "20%"}]})"}
                        }},
                        {"note", "Settings become 'dirty' until user saves preset in UI"}
                    }},
                    {"object_manipulation_workflow", {
                        {"description", "Transform objects (move, rotate, scale, cut)"},
                        {"steps", {
                            {"step", "1. Get object IDs"},
                            {"tool", "get_scene_info"},
                            {"example", R"({"with_model_object_features": false})"},
                            {"note", "Objects are 0-indexed. Look for model_objects array in response."},
                            {"step2", "2. Transform as needed"},
                            {"tools", "move_object, rotate_object, scale_object, mirror_object, cut_object"},
                            {"examples", {
                                {"move", R"({"object_id": 0, "x": 10, "y": 0, "z": 0})"},
                                {"rotate", R"({"object_id": 0, "z": 45})"},
                                {"scale", R"({"object_id": 0, "x": 1.5, "uniform": true})"},
                                {"cut", R"({"object_id": 0, "z_height": 25, "keep": "below"})"}
                            }}
                        }}
                    }},
                    {"per_object_settings_workflow", {
                        {"description", "Apply different settings to specific objects"},
                        {"steps", {
                            {"step", "1. Get object IDs from get_scene_info"},
                            {"step2", "2. Set per-object overrides"},
                            {"tool", "set_object_config"},
                            {"example", R"({"object_id": 0, "settings": [{"key": "sparse_infill_density", "value": "30%"}, {"key": "enable_support", "value": "1"}]})"},
                            {"step3", "3. Verify with get_object_config"},
                            {"step4", "4. Reset if needed with reset_object_config"}
                        }}
                    }},
                    {"layer_range_workflow", {
                        {"description", "Different settings at different Z heights within one object"},
                        {"steps", {
                            {"step", "1. Define layer range"},
                            {"tool", "set_object_layer_range"},
                            {"example", R"({"object_id": 0, "z_min": 10, "z_max": 20, "settings": [{"key": "layer_height", "value": "0.1"}]})"},
                            {"use_case", "Fine detail at specific heights, variable infill, etc."}
                        }}
                    }},
                    {"undo_recovery_workflow", {
                        {"description", "Recover from mistakes"},
                        {"steps", {
                            {"step", "1. Undo last operation"},
                            {"tool", "undo"},
                            {"note", "Can call multiple times to undo multiple operations"},
                            {"step2", "2. Redo if needed"},
                            {"tool2", "redo"}
                        }},
                        {"warning", "Undo history may be limited. For safety, save project (export_3mf) before major changes."}
                    }},
                    {"printer_workflow", {
                        {"description", "Slice and send to printer (OctoPrint/Klipper or Bambu)"},
                        {"steps", {
                            {"step", "1. Check available printers"},
                            {"tool", "get_printers"},
                            {"example", "{}"},
                            {"note", "Look for current_print_host (OctoPrint/Klipper) or local_printers (Bambu)"},
                            {"step2", "2. Slice the project"},
                            {"tool2", "slice_all"},
                            {"step3", "3. Wait for slicing to complete"},
                            {"tool3", "get_slicing_status"},
                            {"note2", "Poll every 2-3 seconds until is_slicing=false"},
                            {"step4", "4. Send to printer"},
                            {"tool4", "send_to_printer"},
                            {"example4", R"({})"},
                            {"note3", "Auto-detects printer type and opens appropriate dialog"}
                        }},
                        {"octoprint_note", "For OctoPrint/Klipper: print_host must be configured in printer preset. Dialog shows Upload/Upload and Print options."},
                        {"bambu_note", "For Bambu: use select_printer with dev_id first if needed. Dialog shows printer selection."}
                    }}
                }},

                // ==================== TOOL EXAMPLES ====================
                {"tool_examples", {
                    {"get_scene_info", {
                        {"minimal", R"({})"},
                        {"with_features", R"({"with_model_object_features": true})"},
                        {"when_to_use", "Start of session, after loading models, before transforms"},
                        {"response_includes", {
                            {"bed", "origin (corner), min_x, min_y, max_x, max_y, max_z - printable area bounds"},
                            {"plates[].model_objects[]", "object_index, name, position, rotation_degrees, scale, bounding_box, instance_count"}
                        }},
                        {"tip", "Use bed info to calculate valid positions. Object positions are center points."}
                    }},
                    {"render_plate_view", {
                        {"single_view", R"({"plate_index": 0, "save_to_file": true, "views": [{"camera_position": [300, -200, 150], "target": [155, 155, 30]}]})"},
                        {"multiple_views", R"({"plate_index": 0, "save_to_file": true, "views": [{"camera_position": [300, -200, 150], "target": [155, 155, 30]}, {"camera_position": [155, -200, 50], "target": [155, 155, 30]}, {"camera_position": [10, -100, 80], "target": [155, 155, 30]}]})"},
                        {"when_to_use", "Before/after transforms, to verify object state, to analyze geometry"},
                        {"tip", "ALWAYS use save_to_file=true to avoid huge base64 responses"}
                    }},
                    {"apply_config", {
                        {"single_setting", R"({"settings": [{"type": "print", "key": "layer_height", "value": "0.2"}]})"},
                        {"multiple_settings", R"({"settings": [{"type": "print", "key": "layer_height", "value": "0.15"}, {"type": "print", "key": "wall_loops", "value": "3"}, {"type": "print", "key": "sparse_infill_density", "value": "20%"}]})"},
                        {"filament_temp", R"({"settings": [{"type": "filament", "key": "nozzle_temperature", "value": ["210"]}]})"},
                        {"when_to_use", "Adjusting print quality, speed, supports, etc."}
                    }},
                    {"cut_object", {
                        {"keep_bottom", R"({"object_id": 0, "z_height": 30, "keep": "below"})"},
                        {"keep_top", R"({"object_id": 0, "z_height": 30, "keep": "above"})"},
                        {"keep_both", R"({"object_id": 0, "z_height": 30, "keep": "both"})"},
                        {"when_to_use", "Splitting models, removing overhangs, creating multi-part prints"}
                    }},
                    {"move_object", {
                        {"relative", R"({"object_id": 0, "x": 10, "y": -5})"},
                        {"absolute", R"({"object_id": 0, "x": 155, "y": 155, "relative": false})"},
                        {"when_to_use", "Positioning objects on bed, separating objects"},
                        {"response_includes", "position, rotation_degrees, scale, on_bed, warnings"},
                        {"tip", "Unspecified axes are preserved. Use on_bed to verify valid placement."}
                    }},
                    {"rotate_object", {
                        {"example", R"({"object_id": 0, "z": 90})"},
                        {"when_to_use", "Orienting objects for better print quality or bed adhesion"},
                        {"response_includes", "position, rotation_degrees, scale, on_bed, warnings"}
                    }},
                    {"scale_object", {
                        {"uniform", R"({"object_id": 0, "x": 1.5, "uniform": true})"},
                        {"non_uniform", R"({"object_id": 0, "x": 1.0, "y": 1.0, "z": 2.0})"},
                        {"when_to_use", "Resizing models, adjusting proportions"},
                        {"response_includes", "position, rotation_degrees, scale, on_bed, warnings"}
                    }},
                    {"get_printers", {
                        {"example", "{}"},
                        {"response_fields", "local_printers, cloud_printers, physical_printers, selected_physical_printer, current_print_host, total_count"},
                        {"when_to_use", "Check what printers are available before sending"},
                        {"tip", "physical_printers lists the printer presets that carry a print host; selected_physical_printer is the active one"}
                    }},
                    {"select_printer", {
                        {"bambu_device", R"({"dev_id": "00M00A2B0123456"})"},
                        {"print_host", R"({"physical_printer": "C5P"})"},
                        {"when_to_use", "Select a Bambu printer by device ID, or a printer preset with a print host by name"}
                    }},
                    {"discover_printers", {
                        {"example", R"({"timeout_ms": 5000})"},
                        {"when_to_use", "Find Flashforge printers on the LAN before add_physical_printer"},
                        {"response_fields", "printers[] with name, serial_number, ip_address"}
                    }},
                    {"add_physical_printer", {
                        {"example", R"({"name": "C5P", "host": "192.168.1.50", "host_type": "flashforge", "serial_number": "SN", "api_key": "check code"})"},
                        {"when_to_use", "Configure a print host and save it as a user printer preset"},
                        {"tip", "printer_preset selects the preset to base it on; defaults to the edited printer preset"}
                    }},
                    {"clone_object", {
                        {"to_current_plate", R"({"object_id": 0, "count": 2, "duplicate": true})"},
                        {"to_specific_plate", R"({"object_id": 0, "count": 2, "duplicate": true, "destination_plate": 1})"},
                        {"destination_behavior", "If destination_plate is OMITTED, clones go to CURRENT plate. If specified, clones go to that plate."},
                        {"example_scenario", "You're on plate 1, cloning object from plate 0: clone_object(object_id=0, count=2) -> clones appear on plate 1 (current). clone_object(object_id=0, count=2, destination_plate=0) -> clones appear on plate 0 (explicit)."},
                        {"response_includes", "source_plate, destination_plate, current_plate_at_call, destination_mode (explicit/defaulted_to_current)"},
                        {"tip", "Use duplicate=true for independent objects, duplicate=false (default) for linked instances."}
                    }},
                    {"send_to_printer", {
                        {"current_plate", R"({})"},
                        {"all_plates", R"({"all_plates": true})"},
                        {"when_to_use", "After slicing complete - opens upload dialog"},
                        {"auto_detect", "Opens OctoPrint dialog if print_host configured, otherwise Bambu dialog"}
                    }}
                }},

                // ==================== CONCEPTS ====================
                {"concepts", {
                    {"presets", {
                        {"description", "OrcaSlicer uses a preset system with three types: Printer, Filament, and Print presets. Each defines a set of configuration options."},
                        {"printer_preset", "Defines machine capabilities: build volume, nozzle size, speeds, G-code flavor, start/end G-code"},
                        {"filament_preset", "Defines material properties: temperatures, cooling, flow ratio, retraction (if not using printer defaults)"},
                        {"print_preset", "Defines slicing parameters: layer height, speeds, infill, walls, supports, etc."}
                    }},
                    {"dirty_values", {
                        {"description", "When you modify a setting, it becomes 'dirty' - meaning it differs from the saved preset. Dirty values are tracked in the 'dirty_options' array."},
                        {"example", "If you change layer_height from 0.2 to 0.22, 'layer_height' appears in dirty_options"},
                        {"persistence", "Dirty values are NOT automatically saved. They exist only in the current editing session."},
                        {"saving", "To save dirty values permanently, the user must save the preset through the UI (Ctrl+S or right-click preset -> Save)"},
                        {"use_case", "Dirty tracking lets you experiment with settings without modifying saved presets. You can always revert by reloading the preset."}
                    }},
                    {"plates", {
                        {"description", "OrcaSlicer supports multiple build plates in a single project. Each plate can contain different objects and be sliced independently."},
                        {"indexing", "Plates are 0-indexed in the API (plate_index: 0 is the first plate)"},
                        {"delete_constraint", "Cannot delete the last remaining plate. Objects on deleted plate are moved to another plate."}
                    }},
                    {"coordinate_system", {
                        {"origin", "CORNER origin (0,0) = front-left of bed. NOT center origin!"},
                        {"valid_range", "X: 0 to max_x, Y: 0 to max_y. Negative coordinates are OFF the bed."},
                        {"z_axis", "Z=0 is the bed surface. Object bottoms rest at Z=0. Object center Z = half the object height."},
                        {"get_bed_bounds", "Call get_scene_info and read bed.min_x, bed.max_x, bed.min_y, bed.max_y"},
                        {"transform_response", "All transforms return position, rotation_degrees, scale, on_bed. Use on_bed to verify placement."},
                        {"rotation_degrees_note", "rotation_degrees reflects UI/initial rotation only. MCP rotate_object applies rotation directly to mesh geometry, so the field may not update. Use bounding_box dimensions to verify rotation was applied."},
                        {"recommendation", "Read bed bounds first. Use arrange_objects to auto-place, or relative=true with offsets."}
                    }},
                    {"slicing", {
                        {"description", "Slicing converts 3D models into G-code layer by layer. It's an async operation."},
                        {"workflow", "1) Load model 2) Configure settings 3) Call slice_all 4) Poll get_slicing_status until complete 5) Export G-code"}
                    }},
                    {"object_ids", {
                        {"description", "Each object has two identifiers: 'id' (stable string) and 'object_index' (transient 0-based integer)."},
                        {"id_stable", "The 'id' field is a unique stable identifier that persists across add/delete operations. Use for tracking objects across sessions."},
                        {"object_index_transient", "The 'object_index' field is a 0-based array index used for MCP tool operations (move, rotate, etc.). It shifts when objects are added/deleted."},
                        {"finding_ids", "Call get_scene_info and look at plates[].model_objects[] array for both 'id' and 'object_index'"},
                        {"best_practice", "For multi-step workflows: store 'id' to track objects, re-query get_scene_info for current 'object_index' before each operation."}
                    }},
                    {"instances_vs_objects", {
                        {"description", "A ModelObject can have multiple instances. Instances share geometry and per-object settings but have independent positions."},
                        {"instances", "Created by clone_object with duplicate=false (default). All instances transform together - move one, all move. Ideal for printing multiple identical copies."},
                        {"independent_objects", "Created by clone_object with duplicate=true. Each copy is a separate ModelObject with its own object_id and can be transformed independently."},
                        {"instance_count", "The 'instance_count' field in get_scene_info shows how many instances an object has."},
                        {"when_to_use_instances", "Use instances (duplicate=false) when you want multiple identical prints and don't need to move them separately."},
                        {"when_to_use_duplicates", "Use duplicates (duplicate=true) when you need to position, rotate, or scale each copy independently."}
                    }},
                    {"per_object_settings", {
                        {"description", "Individual objects can have their own settings that override global print settings."},
                        {"use_cases", "Different layer heights for detail vs speed, enable support only for specific objects, vary infill density"},
                        {"api", "Use get_object_config/set_object_config to manage per-object overrides. object_id is 0-indexed."},
                        {"reset", "Use reset_object_config to remove overrides and fall back to global settings"}
                    }},
                    {"layer_ranges", {
                        {"description", "Within a single object, you can define different settings for specific Z height ranges."},
                        {"example", "Use 0.1mm layers from Z=10-20mm for fine detail, 0.3mm elsewhere for speed"},
                        {"api", "Use get_object_layer_ranges/set_object_layer_range/delete_object_layer_range to manage"},
                        {"key_format", "Ranges are defined by z_min and z_max in millimeters"}
                    }}
                }},

                // ==================== TOOLS BY CATEGORY ====================
                {"tools_by_category", {
                    {"information", {
                        {"get_server_info", "This documentation"},
                        {"get_scene_info", "Get current project state: plates, objects, positions"},
                        {"get_object_info", "Get single object info. Faster than get_scene_info for targeted queries."},
                        {"get_presets", "List presets for the selected printer. Narrow with type/vendor/name_contains; "
                                        "summary:false adds full configs (large)"},
                        {"get_edited_presets", "Get currently active presets with their config values and dirty_options"},
                        {"get_slicing_status", "Check if slicing is in progress"}
                    }},
                    {"configuration", {
                        {"select_preset", "Switch to a different preset by name. type=filament + slot (1-based) sets one filament slot."},
                        {"apply_config", "Modify individual settings (creates dirty values)"},
                        {"clone_preset", "Duplicate an existing preset with a new name"},
                        {"save_preset", "Persist dirty changes to disk"},
                        {"delete_preset", "Remove user-created presets. Cannot delete system presets or presets with dependents."},
                        {"reset_preset", "Discard dirty changes without saving"},
                        {"get_valid_config_keys", "Discover available setting keys by category"}
                    }},
                    {"model_operations", {
                        {"load_model", "Import STL, OBJ, STEP, 3MF model files"},
                        {"load_project", "Open a complete 3MF project with settings"},
                        {"new_project", "Clear all objects and start fresh"},
                        {"auto_orient", "Automatically orient objects for optimal printing"},
                        {"arrange_objects", "Auto-arrange objects on the build plate"},
                        {"undo", "Undo last operation"},
                        {"redo", "Redo last undone operation"}
                    }},
                    {"object_transforms", {
                        {"move_object", "Move/translate an object (relative offset or absolute position)"},
                        {"rotate_object", "Rotate an object around X, Y, Z axes (degrees)"},
                        {"scale_object", "Scale an object (uniform or per-axis factors)"},
                        {"mirror_object", "Mirror an object across X, Y, or Z axis"},
                        {"clone_object", "Copy an object to current or specified plate. duplicate=false (default) creates instances, duplicate=true creates independent objects. destination_plate specifies where clones go (defaults to current plate)."},
                        {"delete_object", "Remove an object from the scene"},
                        {"rename_object", "Rename an object for identification"},
                        {"flatten_object", "Auto-orient object to lay flat on best face"},
                        {"cut_object", "Cut object horizontally at Z height (keep above/below/both)"},
                        {"transform_objects", "Batch transform multiple objects in one call"}
                    }},
                    {"slicing_export", {
                        {"slice_all", "Start slicing (async, poll get_slicing_status)"},
                        {"get_print_estimate", "Get print time and filament usage after slicing"},
                        {"export_gcode", "Export sliced G-code to file"},
                        {"export_3mf", "Export project as 3MF file"},
                        {"save_project", "Save current project"}
                    }},
                    {"visualization", {
                        {"render_plate_view", "Render plate thumbnail from custom camera angles (use save_to_file=true for file paths instead of base64)"},
                        {"get_preview_base64", "Convert preview to base64. Only for agents without filesystem access - Claude Code should use Read tool instead."}
                    }},
                    {"per_object_settings", {
                        {"get_object_config", "Get per-object setting overrides for a specific object"},
                        {"set_object_config", "Set per-object settings (override global settings)"},
                        {"reset_object_config", "Remove per-object overrides (revert to global)"},
                        {"get_object_layer_ranges", "Get layer-range-specific configs for an object"},
                        {"set_object_layer_range", "Set settings for a specific Z height range"},
                        {"delete_object_layer_range", "Remove layer range configs"}
                    }},
                    {"variable_layer_height", {
                        {"apply_adaptive_layer_height", "Apply VLH to object based on geometry. Quality 0.0-1.0 controls layer variation."},
                        {"clear_adaptive_layer_height", "Remove VLH from object, revert to fixed layer height."}
                    }},
                    {"plate_management", {
                        {"add_plate", "Create a new plate"},
                        {"select_plate", "Switch to a plate by index"},
                        {"delete_plate", "Delete a plate. Cannot delete last plate. Objects moved to another plate."}
                    }},
                    {"printer_management", {
                        {"get_printers", "List printers: Bambu (local/cloud) and printer presets with a print host"},
                        {"select_printer", "Select a Bambu printer by dev_id, or a print-host preset by physical_printer"},
                        {"discover_printers", "Find Flashforge printers on the local network"},
                        {"add_physical_printer", "Save a print host into a printer preset and select it"},
                        {"send_to_printer", "Send G-code: auto-detects OctoPrint/Klipper vs Bambu dialog"},
                        {"match_project_to_printer", "Point the project's filament slots at the material and colour the printer's station actually holds"}
                    }}
                }},

                // ==================== COMMON SETTINGS ====================
                {"setting_types", {
                    {"print", "Print process settings like layer_height, infill, speeds, supports"},
                    {"filament", "Filament settings like temperatures, cooling, flow_ratio"},
                    {"printer", "Printer/machine settings like retraction, speeds, G-code flavor"}
                }},
                {"common_print_settings", {
                    {"layer_height", "Layer height in mm (e.g., '0.2')"},
                    {"initial_layer_print_height", "First layer height in mm"},
                    {"wall_loops", "Number of perimeter walls (integer)"},
                    {"sparse_infill_density", "Infill percentage as string (e.g., '15%')"},
                    {"sparse_infill_pattern", "Infill pattern: grid, honeycomb, gyroid, etc."},
                    {"enable_support", "Enable supports: '0' or '1'"},
                    {"support_type", "Support type: normal(auto), tree(auto), etc."},
                    {"top_shell_layers", "Number of top solid layers"},
                    {"bottom_shell_layers", "Number of bottom solid layers"},
                    {"outer_wall_speed", "Outer wall print speed in mm/s"},
                    {"inner_wall_speed", "Inner wall print speed in mm/s"},
                    {"sparse_infill_speed", "Infill print speed in mm/s"},
                    {"travel_speed", "Travel move speed in mm/s"}
                }},
                {"common_filament_settings", {
                    {"nozzle_temperature", "Nozzle temperature array (e.g., ['200'])"},
                    {"nozzle_temperature_initial_layer", "First layer nozzle temp array"},
                    {"hot_plate_temp", "Bed temperature array"},
                    {"hot_plate_temp_initial_layer", "First layer bed temp array"},
                    {"filament_flow_ratio", "Flow multiplier array (e.g., ['0.95'])"},
                    {"fan_max_speed", "Maximum fan speed array (e.g., ['100'])"},
                    {"fan_min_speed", "Minimum fan speed array"}
                }},
                {"common_printer_settings", {
                    {"retraction_length", "Retraction distance array in mm (e.g., ['0.8'])"},
                    {"retraction_speed", "Retraction speed array in mm/s (e.g., ['30'])"},
                    {"z_hop", "Z hop distance array in mm (e.g., ['0.4'])"},
                    {"machine_max_speed_x", "Max X speed array in mm/s"},
                    {"machine_max_speed_y", "Max Y speed array in mm/s"},
                    {"machine_max_acceleration_x", "Max X acceleration array"},
                    {"machine_start_gcode", "Start G-code template"},
                    {"machine_end_gcode", "End G-code template"}
                }},

                // ==================== WARNINGS AND BEST PRACTICES ====================
                {"warnings_and_best_practices", {
                    {"token_optimization", {
                        {"critical", "ALWAYS use save_to_file=true with render_plate_view to avoid 5KB+ base64 images per view"},
                        {"avoid_heavy_tools", {
                            {"get_edited_presets", "~15-20KB response. Use sparingly, cache results."},
                            {"get_presets", "Filter it: {type, vendor, name_contains}. summary:false without a filter "
                                            "is ~1.9MB and will not fit in a response."},
                            {"get_scene_info", "Use with_model_object_features=false unless you need volume/overhang data."}
                        }},
                        {"prefer_light_tools", {
                            "get_slicing_status - tiny response, safe for polling",
                            "apply_config - small response",
                            "undo/redo - minimal response",
                            "All transform tools (move, rotate, scale, etc.) - minimal responses"
                        }}
                    }},
                    {"common_pitfalls", {
                        {"object_index_shifts", "After delete/add, object_index values shift. Use stable 'id' to track objects, re-query for current object_index."},
                        {"async_operations", "slice_all, auto_orient, arrange_objects are async. Poll or wait before next step."},
                        {"cut_object_caution", "Cut removes original and creates new object(s). Use undo if result is wrong."},
                        {"settings_not_saved", "apply_config creates dirty values. User must save preset in UI to persist."},
                        {"undo_limits", "Undo history is limited. Save project before destructive operations."},
                        {"positioning", "For absolute move_object: unspecified axes preserve current position. To spread objects, use relative=true with offsets, or arrange_objects."}
                    }},
                    {"efficiency_tips", {
                        "Batch settings: put multiple items in one apply_config call",
                        "Store stable 'id' values, re-query object_index only when needed for operations",
                        "Use render_plate_view before and after transforms to verify",
                        "Poll get_slicing_status every 2-3 seconds, not faster"
                    }},
                    {"visual_preview", {
                        {"description", "Many tools support include_preview=true to return a turntable preview image path alongside results."},
                        {"supported_tools", {
                            "get_scene_info", "load_model", "load_project",
                            "move_object", "rotate_object", "scale_object", "mirror_object",
                            "flatten_object", "clone_object", "delete_object", "cut_object",
                            "arrange_objects", "auto_orient", "undo", "redo",
                            "apply_adaptive_layer_height", "clear_adaptive_layer_height"
                        }},
                        {"preview_hint", "When include_preview=true, the response includes a 'preview_hint' message encouraging you to check the preview image for a visual sense of the plate and objects."},
                        {"recommendation", "Use include_preview to visually verify the result of operations, especially after loading, transforms, or destructive changes."}
                    }}
                }}
            };
        }
    });

    // ==================== READ OPERATIONS ====================

    // get_scene_info - Get current project state
    register_tool({
        "get_scene_info",
        "Get current project state: plates, objects, positions. Call first to get object_ids.",
        {
            {"type", "object"},
            {"properties", {
                {"with_model_object_features", {
                    {"type", "boolean"},
                    {"description", "Include overhang, volume, etc."}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }},
                {"preview_views", {
                    {"type", "integer"},
                    {"description", "Views: 4 or 8 (default: 4)"}
                }},
                {"preview_resolution", {
                    {"type", "integer"},
                    {"description", "Resolution in pixels (default: 256)"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            bool with_features = params.value("with_model_object_features", false);
            bool include_preview = params.value("include_preview", false);
            int preview_views = params.value("preview_views", 4);
            int preview_resolution = params.value("preview_resolution", 256);
            return run_on_main_thread([with_features, include_preview, preview_views, preview_resolution]() {
                nlohmann::json result = OrcaMCPPlateUtils::GetCurrentProject(with_features);

                // Add turntable preview if requested
                if (include_preview) {
                    Plater* plater = wxGetApp().plater();
                    int plate_index = plater->get_partplate_list().get_curr_plate_index();
                    nlohmann::json preview = OrcaMCPPlateUtils::CaptureTurntablePreview(
                        plate_index, preview_views, preview_resolution);
                    if (preview.contains("preview_path")) {
                        result["preview_path"] = preview["preview_path"];
                        result["preview_hint"] = "Check the preview image to get a visual overview of objects on the current plate.";
                    }
                }

                // Always include active warnings section
                result["active_warnings"] = get_active_warnings_json(wxGetApp().plater());

                return result;
            });
        }
    });

    // get_presets - List the presets compatible with the selected printer
    register_tool({
        "get_presets",
        "List the printer, filament and print presets available for the selected printer. "
        "Returns names and identifying fields only; pass summary:false for full configs "
        "(large -- always narrow it with type/vendor/name_contains first).",
        {
            {"type", "object"},
            {"properties", {
                {"type", {
                    {"type", "string"},
                    {"enum", {"printer", "filament", "print", "all"}},
                    {"description", "Only this preset type. Omit (or \"all\") for all three."}
                }},
                {"vendor", {
                    {"type", "string"},
                    {"description", "Only presets from this vendor (case-insensitive substring, e.g. \"Flashforge\")"}
                }},
                {"name_contains", {
                    {"type", "string"},
                    {"description", "Only presets whose name contains this (case-insensitive, e.g. \"PETG\")"}
                }},
                {"summary", {
                    {"type", "boolean"},
                    {"default", true},
                    {"description", "true (default): name, vendor, filament_type/printer_model and flags. "
                                    "false: also every config key of every matching preset."}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            PresetQuery query;
            query.vendor = params.value("vendor", std::string());
            query.name_contains = params.value("name_contains", std::string());
            if (params.contains("summary") && !parse_boolean_param(params["summary"], query.summary))
                return nlohmann::json{{"status", "error"}, {"message", "summary must be a boolean"}};

            std::string type = params.value("type", std::string());
            if (type == "all")
                type.clear();
            if (!type.empty() && type != "printer" && type != "filament" && type != "print")
                return nlohmann::json{{"status", "error"},
                                      {"message", "type must be one of: printer, filament, print, all"}};

            return run_on_main_thread([query, type]() -> nlohmann::json {
                nlohmann::json result;
                if (type.empty()) {
                    result = OrcaMCPPresetConfigUtils::GetAllPresetJson(query);
                } else if (type == "printer") {
                    result = {{"printerPresets", OrcaMCPPresetConfigUtils::GetPresetsJson(Preset::TYPE_PRINTER, query)}};
                } else if (type == "filament") {
                    result = {{"filamentPresets", OrcaMCPPresetConfigUtils::GetPresetsJson(Preset::TYPE_FILAMENT, query)}};
                } else {
                    result = {{"printProcessPresets", OrcaMCPPresetConfigUtils::GetPresetsJson(Preset::TYPE_PRINT, query)}};
                }
                // Say what was searched and how much of it came back, so a caller can tell an empty
                // list from a filter that was too narrow, and knows the list is already restricted
                // to presets compatible with the selected printer.
                nlohmann::json counts = nlohmann::json::object();
                for (const auto& [key, presets] : result.items())
                    counts[key] = presets.size();
                result["query"] = {
                    {"type", type.empty() ? nlohmann::json(nullptr) : nlohmann::json(type)},
                    {"vendor", query.vendor},
                    {"name_contains", query.name_contains},
                    {"summary", query.summary},
                    {"compatible_with_selected_printer_only", true},
                    {"counts", counts}
                };
                return result;
            });
        }
    });

    // get_edited_presets - Get currently edited presets
    register_tool({
        "get_edited_presets",
        "Get currently edited presets with dirty (modified) options",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([]() {
                return OrcaMCPPresetConfigUtils::GetAllEditedPresetJson();
            });
        }
    });

    // ==================== VISUALIZATION ====================

    // render_plate_view - Render plate thumbnail
    register_tool({
        "render_plate_view",
        "Render plate from custom camera angles. Use save_to_file=true for file paths.",
        {
            {"type", "object"},
            {"properties", {
                {"plate_index", {
                    {"type", "integer"},
                    {"description", "Plate index (0-based)"}
                }},
                {"save_to_file", {
                    {"type", "boolean"},
                    {"description", "Return file paths instead of base64 (default: false)"},
                    {"default", false}
                }},
                {"resolution", {
                    {"type", "integer"},
                    {"description", "Resolution in pixels (default: 512)"},
                    {"default", 512}
                }},
                {"views", {
                    {"type", "array"},
                    {"description", "View configurations"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"camera_position", {
                                {"type", "array"},
                                {"items", {{"type", "number"}}},
                                {"description", "[x, y, z]"}
                            }},
                            {"target", {
                                {"type", "array"},
                                {"items", {{"type", "number"}}},
                                {"description", "[x, y, z]"}
                            }}
                        }}
                    }}
                }}
            }},
            {"required", {"plate_index", "views"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([params]() {
                nlohmann::json render_params = {{"payload", params}};
                return OrcaMCPPlateUtils::RenderPlateView(render_params);
            });
        }
    });

    // get_preview_base64 - Convert preview image to base64 (for remote clients)
    register_tool({
        "get_preview_base64",
        "Convert preview image to base64. Use only if no filesystem access.",
        {
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description", "Path to preview image file"}
                }}
            }},
            {"required", {"path"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return handle_get_preview_base64(params);
        }
    });

    // ==================== CONFIGURATION ====================

    // select_preset - Select a preset
    register_tool({
        "select_preset",
        "Select a printer, filament, or print preset by name. With type 'filament', pass slot "
        "(1-based) to set just that filament slot, like the sidebar filament combo; without slot "
        "the filament tab switches whichever slot it is on and dirty preset changes are discarded.",
        {
            {"type", "object"},
            {"properties", {
                {"type", {
                    {"type", "string"},
                    {"enum", {"printer", "filament", "print"}},
                    {"description", "Preset type"}
                }},
                {"name", {
                    {"type", "string"},
                    {"description", "Preset name"}
                }},
                {"slot", {
                    {"type", "integer"},
                    {"description", "Filament slot to set, 1-based. Only valid with type 'filament'."}
                }}
            }},
            {"required", {"type", "name"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string type = params["type"];
            std::string name = params["name"];
            int  slot     = 0;
            bool has_slot = params.contains("slot") && !params["slot"].is_null();
            if (has_slot) {
                if (!parse_integer_param(params["slot"], slot)) {
                    return nlohmann::json{{"status", "error"},
                                          {"message", "slot must be an integer (1-based filament slot), got: " +
                                                          params["slot"].dump()}};
                }
                if (type != "filament") {
                    return nlohmann::json{{"status", "error"},
                                          {"message", "slot is only valid with type 'filament'"}};
                }
            }

            return run_on_main_thread([type, name, slot, has_slot]() -> nlohmann::json {
                // Suppress any dialogs during preset selection
                McpDialogSuppressionGuard suppression_guard;
                nlohmann::json response = {{"status", "success"}};
                if (has_slot) {
                    std::string error;
                    if (!OrcaMCPPresetConfigUtils::SelectFilamentSlotPreset(slot, name, error)) {
                        return nlohmann::json{{"status", "error"}, {"message", error}};
                    }
                    response["slot"] = slot;
                    response["filaments"] = describe_filaments()["filaments"];
                } else {
                    OrcaMCPPresetConfigUtils::SelectPreset(type, name);
                }

                auto info_messages = suppression_guard.messages();
                if (!info_messages.empty()) {
                    response["info_messages"] = info_messages;
                }
                return response;
            });
        }
    });

    // apply_config - Apply print settings
    register_tool({
        "apply_config",
        "Apply print settings. Batch multiple in one call. Types: print | filament | printer | project.",
        {
            {"type", "object"},
            {"properties", {
                {"settings", {
                    {"type", "array"},
                    {"description", "Settings array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"type", {
                                {"type", "string"},
                                {"enum", {"print", "filament", "printer", "project"}},
                                {"description", "Setting type"}
                            }},
                            {"key", {
                                {"type", "string"},
                                {"description", "Key name"}
                            }},
                            {"value", {
                                {"description", "Value"}
                            }}
                        }},
                        {"required", {"type", "key", "value"}}
                    }}
                }}
            }},
            {"required", {"settings"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            nlohmann::json settings = params["settings"];
            return run_on_main_thread([settings]() {
                McpDialogSuppressionGuard suppression_guard;

                // The wire format is a flat list of {type, key, value} items (existing, published
                // contract). OrcaMCPPresetConfigUtils::ApplyConfig operates on a batch of settings
                // per type ({"type":..., "settings": {key: value, ...}}), so group by type here,
                // preserving each type's first-seen order.
                //
                // Grouping collapses a key listed twice for the same type into one write, so the
                // earlier values are dropped. That is still the behaviour (last one wins, as it
                // would be with two separate calls), but it is reported instead of silent: a
                // caller that builds a batch programmatically otherwise has no way to notice that
                // half of it never took effect.
                std::vector<std::string> type_order;
                std::map<std::string, nlohmann::json> grouped_settings;
                std::vector<std::pair<std::string, std::string>> duplicate_order;  // (type, key), first seen
                std::map<std::pair<std::string, std::string>, int> occurrences;
                for (const auto& item : settings) {
                    const std::string type = item.value("type", "");
                    const std::string key = item.value("key", "");
                    if (grouped_settings.find(type) == grouped_settings.end()) {
                        grouped_settings[type] = nlohmann::json::object();
                        type_order.push_back(type);
                    }
                    const int count = ++occurrences[{type, key}];
                    if (count == 2)
                        duplicate_order.push_back({type, key});
                    grouped_settings[type][key] = item.at("value");
                }

                nlohmann::json duplicate_keys = nlohmann::json::array();
                for (const auto& [type, key] : duplicate_order) {
                    duplicate_keys.push_back({
                        {"type", type},
                        {"key", key},
                        {"occurrences", occurrences[{type, key}]},
                        {"applied_value", grouped_settings[type][key]}
                    });
                }

                nlohmann::json applied_keys = nlohmann::json::array();
                nlohmann::json invalid_keys = nlohmann::json::array();
                nlohmann::json unknown_keys = nlohmann::json::array();
                nlohmann::json rejected_values = nlohmann::json::array();
                bool has_error = false;
                bool has_invalid = false;
                // Writing a slot's colour rewrites filament_multi_colour and filament_colour_type
                // with it, which turns a gradient into one flat colour. That is the right thing to
                // do -- it is what the colour picker does -- but a caller that is never told has no
                // way to know a spool's second colour is gone.
                std::vector<int> flattened_slots;
                std::vector<std::string> color_errors;

                for (const auto& type : type_order) {
                    nlohmann::json config_item = {{"type", type}, {"settings", grouped_settings[type]}};
                    ApplyConfigResult result = OrcaMCPPresetConfigUtils::ApplyConfig(config_item);
                    if (!result.error.empty()) {
                        has_error = true;
                    }
                    if (!result.invalid.empty()) {
                        has_invalid = true;
                    }
                    for (const auto& key : result.applied) applied_keys.push_back(key);
                    for (const auto& key : result.invalid) invalid_keys.push_back(key);
                    for (const auto& key : result.unknown) unknown_keys.push_back(key);
                    for (const auto& rejected : result.rejected)
                        rejected_values.push_back({{"key", rejected.key},
                                                   {"reason", rejected.reason},
                                                   {"expected", rejected.expected}});
                    flattened_slots.insert(flattened_slots.end(), result.flattened_slots.begin(),
                                           result.flattened_slots.end());
                    color_errors.insert(color_errors.end(), result.color_errors.begin(), result.color_errors.end());
                }
                OrcaMCPPresetConfigUtils::UpdatePresetTabs();

                std::string status = has_error ? "error" : (has_invalid ? "partial" : "success");

                Plater* plater = wxGetApp().plater();
                nlohmann::json response = {
                    {"status", status},
                    {"applied_keys", applied_keys},
                    {"invalid_keys", invalid_keys},
                    // invalid_keys is the union of the two, kept because it is the published field.
                    // These two say which problem it was: a key that does not exist, or a value
                    // this key would not take -- and for the second, the shape that would work.
                    {"unknown_keys", unknown_keys},
                    {"rejected_values", rejected_values},
                    {"duplicate_keys", duplicate_keys},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
                auto info_messages = suppression_guard.messages();
                if (!flattened_slots.empty()) {
                    response["flattened_gradient_slots"] = flattened_slots;
                    for (int slot : flattened_slots)
                        info_messages.push_back("Filament slot " + std::to_string(slot) +
                                                " held a multi-colour gradient; it is now the single colour that "
                                                "was written.");
                }
                // A colour that could not be written to all three keys leaves the slot's colour,
                // swatch and preview disagreeing, which is exactly what those keys exist to prevent.
                if (!color_errors.empty()) {
                    if (status == "success")
                        response["status"] = "partial";
                    for (const auto& message : color_errors)
                        info_messages.push_back(message);
                }
                if (!info_messages.empty()) {
                    response["info_messages"] = info_messages;
                }
                return response;
            });
        }
    });

    // clone_preset - Clone/duplicate an existing preset
    register_tool({
        "clone_preset",
        "Clone a preset with a new name.",
        {
            {"type", "object"},
            {"properties", {
                {"type", {
                    {"type", "string"},
                    {"enum", {"printer", "filament", "print"}},
                    {"description", "Preset type"}
                }},
                {"source_name", {
                    {"type", "string"},
                    {"description", "Source preset name"}
                }},
                {"new_name", {
                    {"type", "string"},
                    {"description", "New preset name"}
                }}
            }},
            {"required", {"type", "source_name", "new_name"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string type = params["type"];
            std::string source_name = params["source_name"];
            std::string new_name = params["new_name"];
            return run_on_main_thread([type, source_name, new_name]() {
                McpDialogSuppressionGuard suppression_guard;
                try {
                    OrcaMCPPresetConfigUtils::ClonePreset(type, source_name, new_name);
                    auto info_messages = suppression_guard.messages();
                    nlohmann::json response = {
                        {"status", "success"},
                        {"message", "Preset '" + source_name + "' cloned to '" + new_name + "'"},
                        {"cloned_preset", new_name}
                    };
                    if (!info_messages.empty()) {
                        response["info_messages"] = info_messages;
                    }
                    return response;
                } catch (const std::exception& e) {
                    auto info_messages = suppression_guard.messages();
                    nlohmann::json response = {{"status", "error"}, {"error", e.what()}};
                    if (!info_messages.empty()) {
                        response["info_messages"] = info_messages;
                    }
                    return response;
                }
            });
        }
    });

    // save_preset - Save dirty changes to a preset
    register_tool({
        "save_preset",
        "Save dirty changes to preset. Optionally save as new name.",
        {
            {"type", "object"},
            {"properties", {
                {"type", {
                    {"type", "string"},
                    {"enum", {"printer", "filament", "print"}},
                    {"description", "Preset type"}
                }},
                {"name", {
                    {"type", "string"},
                    {"description", "Save as new name. If omitted, saves to current preset."}
                }}
            }},
            {"required", {"type"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string type = params["type"];
            std::string name = params.value("name", "");
            return run_on_main_thread([type, name]() {
                McpDialogSuppressionGuard suppression_guard;
                try {
                    OrcaMCPPresetConfigUtils::SavePreset(type, name);
                    auto info_messages = suppression_guard.messages();
                    std::string saved_name = name.empty() ? "current preset" : name;
                    nlohmann::json response = {
                        {"status", "success"},
                        {"message", "Preset saved successfully"},
                        {"saved_preset", saved_name}
                    };
                    if (!info_messages.empty()) {
                        response["info_messages"] = info_messages;
                    }
                    return response;
                } catch (const std::exception& e) {
                    auto info_messages = suppression_guard.messages();
                    nlohmann::json response = {{"status", "error"}, {"error", e.what()}};
                    if (!info_messages.empty()) {
                        response["info_messages"] = info_messages;
                    }
                    return response;
                }
            });
        }
    });

    // delete_preset - Delete a user-created preset
    register_tool({
        "delete_preset",
        "Delete a user-created preset.",
        {
            {"type", "object"},
            {"properties", {
                {"type", {
                    {"type", "string"},
                    {"enum", {"printer", "filament", "print"}},
                    {"description", "Preset type"}
                }},
                {"name", {
                    {"type", "string"},
                    {"description", "Preset name"}
                }}
            }},
            {"required", {"type", "name"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string type = params["type"];
            std::string name = params["name"];
            return run_on_main_thread([type, name]() {
                McpDialogSuppressionGuard suppression_guard;
                try {
                    OrcaMCPPresetConfigUtils::DeletePreset(type, name);
                    auto info_messages = suppression_guard.messages();
                    nlohmann::json response = {
                        {"status", "success"},
                        {"message", "Preset '" + name + "' deleted successfully"}
                    };
                    if (!info_messages.empty()) {
                        response["info_messages"] = info_messages;
                    }
                    return response;
                } catch (const std::exception& e) {
                    auto info_messages = suppression_guard.messages();
                    nlohmann::json response = {{"status", "error"}, {"error", e.what()}};
                    if (!info_messages.empty()) {
                        response["info_messages"] = info_messages;
                    }
                    return response;
                }
            });
        }
    });

    // reset_preset - Discard dirty changes and revert to saved state
    register_tool({
        "reset_preset",
        "Discard unsaved preset changes.",
        {
            {"type", "object"},
            {"properties", {
                {"type", {
                    {"type", "string"},
                    {"enum", {"printer", "filament", "print"}},
                    {"description", "Preset type"}
                }}
            }},
            {"required", {"type"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string type = params["type"];
            return run_on_main_thread([type]() {
                McpDialogSuppressionGuard suppression_guard;
                try {
                    OrcaMCPPresetConfigUtils::ResetPreset(type);
                    auto info_messages = suppression_guard.messages();
                    nlohmann::json response = {
                        {"status", "success"},
                        {"message", "Preset changes discarded for " + type}
                    };
                    if (!info_messages.empty()) {
                        response["info_messages"] = info_messages;
                    }
                    return response;
                } catch (const std::exception& e) {
                    auto info_messages = suppression_guard.messages();
                    nlohmann::json response = {{"status", "error"}, {"error", e.what()}};
                    if (!info_messages.empty()) {
                        response["info_messages"] = info_messages;
                    }
                    return response;
                }
            });
        }
    });

    // ==================== MODEL MANIPULATION ====================

    // auto_orient - Auto-orient all objects
    register_tool({
        "auto_orient",
        "Automatically orient all objects for optimal printing",
        {
            {"type", "object"},
            {"properties", {
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            bool include_preview = params.value("include_preview", false);
            return run_on_main_thread([include_preview]() {
                Plater* plater = wxGetApp().plater();
                plater->set_prepare_state(Job::PREPARE_STATE_MENU);
                plater->orient();

                nlohmann::json result = {
                    {"status", "orient_started"},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Add preview if requested
                if (include_preview) {
                    add_turntable_preview_if_requested(result, true);
                    result["preview_hint"] = "Check the preview image to see how objects are now oriented on the plate.";
                }

                return result;
            });
        }
    });

    // arrange_objects - Arrange objects on plate
    register_tool({
        "arrange_objects",
        "Automatically arrange all objects on the build plate",
        {
            {"type", "object"},
            {"properties", {
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            bool include_preview = params.value("include_preview", false);
            return run_on_main_thread([include_preview]() {
                Plater* plater = wxGetApp().plater();
                plater->set_prepare_state(Job::PREPARE_STATE_MENU);
                plater->arrange();

                nlohmann::json result = {
                    {"status", "arrange_started"},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Add preview if requested
                if (include_preview) {
                    add_turntable_preview_if_requested(result, true);
                    result["preview_hint"] = "Check the preview image to see the new arrangement of objects on the plate.";
                }

                return result;
            });
        }
    });

    // undo - Undo last operation
    register_tool({
        "undo",
        "Undo the last operation.",
        {
            {"type", "object"},
            {"properties", {
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            bool include_preview = params.value("include_preview", false);
            return run_on_main_thread([include_preview]() {
                Plater* plater = wxGetApp().plater();
                plater->undo();
                nlohmann::json result = {
                    {"status", "success"},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
                add_turntable_preview_if_requested(result, include_preview);
                return result;
            });
        }
    });

    // redo - Redo last undone operation
    register_tool({
        "redo",
        "Redo the last undone operation",
        {
            {"type", "object"},
            {"properties", {
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            bool include_preview = params.value("include_preview", false);
            return run_on_main_thread([include_preview]() {
                Plater* plater = wxGetApp().plater();
                plater->redo();
                nlohmann::json result = {
                    {"status", "success"},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
                add_turntable_preview_if_requested(result, include_preview);
                return result;
            });
        }
    });

    // ==================== PER-OBJECT SETTINGS ====================

    // get_object_config - Get per-object settings
    register_tool({
        "get_object_config",
        "Get per-object setting overrides.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            return run_on_main_thread([object_id]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];
                const DynamicPrintConfig& cfg = obj->config.get();

                nlohmann::json config_json = nlohmann::json::object();
                for (const std::string& key : cfg.keys()) {
                    config_json[key] = cfg.opt_serialize(key);
                }

                return nlohmann::json{
                    {"object_id", object_id},
                    {"object_name", obj->name},
                    {"config", config_json},
                    {"has_overrides", !cfg.keys().empty()}
                };
            });
        }
    });

    // set_object_config - Set per-object settings (supports batch)
    register_tool({
        "set_object_config",
        "Set per-object setting overrides.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"settings", {
                    {"type", "array"},
                    {"description", "Settings array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"key", {{"type", "string"}}},
                            {"value", {{"type", "string"}, {"description", "Value"}}}
                        }},
                        {"required", {"key", "value"}},
                        {"additionalProperties", false}
                    }}
                }},
                {"configs", {
                    {"type", "array"},
                    {"description", "Batch configs array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"object_id", {{"type", "integer"}}},
                            {"settings", {
                                {"type", "array"},
                                {"items", {
                                    {"type", "object"},
                                    {"properties", {
                                        {"key", {{"type", "string"}}},
                                        {"value", {{"type", "string"}}}
                                    }},
                                    {"required", {"key", "value"}},
                                    {"additionalProperties", false}
                                }}
                            }}
                        }},
                        {"required", {"object_id", "settings"}},
                        {"additionalProperties", false}
                    }}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            // Build list of configs to process
            std::vector<std::pair<int, nlohmann::json>> config_list;

            if (params.contains("configs") && params["configs"].is_array()) {
                for (const auto& cfg : params["configs"]) {
                    config_list.push_back({cfg["object_id"].get<int>(), cfg["settings"]});
                }
            } else if (params.contains("object_id") && params.contains("settings")) {
                config_list.push_back({params["object_id"].get<int>(), params["settings"]});
            } else {
                return nlohmann::json{
                    {"status", "error"},
                    {"message", "Either (object_id + settings) or configs array is required"}
                };
            }

            return run_on_main_thread([config_list]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();
                ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Enable);

                nlohmann::json results = nlohmann::json::array();
                bool any_changes = false;

                for (const auto& [object_id, settings] : config_list) {
                    nlohmann::json obj_result;
                    obj_result["object_id"] = object_id;

                    if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                        obj_result["status"] = "error";
                        obj_result["message"] = "Invalid object_id";
                        results.push_back(obj_result);
                        continue;
                    }

                    ModelObject* obj = model.objects[object_id];
                    std::vector<std::string> applied_keys;
                    std::vector<std::string> invalid_keys;
                    // Same shape as apply_config: a key listed twice for one object is applied
                    // twice, last value wins, and the caller is told rather than left guessing.
                    std::vector<std::string> duplicate_order;
                    std::map<std::string, int> occurrences;

                    for (const auto& item : settings) {
                        std::string key = item["key"];
                        if (++occurrences[key] == 2)
                            duplicate_order.push_back(key);
                        std::string value_str = item["value"].is_string() ?
                            item["value"].get<std::string>() : item["value"].dump();

                        try {
                            obj->config.set_deserialize(key, value_str, context);
                            if (obj->config.has(key)) {
                                applied_keys.push_back(key);
                            } else {
                                invalid_keys.push_back(key);
                            }
                        } catch (...) {
                            invalid_keys.push_back(key);
                        }
                    }

                    if (!applied_keys.empty()) {
                        wxGetApp().obj_list()->changed_object(object_id);
                        any_changes = true;
                    }

                    obj_result["status"] = invalid_keys.empty() ? "success" : "partial";
                    obj_result["applied_count"] = applied_keys.size();
                    obj_result["applied_keys"] = applied_keys;
                    if (!invalid_keys.empty()) {
                        obj_result["invalid_keys"] = invalid_keys;
                    }
                    nlohmann::json duplicate_keys = nlohmann::json::array();
                    for (const std::string& key : duplicate_order)
                        duplicate_keys.push_back({{"key", key}, {"occurrences", occurrences[key]}});
                    obj_result["duplicate_keys"] = duplicate_keys;
                    results.push_back(obj_result);
                }

                if (any_changes) {
                    plater->update();
                }

                // Return single result for single config, array for batch
                if (config_list.size() == 1) {
                    return results[0];
                }

                return nlohmann::json{
                    {"status", "success"},
                    {"objects_processed", results.size()},
                    {"results", results}
                };
            });
        }
    });

    // reset_object_config - Remove per-object overrides
    register_tool({
        "reset_object_config",
        "Remove per-object setting overrides.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"keys", {
                    {"type", "array"},
                    {"description", "Keys to reset. If omitted, resets all overrides."},
                    {"items", {{"type", "string"}}}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            std::vector<std::string> keys;
            if (params.contains("keys") && params["keys"].is_array()) {
                for (const auto& k : params["keys"]) {
                    keys.push_back(k.get<std::string>());
                }
            }
            return run_on_main_thread([object_id, keys]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];
                int reset_count = 0;

                if (keys.empty()) {
                    // Reset all overrides
                    const auto all_keys = obj->config.get().keys();
                    reset_count = all_keys.size();
                    for (const auto& key : all_keys) {
                        obj->config.erase(key);
                    }
                } else {
                    // Reset specific keys
                    for (const auto& key : keys) {
                        if (obj->config.has(key)) {
                            obj->config.erase(key);
                            reset_count++;
                        }
                    }
                }

                // Notify UI of changes
                wxGetApp().obj_list()->changed_object(object_id);
                plater->update();

                return nlohmann::json{
                    {"status", "success"},
                    {"object_id", object_id},
                    {"reset_count", reset_count}
                };
            });
        }
    });

    // get_valid_config_keys - Get valid configuration keys for settings
    register_tool({
        "get_valid_config_keys",
        "Get valid configuration keys.",
        {
            {"type", "object"},
            {"properties", {
                {"category", {
                    {"type", "string"},
                    {"description", "per_object, print, filament, printer, toolchanger, project, or all"}
                }},
                {"include_descriptions", {
                    {"type", "boolean"},
                    {"description", "Include key descriptions"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string category = params.value("category", "per_object");
            bool include_descriptions = params.value("include_descriptions", false);

            return run_on_main_thread([category, include_descriptions]() {
                nlohmann::json result;
                result["category"] = category;

                // Get the config definition
                const ConfigDef& def = print_config_def;

                // Commonly used per-object settings (subset that makes sense for per-object overrides)
                static const std::set<std::string> per_object_keys = {
                    "layer_height", "initial_layer_print_height", "adaptive_layer_height",
                    "wall_loops", "top_shell_layers", "bottom_shell_layers",
                    "sparse_infill_density", "sparse_infill_pattern",
                    "enable_support", "support_type", "support_style",
                    "brim_type", "brim_width",
                    "seam_position", "xy_hole_compensation", "xy_contour_compensation",
                    "ironing_type", "detect_thin_wall", "detect_overhang_wall",
                    "wall_infill_order", "bridge_no_support", "max_bridge_length",
                    "thick_bridges", "internal_bridge_support_thickness",
                    "fuzzy_skin", "fuzzy_skin_thickness", "fuzzy_skin_point_dist",
                    "extruder", "wall_filament", "sparse_infill_filament",
                    "solid_infill_filament", "support_filament", "support_interface_filament"
                };

                // toolchanger_keys / project_keys live in OrcaMCPConfigKeys.hpp so they can be
                // shared with get_toolchanger_config without duplicating the lists.

                nlohmann::json keys_array = nlohmann::json::array();

                for (const auto& [key, opt_def] : def.options) {
                    bool include_key = false;

                    if (category == "all") {
                        include_key = true;
                    } else if (category == "per_object") {
                        include_key = per_object_keys.count(key) > 0;
                    } else if (category == "print") {
                        // PrintConfig keys - slicer settings
                        include_key = opt_def.mode == comSimple || opt_def.mode == comAdvanced || opt_def.mode == comDevelop;
                    } else if (category == "filament") {
                        include_key = key.find("filament") != std::string::npos ||
                                     key.find("temperature") != std::string::npos ||
                                     key.find("fan") != std::string::npos ||
                                     key.find("cooling") != std::string::npos;
                    } else if (category == "printer") {
                        include_key = key.find("machine") != std::string::npos ||
                                     key.find("retract") != std::string::npos ||
                                     key.find("gcode") != std::string::npos ||
                                     key.find("nozzle") != std::string::npos;
                    } else if (category == "toolchanger") {
                        include_key = toolchanger_keys.count(key) > 0;
                    } else if (category == "project") {
                        include_key = project_keys.count(key) > 0;
                    }

                    if (include_key) {
                        nlohmann::json key_info;
                        key_info["key"] = key;

                        // Convert type to string
                        std::string type_str;
                        switch (opt_def.type) {
                            case coFloat: type_str = "float"; break;
                            case coFloats: type_str = "floats"; break;
                            case coInt: type_str = "int"; break;
                            case coInts: type_str = "ints"; break;
                            case coString: type_str = "string"; break;
                            case coStrings: type_str = "strings"; break;
                            case coPercent: type_str = "percent"; break;
                            case coPercents: type_str = "percents"; break;
                            case coBool: type_str = "bool"; break;
                            case coBools: type_str = "bools"; break;
                            case coEnum: type_str = "enum"; break;
                            case coFloatOrPercent: type_str = "float_or_percent"; break;
                            case coFloatsOrPercents: type_str = "floats_or_percents"; break;
                            case coPoint: type_str = "point"; break;
                            case coPoints: type_str = "points"; break;
                            default: type_str = "unknown"; break;
                        }
                        key_info["type"] = type_str;

                        if (include_descriptions && !opt_def.tooltip.empty()) {
                            key_info["description"] = opt_def.tooltip;
                        }

                        // Add enum values if applicable
                        if (opt_def.type == coEnum && opt_def.enum_keys_map) {
                            nlohmann::json enum_values = nlohmann::json::array();
                            for (const auto& ev : opt_def.enum_values) {
                                enum_values.push_back(ev);
                            }
                            if (!enum_values.empty()) {
                                key_info["enum_values"] = enum_values;
                            }
                        }

                        keys_array.push_back(key_info);
                    }
                }

                result["keys"] = keys_array;
                result["count"] = keys_array.size();

                return result;
            });
        }
    });

    // get_object_layer_ranges - Get layer-range-specific configs
    register_tool({
        "get_object_layer_ranges",
        "Get layer-range settings for an object.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            return run_on_main_thread([object_id]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];
                nlohmann::json ranges_array = nlohmann::json::array();

                for (const auto& [range, config] : obj->layer_config_ranges) {
                    nlohmann::json config_json = nlohmann::json::object();
                    const DynamicPrintConfig& cfg = config.get();
                    for (const std::string& key : cfg.keys()) {
                        config_json[key] = cfg.opt_serialize(key);
                    }

                    ranges_array.push_back({
                        {"z_min", range.first},
                        {"z_max", range.second},
                        {"config", config_json}
                    });
                }

                return nlohmann::json{
                    {"object_id", object_id},
                    {"object_name", obj->name},
                    {"layer_ranges", ranges_array}
                };
            });
        }
    });

    // set_object_layer_range - Set layer-range-specific settings
    register_tool({
        "set_object_layer_range",
        "Set settings for a Z height range.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"z_min", {
                    {"type", "number"},
                    {"description", "Min Z height (mm)"}
                }},
                {"z_max", {
                    {"type", "number"},
                    {"description", "Max Z height (mm)"}
                }},
                {"settings", {
                    {"type", "array"},
                    {"description", "Settings array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"key", {{"type", "string"}, {"description", "Key name"}}},
                            {"value", {{"type", "string"}, {"description", "Value"}}}
                        }},
                        {"required", {"key", "value"}},
                        {"additionalProperties", false}
                    }}
                }}
            }},
            {"required", {"object_id", "z_min", "z_max", "settings"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            double z_min = params["z_min"];
            double z_max = params["z_max"];
            nlohmann::json settings = params["settings"];
            return run_on_main_thread([object_id, z_min, z_max, settings]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];
                t_layer_height_range range = {z_min, z_max};
                ModelConfig& layer_cfg = obj->layer_config_ranges[range];

                ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Enable);
                for (const auto& item : settings) {
                    std::string key = item["key"];
                    std::string value_str = item["value"].is_string() ?
                        item["value"].get<std::string>() : item["value"].dump();
                    layer_cfg.set_deserialize(key, value_str, context);
                }

                // Notify UI of changes
                wxGetApp().obj_list()->changed_object(object_id);
                plater->update();

                return nlohmann::json{
                    {"status", "success"},
                    {"object_id", object_id},
                    {"range", {z_min, z_max}},
                    {"applied_count", settings.size()}
                };
            });
        }
    });

    // delete_object_layer_range - Remove layer-range config
    register_tool({
        "delete_object_layer_range",
        "Remove layer range config. If z_min/z_max omitted, removes ALL ranges.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"z_min", {
                    {"type", "number"},
                    {"description", "Min Z height (mm). Omit both to delete all ranges."}
                }},
                {"z_max", {
                    {"type", "number"},
                    {"description", "Max Z height (mm). Omit both to delete all ranges."}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            bool has_range = params.contains("z_min") && params.contains("z_max");
            double z_min = has_range ? params["z_min"].get<double>() : 0;
            double z_max = has_range ? params["z_max"].get<double>() : 0;
            return run_on_main_thread([object_id, has_range, z_min, z_max]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];
                int deleted_count = 0;

                if (has_range) {
                    // Delete specific range
                    t_layer_height_range range = {z_min, z_max};
                    if (obj->layer_config_ranges.erase(range) > 0) {
                        deleted_count = 1;
                    }
                } else {
                    // Delete all ranges
                    deleted_count = obj->layer_config_ranges.size();
                    obj->layer_config_ranges.clear();
                }

                // Notify UI of changes
                wxGetApp().obj_list()->changed_object(object_id);
                plater->update();

                return nlohmann::json{
                    {"status", "success"},
                    {"object_id", object_id},
                    {"deleted_count", deleted_count}
                };
            });
        }
    });

    // ==================== VARIABLE LAYER HEIGHT ====================

    // apply_adaptive_layer_height - Apply VLH to objects (supports batch)
    register_tool({
        "apply_adaptive_layer_height",
        "Apply Variable Layer Height based on geometry.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"object_ids", {
                    {"type", "array"},
                    {"items", {{"type", "integer"}}},
                    {"description", "Object indices for batch"}
                }},
                {"quality", {
                    {"type", "number"},
                    {"description", "0.0 (speed) to 1.0 (quality). Default: 0.5"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            // Build list of object IDs to process
            std::vector<int> object_ids;
            if (params.contains("object_ids") && params["object_ids"].is_array()) {
                for (const auto& id : params["object_ids"]) {
                    object_ids.push_back(id.get<int>());
                }
            } else if (params.contains("object_id")) {
                object_ids.push_back(params["object_id"].get<int>());
            } else {
                return nlohmann::json{
                    {"status", "error"},
                    {"message", "Either object_id or object_ids is required"}
                };
            }

            float quality = params.value("quality", 0.5f);
            bool include_preview = params.value("include_preview", false);

            return run_on_main_thread([object_ids, quality, include_preview]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                // Clamp quality to valid range
                float clamped_quality = std::max(0.0f, std::min(1.0f, quality));

                // Get slicing config once (shared across all objects)
                DynamicPrintConfig full_config;
                full_config.apply(wxGetApp().preset_bundle->prints.get_edited_preset().config);
                full_config.apply(wxGetApp().preset_bundle->filaments.get_edited_preset().config);
                full_config.apply(wxGetApp().preset_bundle->printers.get_edited_preset().config);

                nlohmann::json results = nlohmann::json::array();

                for (int object_id : object_ids) {
                    nlohmann::json obj_result;
                    obj_result["object_id"] = object_id;

                    if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                        obj_result["status"] = "error";
                        obj_result["message"] = "Invalid object_id";
                        results.push_back(obj_result);
                        continue;
                    }

                    ModelObject* obj = model.objects[object_id];

                    float object_max_z = static_cast<float>(obj->max_z());
                    Vec3d shrinkage(1.0, 1.0, 1.0);

                    SlicingParameters slicing_params = PrintObject::slicing_parameters(
                        full_config, *obj, object_max_z, shrinkage);

                    // Generate adaptive layer height profile
                    std::vector<double> profile = layer_height_profile_adaptive(slicing_params, *obj, clamped_quality);

                    // Set the profile on the model object
                    obj->layer_height_profile.set(profile);

                    // Notify UI of changes
                    wxGetApp().obj_list()->update_info_items(object_id);

                    // Calculate profile statistics
                    double min_layer_height = slicing_params.max_layer_height;
                    double max_layer_height = slicing_params.min_layer_height;

                    for (size_t i = 1; i < profile.size(); i += 2) {
                        double h = profile[i];
                        if (h > 0) {
                            min_layer_height = std::min(min_layer_height, h);
                            max_layer_height = std::max(max_layer_height, h);
                        }
                    }

                    int layer_count = 0;
                    if (profile.size() >= 4) {
                        double total_z = profile[profile.size() - 2];
                        double avg_height = (min_layer_height + max_layer_height) / 2.0;
                        layer_count = static_cast<int>(total_z / avg_height);
                    }

                    obj_result["status"] = "success";
                    obj_result["object_name"] = obj->name;
                    obj_result["vlh_enabled"] = true;
                    obj_result["min_layer_height"] = min_layer_height;
                    obj_result["max_layer_height"] = max_layer_height;
                    obj_result["estimated_layer_count"] = layer_count;
                    obj_result["profile_points"] = profile.size() / 2;
                    results.push_back(obj_result);
                }

                // Schedule background process once (after all objects processed)
                GLCanvas3D* canvas = plater->get_view3D_canvas3D();
                if (canvas) {
                    canvas->post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
                }
                plater->update();

                // Return single result for single object, array for batch
                nlohmann::json result;
                if (object_ids.size() == 1) {
                    result = results[0];
                    result["quality_factor"] = clamped_quality;
                } else {
                    result = nlohmann::json{
                        {"status", "success"},
                        {"quality_factor", clamped_quality},
                        {"objects_processed", results.size()},
                        {"results", results}
                    };
                }

                // Add preview if requested
                if (include_preview) {
                    add_turntable_preview_if_requested(result, true);
                    result["preview_hint"] = "Check the preview image to visually verify the VLH changes on the plate.";
                }

                return result;
            });
        }
    });

    // clear_adaptive_layer_height - Remove VLH from objects (supports batch)
    register_tool({
        "clear_adaptive_layer_height",
        "Remove Variable Layer Height, revert to fixed.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"object_ids", {
                    {"type", "array"},
                    {"items", {{"type", "integer"}}},
                    {"description", "Object indices for batch"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Include a preview image path to visually verify the result"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            // Build list of object IDs to process
            std::vector<int> object_ids;
            if (params.contains("object_ids") && params["object_ids"].is_array()) {
                for (const auto& id : params["object_ids"]) {
                    object_ids.push_back(id.get<int>());
                }
            } else if (params.contains("object_id")) {
                object_ids.push_back(params["object_id"].get<int>());
            } else {
                return nlohmann::json{
                    {"status", "error"},
                    {"message", "Either object_id or object_ids is required"}
                };
            }

            bool include_preview = params.value("include_preview", false);

            return run_on_main_thread([object_ids, include_preview]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                nlohmann::json results = nlohmann::json::array();

                for (int object_id : object_ids) {
                    nlohmann::json obj_result;
                    obj_result["object_id"] = object_id;

                    if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                        obj_result["status"] = "error";
                        obj_result["message"] = "Invalid object_id";
                        results.push_back(obj_result);
                        continue;
                    }

                    ModelObject* obj = model.objects[object_id];

                    bool had_vlh = !obj->layer_height_profile.get().empty();
                    obj->layer_height_profile.clear();
                    wxGetApp().obj_list()->update_info_items(object_id);

                    obj_result["status"] = "success";
                    obj_result["object_name"] = obj->name;
                    obj_result["vlh_enabled"] = false;
                    obj_result["previous_vlh_active"] = had_vlh;
                    results.push_back(obj_result);
                }

                // Schedule background process once
                GLCanvas3D* canvas = plater->get_view3D_canvas3D();
                if (canvas) {
                    canvas->post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
                }
                plater->update();

                // Return single result for single object, array for batch
                nlohmann::json result;
                if (object_ids.size() == 1) {
                    result = results[0];
                } else {
                    result = nlohmann::json{
                        {"status", "success"},
                        {"objects_processed", results.size()},
                        {"results", results}
                    };
                }

                // Add preview if requested
                if (include_preview) {
                    add_turntable_preview_if_requested(result, true);
                    result["preview_hint"] = "Check the preview image to verify VLH has been cleared from the object(s).";
                }

                return result;
            });
        }
    });

    // ==================== SLICING & EXPORT ====================

    // slice_all - Start slicing
    register_tool({
        "slice_all",
        "Start slicing all plates. Poll get_slicing_status until done.",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([]() {
                Plater* plater = wxGetApp().plater();

                // Suppress any dialogs during slicing initiation
                McpDialogSuppressionGuard suppression_guard;
                plater->reslice();
                auto info_messages = suppression_guard.messages();

                nlohmann::json result = {
                    {"status", "slicing_started"},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
                if (!info_messages.empty()) {
                    result["info_messages"] = info_messages;
                }
                return result;
            });
        }
    });

    // export_gcode - Export G-code
    register_tool({
        "export_gcode",
        "Export G-code. Requires slicing complete.",
        {
            {"type", "object"},
            {"properties", {
                {"output_path", {
                    {"type", "string"},
                    {"description", "Output path (required; file dialogs cannot be opened from MCP)."}
                }}
            }},
            {"required", {"output_path"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string output_path = params.value("output_path", "");
            return run_on_main_thread([output_path]() {
                Plater* plater = wxGetApp().plater();
                if (plater->is_background_process_slicing()) {
                    return nlohmann::json{{"status", "error"}, {"message", "Slicing still in progress"}};
                }

                // Enable dialog suppression to capture any error messages
                McpDialogSuppressionGuard suppression_guard;

                nlohmann::json result;

                if (!output_path.empty()) {
                    // Silent export to specific path
                    bool success = plater->export_gcode_to_file(output_path);
                    auto info_messages = suppression_guard.messages();

                    if (success) {
                        result["status"] = "export_started";
                        result["output_path"] = output_path;
                        result["note"] = "G-code export started. The file will be written asynchronously.";
                    } else {
                        result["status"] = "error";
                        result["message"] = "Failed to start G-code export. Check that slicing completed successfully.";
                    }
                    if (!info_messages.empty()) {
                        result["info_messages"] = info_messages;
                    }
                } else {
                    // No path provided. File dialogs are modal and would block the GUI thread
                    // for as long as the MCP call waits, so require an explicit path instead.
                    result["status"] = "error";
                    result["message"] = "output_path is required: file dialogs cannot be opened from MCP.";
                }

                return result;
            });
        }
    });

    // export_3mf - Export project as 3MF
    register_tool({
        "export_3mf",
        "Export project as 3MF file.",
        {
            {"type", "object"},
            {"properties", {
                {"output_path", {
                    {"type", "string"},
                    {"description", "Output path (required; file dialogs cannot be opened from MCP)."}
                }}
            }},
            {"required", {"output_path"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string output_path = params.value("output_path", "");
            return run_on_main_thread([output_path]() {
                Plater* plater = wxGetApp().plater();

                // Enable dialog suppression to capture any error messages
                McpDialogSuppressionGuard suppression_guard;

                nlohmann::json result;

                if (!output_path.empty()) {
                    if (!boost::iends_with(output_path, ".3mf")) {
                        return nlohmann::json{{"status", "error"},
                                              {"message", "output_path must end in .3mf, got \"" + output_path + "\""}};
                    }
                    const std::string name_before = into_u8(plater->get_project_filename(".3mf"));

                    // Silent export with path
                    int export_result = plater->export_3mf(boost::filesystem::path(output_path), SaveStrategy::Silence | SaveStrategy::SplitModel);

                    auto info_messages = suppression_guard.messages();

                    if (export_result == 0) {
                        // SaveStrategy::Silence skips Plater's own naming, so do it here: this is
                        // the API's save-project operation, and save_project can then save in place.
                        plater->set_project_filename(wxString::FromUTF8(output_path));
                        result["status"] = "success";
                        result["output_path"] = output_path;
                        // Naming the project is not a side effect a caller can be expected to guess:
                        // it retitles the window, adds the file to Recent Projects, and makes both
                        // save_project and a Cmd-S in the GUI overwrite this file from now on.
                        if (output_path != name_before) {
                            result["project_renamed_to"] = output_path;
                            info_messages.push_back("The project is now named " + output_path +
                                                    ": export_3mf is this API's Save, so save_project and the GUI's "
                                                    "Save both write there from now on.");
                        }
                    } else {
                        result["status"] = "error";
                        result["message"] = "Failed to export the project to " + output_path +
                                            ". Check that the folder exists and is writable.";
                    }
                    if (!info_messages.empty()) {
                        result["info_messages"] = info_messages;
                    }
                } else {
                    // No path provided. File dialogs are modal and would block the GUI thread
                    // for as long as the MCP call waits, so require an explicit path instead.
                    result["status"] = "error";
                    result["message"] = "output_path is required: file dialogs cannot be opened from MCP.";
                }

                return result;
            });
        }
    });

    // save_project - Save current project
    register_tool({
        "save_project",
        "Save the current project. Saves in place once the project has a file name; pass "
        "output_path to name it (or to save a copy under a new name).",
        {
            {"type", "object"},
            {"properties", {
                {"output_path", {
                    {"type", "string"},
                    {"description", "Path of the .3mf to save to. Required while the project has no "
                                    "file name; naming it any other way needs a file dialog, which "
                                    "MCP cannot open. Also acts as Save As."}
                }},
                {"save_as", {
                    {"type", "boolean"},
                    {"description", "Legacy, ignored: use output_path to save under a new name."}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const std::string output_path = params.value("output_path", std::string());
            return run_on_main_thread([output_path]() -> nlohmann::json {
                Plater* plater = wxGetApp().plater();

                // Suppress any dialogs during save
                McpDialogSuppressionGuard suppression_guard;

                const std::string current_name = into_u8(plater->get_project_filename(".3mf"));
                const std::string target = output_path.empty() ? current_name : output_path;
                if (target.empty()) {
                    return nlohmann::json{
                        {"status", "error"},
                        {"message", "This project has no file name yet, and MCP cannot open the file "
                                    "dialog that would ask for one. Call save_project again with "
                                    "output_path set to the .3mf path to save to."}};
                }
                if (!boost::iends_with(target, ".3mf")) {
                    return nlohmann::json{{"status", "error"},
                                          {"message", "output_path must end in .3mf, got \"" + target + "\""}};
                }

                // Naming the project first turns save_project into the Save As the GUI would do
                // after its file dialog; with the name already set it saves in place.
                const bool renamed = target != current_name;
                if (renamed)
                    plater->set_project_filename(wxString::FromUTF8(target));

                int result = plater->save_project(false);
                auto info_messages = suppression_guard.messages();

                nlohmann::json response;
                if (result == wxID_YES) {
                    response = {{"status", "success"}, {"filename", into_u8(plater->get_project_filename(".3mf"))}};
                    if (renamed) {
                        response["project_renamed_to"] = target;
                        info_messages.push_back("The project is now named " + target +
                                                ": save_project and the GUI's Save both write there from now on.");
                    }
                } else {
                    response = {{"status", "error"},
                                {"message", std::string("Failed to save the project to ") + target +
                                            ". Check that the folder exists and is writable." +
                                            (renamed ? " The project has been renamed to that path even though the "
                                                       "save failed."
                                                     : "")}};
                }
                if (!info_messages.empty()) {
                    response["info_messages"] = info_messages;
                }
                return response;
            });
        }
    });

    // load_model - Import 3D model file
    register_tool({
        "load_model",
        "Import a 3D model file (STL, 3MF, OBJ, STEP, etc.)",
        {
            {"type", "object"},
            {"properties", {
                {"file_path", {
                    {"type", "string"},
                    {"description", "Path to model file"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }},
            {"required", {"file_path"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string file_path = params["file_path"];
            bool include_preview = params.value("include_preview", false);
            return run_on_main_thread([file_path, include_preview]() {
                Plater* plater = wxGetApp().plater();
                wxArrayString files;
                files.Add(wxString::FromUTF8(file_path));

                // Suppress dialogs and capture info messages
                McpDialogSuppressionGuard suppression_guard;
                bool result = plater->load_files(files);
                auto info_messages = suppression_guard.messages();

                nlohmann::json response;
                if (result) {
                    response = {
                        {"status", "success"},
                        {"file", file_path},
                        {"active_warnings", get_active_warnings_json(plater)}
                    };
                    // Add turntable preview if requested
                    add_turntable_preview_if_requested(response, include_preview);
                } else {
                    response = {
                        {"status", "error"},
                        {"message", "Failed to load model file"},
                        {"active_warnings", get_active_warnings_json(plater)}
                    };
                }

                // Add any captured info messages
                if (!info_messages.empty()) {
                    response["info_messages"] = info_messages;
                }

                return response;
            });
        }
    });

    // get_slicing_status - Check slicing progress
    register_tool({
        "get_slicing_status",
        "Get the current slicing state: idle (not sliced), slicing (in progress) or done (the "
        "current plate has a valid slice result). Poll until state is done, then get_print_estimate.",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([]() {
                Plater* plater = wxGetApp().plater();
                PartPlate* plate = plater->get_partplate_list().get_curr_plate();
                const bool is_running = plater->is_background_process_slicing();
                // "not running" is not "finished": before the first slice, and after any edit
                // invalidates the result, the background process is equally idle. The plate's own
                // slice-result validity is what the GUI's Print/Export buttons use, so use it here.
                const bool has_result = plate != nullptr && plate->is_slice_result_valid();

                nlohmann::json result = {
                    {"is_slicing", is_running},
                    {"state", is_running ? "slicing" : (has_result ? "done" : "idle")},
                    {"status", is_running ? "slicing" : "idle"},  // kept for older callers
                    {"plate_index", plater->get_partplate_list().get_curr_plate_index()},
                    {"slice_result_valid", has_result},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                return result;
            });
        }
    });

    // get_print_estimate - Get print time and filament estimates after slicing
    register_tool({
        "get_print_estimate",
        "Get print time and filament estimates for the current plate. Requires a valid slice "
        "result (get_slicing_status state \"done\").",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([]() -> nlohmann::json {
                Plater* plater = wxGetApp().plater();

                // Check if slicing is actively running
                if (plater->is_background_process_slicing()) {
                    return nlohmann::json{
                        {"status", "in_progress"},
                        {"state", "slicing"},
                        {"message", "Slicing still in progress. Poll get_slicing_status until state is \"done\"."}
                    };
                }

                // Plater::fff_print() is the Plater's own Print object, which nothing ever slices:
                // every plate owns its Print (PartPlate::set_print) and the background process is
                // pointed at it by PartPlate::update_slice_context. Asking the Plater's copy
                // whether it finished the G-code export therefore always answered "no", which is
                // what left this tool reporting in_progress forever after a completed slice.
                PartPlateList& plate_list = plater->get_partplate_list();
                PartPlate*     plate      = plate_list.get_curr_plate();
                if (plate == nullptr) {
                    return nlohmann::json{{"status", "error"}, {"state", "idle"}, {"message", "No current plate"}};
                }
                GCodeProcessorResult* slice_result = plate->get_slice_result();
                if (!plate->is_slice_result_valid() || slice_result == nullptr) {
                    return nlohmann::json{
                        {"status", "error"},
                        {"state", "idle"},
                        {"message", "The current plate has no valid slice result. Run slice_all and poll "
                                    "get_slicing_status until state is \"done\"."},
                        {"active_warnings", get_active_warnings_json(plater)}
                    };
                }

                const PrintEstimatedStatistics& ps = slice_result->print_statistics;
                const double normal_time = ps.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
                const double silent_time = ps.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time;

                const SliceEstimate estimate = compute_slice_estimate(
                    ps.total_volumes_per_extruder, slice_result->filament_diameters,
                    slice_result->filament_densities, slice_result->filament_costs);

                // A property the slicer did not record is reported as null, never as zero.
                auto number_or_null = [](const std::optional<double>& value) {
                    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
                };

                nlohmann::json per_filament = nlohmann::json::array();
                for (const FilamentUsage& usage : estimate.per_filament) {
                    per_filament.push_back({
                        {"filament", static_cast<int>(usage.filament_id) + 1},  // 1-based, as every other filament tool
                        {"volume_mm3", usage.volume_mm3},
                        {"length_mm", number_or_null(usage.length_mm)},
                        {"weight_grams", number_or_null(usage.weight_g)},
                        {"cost", number_or_null(usage.cost)}
                    });
                }

                // Layer count comes from the plate's own Print, the one that was actually sliced.
                size_t total_layers = 0;
                if (const Print* print = plate->fff_print())
                    for (const PrintObject* obj : print->objects())
                        total_layers = std::max(total_layers, obj->total_layer_count());

                return nlohmann::json{
                    {"status", "success"},
                    {"state", "done"},
                    {"plate_index", plate_list.get_curr_plate_index()},
                    {"estimated_time", get_time_dhms(static_cast<float>(normal_time))},
                    {"estimated_time_seconds", normal_time},
                    {"estimated_time_silent", silent_time > 0.0 ? nlohmann::json(get_time_dhms(static_cast<float>(silent_time)))
                                                                : nlohmann::json(nullptr)},
                    {"layer_count", total_layers},
                    {"filament", {
                        {"total_length_mm", number_or_null(estimate.length_mm)},
                        {"total_volume_mm3", estimate.volume_mm3},
                        {"total_weight_grams", number_or_null(estimate.weight_g)},
                        {"total_cost", number_or_null(estimate.cost)},
                        {"per_filament", per_filament}
                    }},
                    {"total_toolchanges", ps.total_filament_changes},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
            });
        }
    });

    // new_project - Create new project
    register_tool({
        "new_project",
        "Create a new empty project.",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([]() {
                Plater* plater = wxGetApp().plater();

                // Suppress dialogs (like "save unsaved changes?") and capture messages
                McpDialogSuppressionGuard suppression_guard;
                plater->new_project();
                auto info_messages = suppression_guard.messages();

                nlohmann::json response = {{"status", "success"}};
                if (!info_messages.empty()) {
                    response["info_messages"] = info_messages;
                }
                return response;
            });
        }
    });

    // load_project - Load 3MF project file
    register_tool({
        "load_project",
        "Load a 3MF project file",
        {
            {"type", "object"},
            {"properties", {
                {"file_path", {
                    {"type", "string"},
                    {"description", "Path to 3MF file"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }},
            {"required", {"file_path"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string file_path = params["file_path"];
            bool include_preview = params.value("include_preview", false);
            return run_on_main_thread([file_path, include_preview]() {
                Plater* plater = wxGetApp().plater();

                // Suppress dialogs and capture info messages
                McpDialogSuppressionGuard suppression_guard;

                // Use load_project with "<silence>" to suppress dialogs via LoadStrategy::Silence
                // This ensures both MCP suppression AND the silence flag are active
                plater->load_project(wxString::FromUTF8(file_path), "<silence>");

                auto info_messages = suppression_guard.messages();

                // Check if project loaded by seeing if there are objects
                bool result = !plater->model().objects.empty();

                // LoadStrategy::Silence skips Plater's own set_project_filename, which would leave
                // the project nameless (wrong window title, and save_project could never save in
                // place). Name it after the file we just opened.
                if (result)
                    plater->set_project_filename(wxString::FromUTF8(file_path));

                nlohmann::json response = {
                    {"status", result ? "success" : "error"},
                    {"file", file_path}
                };

                // Say so: from here on save_project and the GUI's Save write back to this file.
                if (result) {
                    response["project_renamed_to"] = file_path;
                    info_messages.push_back("The project is now named " + file_path +
                                            ": save_project and the GUI's Save both write there from now on.");
                }

                // Add turntable preview if requested and load succeeded
                if (result) {
                    add_turntable_preview_if_requested(response, include_preview);
                }

                // Add any captured info messages
                if (!info_messages.empty()) {
                    response["info_messages"] = info_messages;
                }

                // Add active warnings
                response["active_warnings"] = get_active_warnings_json(plater);

                return response;
            });
        }
    });

    // ==================== PLATE MANAGEMENT ====================

    // add_plate - Create a new plate
    register_tool({
        "add_plate",
        "Create a new plate.",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([]() {
                Plater* plater = wxGetApp().plater();
                PartPlateList& plate_list = plater->get_partplate_list();

                // Capture state before creation
                int previous_plate_count = plate_list.get_plate_count();
                int current_plate_before = plate_list.get_curr_plate_index();

                // Create the new plate
                int new_index = plate_list.create_plate(true);
                int total_plates = plate_list.get_plate_count();
                int current_plate_after = plate_list.get_curr_plate_index();

                return nlohmann::json{
                    {"status", "success"},
                    {"new_plate_index", new_index},
                    {"previous_plate_count", previous_plate_count},
                    {"total_plates", total_plates},
                    {"current_plate_before", current_plate_before},
                    {"current_plate_after", current_plate_after},
                    {"note", "New plate " + std::to_string(new_index) + " created. Use select_plate to switch to it if needed."}
                };
            });
        }
    });

    // delete_plate - Delete a plate
    register_tool({
        "delete_plate",
        "Delete a plate. Cannot delete last plate. Objects moved to another plate.",
        {
            {"type", "object"},
            {"properties", {
                {"plate_index", {
                    {"type", "integer"},
                    {"description", "Plate index. If omitted, deletes current plate."}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int plate_index = params.value("plate_index", -1);
            return run_on_main_thread([plate_index]() {
                Plater* plater = wxGetApp().plater();
                PartPlateList& plate_list = plater->get_partplate_list();

                // Capture state before deletion
                int plate_count_before = plate_list.get_plate_count();
                int current_plate_before = plate_list.get_curr_plate_index();
                int actual_plate_to_delete = (plate_index == -1) ? current_plate_before : plate_index;

                // Check if we have more than one plate
                if (plate_count_before <= 1) {
                    return nlohmann::json{
                        {"status", "error"},
                        {"message", "Cannot delete the last remaining plate"},
                        {"total_plates", plate_count_before}
                    };
                }

                // Validate plate index
                if (actual_plate_to_delete < 0 || actual_plate_to_delete >= plate_count_before) {
                    return nlohmann::json{
                        {"status", "error"},
                        {"message", "Invalid plate_index: " + std::to_string(actual_plate_to_delete) +
                                    ". Valid range: 0 to " + std::to_string(plate_count_before - 1)}
                    };
                }

                int result = plater->delete_plate(plate_index);
                if (result == 0) {
                    int current_plate_after = plate_list.get_curr_plate_index();
                    int plate_count_after = plate_list.get_plate_count();

                    nlohmann::json response = {
                        {"status", "success"},
                        {"deleted_plate_index", actual_plate_to_delete},
                        {"plate_count_before", plate_count_before},
                        {"plate_count_after", plate_count_after},
                        {"current_plate_before", current_plate_before},
                        {"current_plate_after", current_plate_after}
                    };

                    // Add note about plate indices shifting
                    if (actual_plate_to_delete < plate_count_before - 1) {
                        response["note"] = "Plates after index " + std::to_string(actual_plate_to_delete) +
                                          " have shifted down. Re-query get_scene_info for updated indices.";
                    }

                    return response;
                } else {
                    return nlohmann::json{
                        {"status", "error"},
                        {"message", "Failed to delete plate"}
                    };
                }
            });
        }
    });

    // select_plate - Select/switch to a plate
    register_tool({
        "select_plate",
        "Select a plate as current.",
        {
            {"type", "object"},
            {"properties", {
                {"plate_index", {
                    {"type", "integer"},
                    {"description", "Plate index (0-based)"}
                }}
            }},
            {"required", {"plate_index"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int plate_index = params["plate_index"];
            return run_on_main_thread([plate_index]() {
                Plater* plater = wxGetApp().plater();
                PartPlateList& plate_list = plater->get_partplate_list();

                // Capture state before selection
                int previous_plate = plate_list.get_curr_plate_index();
                int plate_count = plate_list.get_plate_count();

                if (plate_index < 0 || plate_index >= plate_count) {
                    return nlohmann::json{
                        {"status", "error"},
                        {"message", "Invalid plate_index: " + std::to_string(plate_index) +
                                    ". Valid range: 0 to " + std::to_string(plate_count - 1)},
                        {"current_plate", previous_plate},
                        {"total_plates", plate_count}
                    };
                }

                // Check if already on requested plate
                if (plate_index == previous_plate) {
                    return nlohmann::json{
                        {"status", "success"},
                        {"previous_plate", previous_plate},
                        {"current_plate", plate_index},
                        {"total_plates", plate_count},
                        {"note", "Already on plate " + std::to_string(plate_index) + ", no change needed."}
                    };
                }

                int result = plater->select_plate(plate_index);
                int current_plate = plate_list.get_curr_plate_index();

                nlohmann::json response = {
                    {"status", result == 0 ? "success" : "error"},
                    {"previous_plate", previous_plate},
                    {"current_plate", current_plate},
                    {"total_plates", plate_count}
                };

                if (result == 0 && previous_plate != current_plate) {
                    response["note"] = "Switched from plate " + std::to_string(previous_plate) +
                                      " to plate " + std::to_string(current_plate) + ".";
                }

                return response;
            });
        }
    });

    // ==================== OBJECT TRANSFORMS ====================

    // move_object - Move/translate an object
    register_tool({
        "move_object",
        "Move object by offset (relative) or to position (relative=false).",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"x", {
                    {"type", "number"},
                    {"description", "X in mm"}
                }},
                {"y", {
                    {"type", "number"},
                    {"description", "Y in mm"}
                }},
                {"z", {
                    {"type", "number"},
                    {"description", "Z in mm"}
                }},
                {"relative", {
                    {"type", "boolean"},
                    {"description", "true (default)=offset, false=absolute position"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }},
                {"preview_views", {
                    {"type", "integer"},
                    {"description", "Views: 4 or 8 (default: 4)"}
                }},
                {"preview_resolution", {
                    {"type", "integer"},
                    {"description", "Resolution in pixels (default: 256)"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            bool has_x = params.contains("x");
            bool has_y = params.contains("y");
            bool has_z = params.contains("z");
            double x = params.value("x", 0.0);
            double y = params.value("y", 0.0);
            double z = params.value("z", 0.0);
            bool relative = params.value("relative", true);
            bool include_preview = params.value("include_preview", false);
            int preview_views = params.value("preview_views", 4);
            int preview_resolution = params.value("preview_resolution", 256);
            return run_on_main_thread([object_id, x, y, z, has_x, has_y, has_z, relative,
                                       include_preview, preview_views, preview_resolution]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];
                BoundingBoxf3 bbox = obj->bounding_box_approx();
                Vec3d current_center = bbox.center();

                if (relative) {
                    // Relative: only apply offset for specified axes (unspecified = 0 offset)
                    obj->translate(Vec3d(x, y, z));
                } else {
                    // Absolute: only change specified axes, preserve others
                    Vec3d target(
                        has_x ? x : current_center.x(),
                        has_y ? y : current_center.y(),
                        has_z ? z : current_center.z()
                    );
                    obj->translate(target - current_center);
                }

                // Notify UI of changes
                obj->invalidate_bounding_box();
                plater->update();

                // Get resulting state
                BoundingBoxf3 new_bbox = obj->bounding_box_approx();
                Vec3d new_center = new_bbox.center();
                Vec3d rotation = obj->instances[0]->get_rotation();
                Vec3d scale = obj->instances[0]->get_scaling_factor();

                // Check if object is within printable area
                auto plate = plater->get_partplate_list().get_curr_plate();
                BoundingBoxf3 bed_box = plate->get_plate_box();
                bool on_bed = new_bbox.min.x() >= bed_box.min.x() &&
                              new_bbox.min.y() >= bed_box.min.y() &&
                              new_bbox.max.x() <= bed_box.max.x() &&
                              new_bbox.max.y() <= bed_box.max.y() &&
                              new_bbox.min.z() >= -0.1;  // Allow tiny tolerance for bed contact

                // Build enhanced response with context
                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", object_id},
                    {"movement_mode", relative ? "relative" : "absolute"},
                    {"previous_position", {{"x", current_center.x()}, {"y", current_center.y()}, {"z", current_center.z()}}},
                    {"position", {{"x", new_center.x()}, {"y", new_center.y()}, {"z", new_center.z()}}},
                    {"axes_specified", {{"x", has_x}, {"y", has_y}, {"z", has_z}}},
                    {"rotation_degrees", {
                        {"x", Geometry::rad2deg(rotation.x())},
                        {"y", Geometry::rad2deg(rotation.y())},
                        {"z", Geometry::rad2deg(rotation.z())}
                    }},
                    {"scale", {{"x", scale.x()}, {"y", scale.y()}, {"z", scale.z()}}},
                    {"on_bed", on_bed},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Add movement delta for clarity
                Vec3d delta = new_center - current_center;
                if (delta.norm() > 0.001) {
                    result["movement_delta"] = {{"x", delta.x()}, {"y", delta.y()}, {"z", delta.z()}};
                }

                // Add local warning if off bed
                if (!on_bed) {
                    result["placement_warning"] = "Object positioned outside printable area";
                }

                // Add turntable preview if requested
                add_turntable_preview_if_requested(result, include_preview, preview_views, preview_resolution);

                return result;
            });
        }
    });

    // rotate_object - Rotate an object
    register_tool({
        "rotate_object",
        "Rotate object around X, Y, Z axes (degrees).",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"x", {
                    {"type", "number"},
                    {"description", "X rotation (degrees)"}
                }},
                {"y", {
                    {"type", "number"},
                    {"description", "Y rotation (degrees)"}
                }},
                {"z", {
                    {"type", "number"},
                    {"description", "Z rotation (degrees)"}
                }},
                {"relative", {
                    {"type", "boolean"},
                    {"description", "true=add, false=absolute"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }},
                {"preview_views", {
                    {"type", "integer"},
                    {"description", "Views: 4 or 8 (default: 4)"}
                }},
                {"preview_resolution", {
                    {"type", "integer"},
                    {"description", "Resolution in pixels (default: 256)"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            double x_deg = params.value("x", 0.0);
            double y_deg = params.value("y", 0.0);
            double z_deg = params.value("z", 0.0);
            bool relative = params.value("relative", true);
            (void)relative;  // Reserved for future absolute rotation support
            bool include_preview = params.value("include_preview", false);
            int preview_views = params.value("preview_views", 4);
            int preview_resolution = params.value("preview_resolution", 256);
            return run_on_main_thread([object_id, x_deg, y_deg, z_deg,
                                       include_preview, preview_views, preview_resolution]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];

                // Convert degrees to radians
                const double deg_to_rad = M_PI / 180.0;

                if (x_deg != 0.0) obj->rotate(x_deg * deg_to_rad, Axis::X);
                if (y_deg != 0.0) obj->rotate(y_deg * deg_to_rad, Axis::Y);
                if (z_deg != 0.0) obj->rotate(z_deg * deg_to_rad, Axis::Z);

                // Notify UI of changes
                obj->invalidate_bounding_box();
                plater->update();

                // Get resulting state
                BoundingBoxf3 new_bbox = obj->bounding_box_approx();
                Vec3d new_center = new_bbox.center();
                Vec3d rotation = obj->instances[0]->get_rotation();
                Vec3d scale = obj->instances[0]->get_scaling_factor();

                // Check if object is within printable area
                auto plate = plater->get_partplate_list().get_curr_plate();
                BoundingBoxf3 bed_box = plate->get_plate_box();
                bool on_bed = new_bbox.min.x() >= bed_box.min.x() &&
                              new_bbox.min.y() >= bed_box.min.y() &&
                              new_bbox.max.x() <= bed_box.max.x() &&
                              new_bbox.max.y() <= bed_box.max.y() &&
                              new_bbox.min.z() >= -0.1;

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", object_id},
                    {"position", {{"x", new_center.x()}, {"y", new_center.y()}, {"z", new_center.z()}}},
                    {"rotation_degrees", {
                        {"x", Geometry::rad2deg(rotation.x())},
                        {"y", Geometry::rad2deg(rotation.y())},
                        {"z", Geometry::rad2deg(rotation.z())}
                    }},
                    {"scale", {{"x", scale.x()}, {"y", scale.y()}, {"z", scale.z()}}},
                    {"on_bed", on_bed},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Add local warning if off bed
                if (!on_bed) {
                    result["placement_warning"] = "Object positioned outside printable area";
                }

                // Add turntable preview if requested
                add_turntable_preview_if_requested(result, include_preview, preview_views, preview_resolution);

                return result;
            });
        }
    });

    // scale_object - Scale an object
    register_tool({
        "scale_object",
        "Scale object by axis factors. uniform=true uses x for all.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"x", {
                    {"type", "number"},
                    {"description", "X scale factor"}
                }},
                {"y", {
                    {"type", "number"},
                    {"description", "Y scale factor"}
                }},
                {"z", {
                    {"type", "number"},
                    {"description", "Z scale factor"}
                }},
                {"uniform", {
                    {"type", "boolean"},
                    {"description", "If true, use x for all axes (default: false)"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }},
                {"preview_views", {
                    {"type", "integer"},
                    {"description", "Views: 4 or 8 (default: 4)"}
                }},
                {"preview_resolution", {
                    {"type", "integer"},
                    {"description", "Resolution in pixels (default: 256)"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            double x = params.value("x", 1.0);
            double y = params.value("y", 1.0);
            double z = params.value("z", 1.0);
            bool uniform = params.value("uniform", false);
            bool include_preview = params.value("include_preview", false);
            int preview_views = params.value("preview_views", 4);
            int preview_resolution = params.value("preview_resolution", 256);
            return run_on_main_thread([object_id, x, y, z, uniform,
                                       include_preview, preview_views, preview_resolution]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];

                if (uniform) {
                    obj->scale(x);  // Uniform scale
                } else {
                    obj->scale(Vec3d(x, y, z));
                }

                // Notify UI of changes
                obj->invalidate_bounding_box();
                plater->update();

                // Get resulting state
                BoundingBoxf3 new_bbox = obj->bounding_box_approx();
                Vec3d new_center = new_bbox.center();
                Vec3d rotation = obj->instances[0]->get_rotation();
                Vec3d scale_result = obj->instances[0]->get_scaling_factor();

                // Check if object is within printable area
                auto plate = plater->get_partplate_list().get_curr_plate();
                BoundingBoxf3 bed_box = plate->get_plate_box();
                bool on_bed = new_bbox.min.x() >= bed_box.min.x() &&
                              new_bbox.min.y() >= bed_box.min.y() &&
                              new_bbox.max.x() <= bed_box.max.x() &&
                              new_bbox.max.y() <= bed_box.max.y() &&
                              new_bbox.min.z() >= -0.1;

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", object_id},
                    {"position", {{"x", new_center.x()}, {"y", new_center.y()}, {"z", new_center.z()}}},
                    {"rotation_degrees", {
                        {"x", Geometry::rad2deg(rotation.x())},
                        {"y", Geometry::rad2deg(rotation.y())},
                        {"z", Geometry::rad2deg(rotation.z())}
                    }},
                    {"scale", {{"x", scale_result.x()}, {"y", scale_result.y()}, {"z", scale_result.z()}}},
                    {"on_bed", on_bed},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Add local warning if off bed
                if (!on_bed) {
                    result["placement_warning"] = "Object positioned outside printable area";
                }

                // Add turntable preview if requested
                add_turntable_preview_if_requested(result, include_preview, preview_views, preview_resolution);

                return result;
            });
        }
    });

    // transform_objects - Batch transform multiple objects
    register_tool({
        "transform_objects",
        "Batch transform multiple objects.",
        {
            {"type", "object"},
            {"properties", {
                {"transforms", {
                    {"type", "array"},
                    {"description", "Transform operations"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
                            {"position", {
                                {"type", "object"},
                                {"description", "Absolute position {x, y, z} - unspecified axes preserved"},
                                {"properties", {
                                    {"x", {{"type", "number"}}},
                                    {"y", {{"type", "number"}}},
                                    {"z", {{"type", "number"}}}
                                }},
                                {"additionalProperties", false}
                            }},
                            {"rotation", {
                                {"type", "object"},
                                {"description", "Rotation in degrees {x, y, z} - applied incrementally"},
                                {"properties", {
                                    {"x", {{"type", "number"}}},
                                    {"y", {{"type", "number"}}},
                                    {"z", {{"type", "number"}}}
                                }},
                                {"additionalProperties", false}
                            }},
                            {"scale", {
                                {"type", "object"},
                                {"description", "Scale factors {x, y, z} or {uniform: value}"},
                                {"properties", {
                                    {"x", {{"type", "number"}}},
                                    {"y", {{"type", "number"}}},
                                    {"z", {{"type", "number"}}},
                                    {"uniform", {{"type", "number"}}}
                                }},
                                {"additionalProperties", false}
                            }}
                        }},
                        {"required", {"object_id"}},
                        {"additionalProperties", false}
                    }}
                }}
            }},
            {"required", {"transforms"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            auto transforms = params["transforms"];
            return run_on_main_thread([transforms]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();
                nlohmann::json results = nlohmann::json::array();

                const double deg_to_rad = M_PI / 180.0;

                // Apply all transforms
                for (const auto& t : transforms) {
                    int object_id = t["object_id"];

                    if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                        results.push_back({
                            {"object_id", object_id},
                            {"status", "error"},
                            {"message", "Invalid object_id"}
                        });
                        continue;
                    }

                    ModelObject* obj = model.objects[object_id];

                    // Apply position (absolute, unspecified axes preserved)
                    if (t.contains("position")) {
                        auto pos = t["position"];
                        BoundingBoxf3 bbox = obj->bounding_box_approx();
                        Vec3d current_center = bbox.center();
                        Vec3d target(
                            pos.contains("x") ? pos["x"].get<double>() : current_center.x(),
                            pos.contains("y") ? pos["y"].get<double>() : current_center.y(),
                            pos.contains("z") ? pos["z"].get<double>() : current_center.z()
                        );
                        obj->translate(target - current_center);
                    }

                    // Apply rotation (incremental)
                    if (t.contains("rotation")) {
                        auto rot = t["rotation"];
                        if (rot.contains("x") && rot["x"].get<double>() != 0.0)
                            obj->rotate(rot["x"].get<double>() * deg_to_rad, Axis::X);
                        if (rot.contains("y") && rot["y"].get<double>() != 0.0)
                            obj->rotate(rot["y"].get<double>() * deg_to_rad, Axis::Y);
                        if (rot.contains("z") && rot["z"].get<double>() != 0.0)
                            obj->rotate(rot["z"].get<double>() * deg_to_rad, Axis::Z);
                    }

                    // Apply scale
                    if (t.contains("scale")) {
                        auto sc = t["scale"];
                        if (sc.contains("uniform")) {
                            obj->scale(sc["uniform"].get<double>());
                        } else {
                            double sx = sc.value("x", 1.0);
                            double sy = sc.value("y", 1.0);
                            double sz = sc.value("z", 1.0);
                            obj->scale(Vec3d(sx, sy, sz));
                        }
                    }

                    obj->invalidate_bounding_box();
                }

                // Single UI update for all transforms
                plater->update();

                // Build results for each object
                auto plate = plater->get_partplate_list().get_curr_plate();
                BoundingBoxf3 bed_box = plate->get_plate_box();

                for (const auto& t : transforms) {
                    int object_id = t["object_id"];
                    if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                        continue;  // Already reported error
                    }

                    ModelObject* obj = model.objects[object_id];
                    BoundingBoxf3 bbox = obj->bounding_box_approx();
                    Vec3d center = bbox.center();

                    bool on_bed = bbox.min.x() >= bed_box.min.x() &&
                                  bbox.min.y() >= bed_box.min.y() &&
                                  bbox.max.x() <= bed_box.max.x() &&
                                  bbox.max.y() <= bed_box.max.y() &&
                                  bbox.min.z() >= -0.1;

                    results.push_back({
                        {"object_id", object_id},
                        {"status", "success"},
                        {"position", {{"x", center.x()}, {"y", center.y()}, {"z", center.z()}}},
                        {"on_bed", on_bed}
                    });
                }

                nlohmann::json response = {
                    {"status", "success"},
                    {"results", results},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
                return response;
            });
        }
    });

    // mirror_object - Mirror an object across an axis
    register_tool({
        "mirror_object",
        "Mirror an object across the specified axis",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"axis", {
                    {"type", "string"},
                    {"enum", {"x", "y", "z"}},
                    {"description", "Axis: x, y, or z"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }},
                {"preview_views", {
                    {"type", "integer"},
                    {"description", "Views: 4 or 8 (default: 4)"}
                }},
                {"preview_resolution", {
                    {"type", "integer"},
                    {"description", "Resolution in pixels (default: 256)"}
                }}
            }},
            {"required", {"object_id", "axis"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            std::string axis_str = params["axis"];
            bool include_preview = params.value("include_preview", false);
            int preview_views = params.value("preview_views", 4);
            int preview_resolution = params.value("preview_resolution", 256);
            return run_on_main_thread([object_id, axis_str,
                                       include_preview, preview_views, preview_resolution]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];

                Axis axis;
                if (axis_str == "x" || axis_str == "X") axis = Axis::X;
                else if (axis_str == "y" || axis_str == "Y") axis = Axis::Y;
                else if (axis_str == "z" || axis_str == "Z") axis = Axis::Z;
                else throw std::runtime_error("Invalid axis: " + axis_str);

                obj->mirror(axis);

                // Notify UI of changes
                obj->invalidate_bounding_box();
                plater->update();

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", object_id},
                    {"axis", axis_str},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Add turntable preview if requested
                add_turntable_preview_if_requested(result, include_preview, preview_views, preview_resolution);

                return result;
            });
        }
    });

    // clone_object - Duplicate an object
    register_tool({
        "clone_object",
        "Clone object. duplicate=true for independent copies.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"count", {
                    {"type", "integer"},
                    {"description", "Number of copies (default: 1)"}
                }},
                {"duplicate", {
                    {"type", "boolean"},
                    {"description", "true=independent copies, false (default)=linked instances"}
                }},
                {"destination_plate", {
                    {"type", "integer"},
                    {"description", "Target plate. If omitted, uses current plate."}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            int count = params.value("count", 1);
            bool duplicate = params.value("duplicate", false);
            // Support both "destination_plate" (new) and "target_plate" (legacy) for backward compatibility
            int destination_plate = params.contains("destination_plate") ? params["destination_plate"].get<int>() :
                                    params.value("target_plate", -1);  // -1 means current plate
            bool destination_was_explicit = params.contains("destination_plate") || params.contains("target_plate");
            // -1 is the documented "current plate"; any other negative index is a caller mistake and
            // must not silently become "current plate" (same rule as set_object_filament's volume_id).
            if (destination_was_explicit && destination_plate < -1) {
                return nlohmann::json{{"status", "error"},
                                      {"message", "Invalid destination_plate " + std::to_string(destination_plate) +
                                                  ": use a 0-based plate index, or omit it (or pass -1) for the current plate"}};
            }
            bool include_preview = params.value("include_preview", false);
            return run_on_main_thread([object_id, count, duplicate, destination_plate, destination_was_explicit, include_preview]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                // Determine source and destination plates
                PartPlateList& plate_list = plater->get_partplate_list();
                int current_plate = plate_list.get_curr_plate_index();
                int source_plate = plate_list.find_instance_belongs(object_id, 0);
                if (source_plate < 0) {
                    source_plate = current_plate;  // Fallback to current plate
                }
                // Default to current plate when destination_plate not specified
                int actual_destination = (destination_plate >= 0) ? destination_plate : current_plate;

                if (actual_destination != current_plate) {
                    int plate_count = plate_list.get_plate_count();
                    if (actual_destination >= plate_count) {
                        throw std::runtime_error("Invalid destination_plate: " + std::to_string(actual_destination) +
                                                 " (only " + std::to_string(plate_count) + " plates exist)");
                    }
                    // Select destination plate before cloning
                    plater->select_plate(actual_destination);
                }

                ModelObject* obj = model.objects[object_id];
                nlohmann::json result;

                if (duplicate) {
                    // Create independent copies - each gets its own object_id
                    std::vector<int> new_object_ids;

                    // Calculate position offset if cloning to a different plate
                    Vec3d position_offset(0, 0, 0);
                    if (actual_destination != source_plate) {
                        Vec3d source_origin = plate_list.get_plate(source_plate)->get_origin();
                        Vec3d dest_origin = plate_list.get_plate(actual_destination)->get_origin();
                        position_offset = dest_origin - source_origin;
                    }

                    for (int i = 0; i < count; ++i) {
                        ModelObject* new_obj = model.add_object(*obj);
                        new_obj->name = obj->name;  // Keep the same name

                        // Offset the new object's instances to the destination plate
                        if (position_offset.norm() > 0) {
                            for (ModelInstance* inst : new_obj->instances) {
                                Vec3d current_offset = inst->get_offset();
                                inst->set_offset(current_offset + position_offset);
                            }
                        }

                        int new_id = static_cast<int>(model.objects.size()) - 1;
                        new_object_ids.push_back(new_id);

                        // Register with GUI object list
                        wxGetApp().obj_list()->add_object_to_list(new_id, false, true, false);
                    }

                    // Update UI
                    plater->update();

                    // Arrange to place the new objects on the destination plate
                    plater->set_prepare_state(Job::PREPARE_STATE_MENU);
                    plater->arrange();

                    // Build enhanced response with clear metadata
                    result = {
                        {"status", "success"},
                        {"source_object_id", object_id},
                        {"source_plate", source_plate},
                        {"destination_plate", actual_destination},
                        {"current_plate_at_call", current_plate},
                        {"destination_mode", destination_was_explicit ? "explicit" : "defaulted_to_current"},
                        {"copies_created", count},
                        {"new_object_ids", new_object_ids},
                        {"mode", "duplicate"},
                        {"total_objects", model.objects.size()},
                        {"active_warnings", get_active_warnings_json(plater)}
                    };
                    // Add note if source and destination are the same
                    if (source_plate == actual_destination) {
                        result["note"] = "Clones created on same plate as source (plate " + std::to_string(source_plate) + ")";
                    }
                } else {
                    // Create instances (share transforms) - original behavior
                    plater->select_all();
                    plater->deselect_all();

                    // Calculate position offset if cloning to a different plate
                    Vec3d position_offset(0, 0, 0);
                    if (actual_destination != source_plate) {
                        Vec3d source_origin = plate_list.get_plate(source_plate)->get_origin();
                        Vec3d dest_origin = plate_list.get_plate(actual_destination)->get_origin();
                        position_offset = dest_origin - source_origin;
                    }

                    size_t original_instance_count = obj->instances.size();
                    for (int i = 0; i < count; ++i) {
                        obj->add_instance(*obj->instances.back());

                        // Offset the new instance to the destination plate
                        if (position_offset.norm() > 0) {
                            ModelInstance* new_inst = obj->instances.back();
                            Vec3d current_offset = new_inst->get_offset();
                            new_inst->set_offset(current_offset + position_offset);
                        }
                    }

                    // Notify plate list about new instances
                    if (position_offset.norm() > 0) {
                        for (size_t i = original_instance_count; i < obj->instances.size(); ++i) {
                            plate_list.notify_instance_update(object_id, static_cast<int>(i), true);
                        }
                    }

                    // Arrange to place the new instances on the destination plate
                    plater->set_prepare_state(Job::PREPARE_STATE_MENU);
                    plater->arrange();

                    // Build enhanced response with clear metadata
                    result = {
                        {"status", "success"},
                        {"object_id", object_id},
                        {"source_plate", source_plate},
                        {"destination_plate", actual_destination},
                        {"current_plate_at_call", current_plate},
                        {"destination_mode", destination_was_explicit ? "explicit" : "defaulted_to_current"},
                        {"copies_created", count},
                        {"total_instances", obj->instances.size()},
                        {"mode", "instance"},
                        {"active_warnings", get_active_warnings_json(plater)}
                    };
                    // Add note if source and destination are the same
                    if (source_plate == actual_destination) {
                        result["note"] = "Instances created on same plate as source (plate " + std::to_string(source_plate) + ")";
                    }
                }

                // Add preview if requested
                if (include_preview) {
                    add_turntable_preview_if_requested(result, true);
                    result["preview_hint"] = "Check the preview image to see the cloned object(s) and their arrangement on the plate.";
                }

                return result;
            });
        }
    });

    // get_object_info - Get lightweight info about a single object
    register_tool({
        "get_object_info",
        "Get info about a single object.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            return run_on_main_thread([object_id]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];

                // Get bounding box (used for position and size)
                BoundingBoxf3 bbox = obj->bounding_box_approx();
                Vec3d center = bbox.center();
                Vec3d size = bbox.size();

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", object_id},
                    {"name", obj->name},
                    {"instance_count", static_cast<int>(obj->instances.size())},
                    {"position", {{"x", center.x()}, {"y", center.y()}, {"z", center.z()}}},
                    {"bounding_box", {
                        {"size_x", size.x()},
                        {"size_y", size.y()},
                        {"size_z", size.z()},
                        {"min", {{"x", bbox.min.x()}, {"y", bbox.min.y()}, {"z", bbox.min.z()}}},
                        {"max", {{"x", bbox.max.x()}, {"y", bbox.max.y()}, {"z", bbox.max.z()}}}
                    }}
                };

                // Add transform info from first instance
                if (!obj->instances.empty()) {
                    auto* inst = obj->instances[0];
                    Vec3d rotation = inst->get_rotation();
                    Vec3d scale = inst->get_scaling_factor();

                    result["rotation_degrees"] = {
                        {"x", Geometry::rad2deg(rotation.x())},
                        {"y", Geometry::rad2deg(rotation.y())},
                        {"z", Geometry::rad2deg(rotation.z())}
                    };
                    result["scale"] = {
                        {"x", scale.x()},
                        {"y", scale.y()},
                        {"z", scale.z()}
                    };
                }

                // Check if on bed
                auto plate = plater->get_partplate_list().get_curr_plate();
                BoundingBoxf3 bed_box = plate->get_plate_box();
                bool on_bed = bbox.min.x() >= bed_box.min.x() &&
                              bbox.min.y() >= bed_box.min.y() &&
                              bbox.max.x() <= bed_box.max.x() &&
                              bbox.max.y() <= bed_box.max.y() &&
                              bbox.min.z() >= -0.1;
                result["on_bed"] = on_bed;

                return result;
            });
        }
    });

    // rename_object - Rename an object
    register_tool({
        "rename_object",
        "Rename an object for identification purposes",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"new_name", {
                    {"type", "string"},
                    {"description", "New name"}
                }}
            }},
            {"required", {"object_id", "new_name"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            std::string new_name = params["new_name"];
            return run_on_main_thread([object_id, new_name]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                std::string old_name = model.objects[object_id]->name;
                model.objects[object_id]->name = new_name;

                // Update the object list UI to reflect the new name
                wxGetApp().obj_list()->update_name_for_items();

                return nlohmann::json{
                    {"status", "success"},
                    {"object_id", object_id},
                    {"old_name", old_name},
                    {"new_name", new_name}
                };
            });
        }
    });

    // delete_object - Remove an object from the scene
    register_tool({
        "delete_object",
        "Remove an object from the scene",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            bool include_preview = params.value("include_preview", false);
            return run_on_main_thread([object_id, include_preview]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                std::string deleted_name = model.objects[object_id]->name;
                plater->remove(object_id);

                nlohmann::json result = {
                    {"status", "success"},
                    {"deleted_object_id", object_id},
                    {"deleted_object_name", deleted_name},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Add turntable preview if requested
                add_turntable_preview_if_requested(result, include_preview);

                return result;
            });
        }
    });

    // set_object_printable - Toggle whether an object is included when slicing
    register_tool({
        "set_object_printable",
        "Mark an object printable (included when slicing) or unprintable (skipped). "
        "Useful for excluding specific objects from a print without removing them from the scene.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"printable", {
                    {"type", "boolean"},
                    {"description", "true to include in slicing, false to skip"}
                }}
            }},
            {"required", {"object_id", "printable"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            bool printable = params["printable"];
            return run_on_main_thread([object_id, printable]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* object = model.objects[object_id];
                std::string snapshot_text = (boost::format("%1% \"%2%\"") %
                    (printable ? "Set Object Printable" : "Set Object Unprintable") %
                    object->name).str();
                plater->take_snapshot(snapshot_text);

                for (auto* inst : object->instances)
                    inst->printable = printable;

                wxGetApp().obj_list()->update_printable_state(object_id, 0);
                wxGetApp().plater()->canvas3D()->update_instance_printable_state_for_object(static_cast<size_t>(object_id));
                plater->update();

                return nlohmann::json{
                    {"status", "success"},
                    {"object_id", object_id},
                    {"object_name", object->name},
                    {"printable", printable},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
            });
        }
    });

    // flatten_object - Lay object flat on its best face
    register_tool({
        "flatten_object",
        "Automatically orient an object to lay flat on its best face for printing",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }},
                {"preview_views", {
                    {"type", "integer"},
                    {"description", "Views: 4 or 8 (default: 4)"}
                }},
                {"preview_resolution", {
                    {"type", "integer"},
                    {"description", "Resolution in pixels (default: 256)"}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            bool include_preview = params.value("include_preview", false);
            int preview_views = params.value("preview_views", 4);
            int preview_resolution = params.value("preview_resolution", 256);
            return run_on_main_thread([object_id, include_preview, preview_views, preview_resolution]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                // Use the orient function which auto-orients for optimal printing
                plater->set_prepare_state(Job::PREPARE_STATE_MENU);
                plater->orient();

                nlohmann::json result = {
                    {"status", "orient_started"},
                    {"object_id", object_id},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Add turntable preview if requested
                add_turntable_preview_if_requested(result, include_preview, preview_views, preview_resolution);

                return result;
            });
        }
    });

    // ==================== OBJECT CUTTING ====================

    // cut_object - Cut an object at a specified Z height
    register_tool({
        "cut_object",
        "Cut object at Z height. keep: below, above, or both.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"z_height", {
                    {"type", "number"},
                    {"description", "Cut height in mm"}
                }},
                {"keep", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"below", "above", "both"})},
                    {"description", "below (default), above, or both"}
                }},
                {"include_preview", {
                    {"type", "boolean"},
                    {"description", "Return turntable preview path"}
                }}
            }},
            {"required", nlohmann::json::array({"object_id", "z_height"})}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            double z_height = params["z_height"];
            std::string keep = params.value("keep", "below");
            bool include_preview = params.value("include_preview", false);
            return run_on_main_thread([object_id, z_height, keep, include_preview]() {
                Plater* plater = wxGetApp().plater();
                Model& model = plater->model();

                if (object_id < 0 || object_id >= static_cast<int>(model.objects.size())) {
                    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
                }

                ModelObject* obj = model.objects[object_id];

                // Get instance offset to calculate cut position relative to object
                const Vec3d instance_offset = obj->instances[0]->get_offset();

                // For horizontal cut at z_height (world coords), the cut plane offset
                // relative to instance is (0, 0, z_height - instance_offset.z)
                // Since objects on bed have z_offset=0, this simplifies to (0, 0, z_height)
                Vec3d cut_center_offset(0, 0, z_height - instance_offset.z());

                // Create cut matrix: translation to cut position (no rotation for horizontal cut)
                Transform3d cut_matrix = Geometry::translation_transform(cut_center_offset);

                // Set attributes based on what to keep
                // PlaceOnCut flips the piece so the cut face becomes the new bottom
                // - "below": keep lower part, no flip needed (already on bed)
                // - "above": keep upper part, flip so cut face is down (printable)
                // - "both": keep both, flip both so cut faces are down (both printable)
                ModelObjectCutAttributes attributes;
                if (keep == "above") {
                    attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::PlaceOnCutUpper;
                } else if (keep == "both") {
                    attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                 ModelObjectCutAttribute::PlaceOnCutUpper | ModelObjectCutAttribute::PlaceOnCutLower;
                } else { // below (default)
                    attributes = ModelObjectCutAttribute::KeepLower;  // No flip - bottom already on bed
                }

                // Perform the cut
                Cut cut(obj, 0, cut_matrix, attributes);
                const ModelObjectPtrs& new_objects = cut.perform_with_plane();

                // Add the resulting objects to the model
                for (ModelObject* new_obj : new_objects) {
                    model.add_object(*new_obj);
                }

                // Remove the original object
                std::string original_name = obj->name;
                plater->remove(object_id);

                plater->update();

                nlohmann::json result = {
                    {"status", "success"},
                    {"original_object", original_name},
                    {"z_height", z_height},
                    {"kept", keep},
                    {"new_objects_count", new_objects.size()},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Add turntable preview if requested
                add_turntable_preview_if_requested(result, include_preview);

                return result;
            });
        }
    });

    // ==================== G-CODE VIEW TYPE ====================

    // set_gcode_view_type - Set the G-code preview visualization mode
    register_tool({
        "set_gcode_view_type",
        "Set G-code preview visualization mode. Requires sliced G-code. "
        "Available types: feature_type, speed, actual_speed, fan_speed, temperature, "
        "flow, actual_flow, layer_height, line_width, layer_time, layer_time_log, "
        "pressure_advance, acceleration, jerk, tool, filament",
        {
            {"type", "object"},
            {"properties", {
                {"view_type", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({
                        "feature_type", "speed", "actual_speed", "fan_speed",
                        "temperature", "flow", "actual_flow", "layer_height",
                        "line_width", "layer_time", "layer_time_log",
                        "pressure_advance", "acceleration", "jerk",
                        "tool", "filament"
                    })},
                    {"description", "Visualization mode for G-code preview"}
                }}
            }},
            {"required", nlohmann::json::array({"view_type"})}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            std::string view_type_str = params["view_type"];
            return run_on_main_thread([view_type_str]() {
                Plater* plater = wxGetApp().plater();
                GLCanvas3D* canvas = plater->get_preview_canvas3D();
                if (!canvas)
                    throw std::runtime_error("Preview canvas not available");

                GCodeViewer& viewer = canvas->get_gcode_viewer();

                // Map string to EViewType
                static const std::map<std::string, libvgcode::EViewType> type_map = {
                    {"feature_type",     libvgcode::EViewType::FeatureType},
                    {"speed",            libvgcode::EViewType::Speed},
                    {"actual_speed",     libvgcode::EViewType::ActualSpeed},
                    {"fan_speed",        libvgcode::EViewType::FanSpeed},
                    {"temperature",      libvgcode::EViewType::Temperature},
                    {"flow",             libvgcode::EViewType::VolumetricFlowRate},
                    {"actual_flow",      libvgcode::EViewType::ActualVolumetricFlowRate},
                    {"layer_height",     libvgcode::EViewType::Height},
                    {"line_width",       libvgcode::EViewType::Width},
                    {"layer_time",       libvgcode::EViewType::LayerTimeLinear},
                    {"layer_time_log",   libvgcode::EViewType::LayerTimeLogarithmic},
                    {"pressure_advance", libvgcode::EViewType::PressureAdvance},
                    {"acceleration",     libvgcode::EViewType::Acceleration},
                    {"jerk",             libvgcode::EViewType::Jerk},
                    {"tool",             libvgcode::EViewType::Tool},
                    {"filament",         libvgcode::EViewType::ColorPrint},
                };

                auto it = type_map.find(view_type_str);
                if (it == type_map.end())
                    throw std::runtime_error("Unknown view type: " + view_type_str);

                viewer.set_view_type(it->second);
                canvas->set_as_dirty();
                canvas->request_extra_frame();

                return nlohmann::json{
                    {"status", "success"},
                    {"view_type", view_type_str},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
            });
        }
    });

    register_filament_tools();
    register_printer_tools();

    BOOST_LOG_TRIVIAL(info) << "OrcaMCPServer: Registered " << s_tools.size() << " tools";
}

}} // namespace Slic3r::GUI
