#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPresetConfigUtils.hpp"
#include "OrcaMCPPlateUtils.hpp"
#include "OrcaMCPConfigKeys.hpp"
#include "OrcaMCPFilamentUtils.hpp"
#include "OrcaMCPSliceEstimate.hpp"
#include "OrcaMCPServerInfo.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <cmath>
#include <future>
#include <set>
#include <stdexcept>

namespace Slic3r { namespace GUI {

using namespace Slic3r::GUI::OrcaMCP;

// Static member initialization
std::map<std::string, OrcaMCPServer::ToolDefinition> OrcaMCPServer::s_tools;
bool OrcaMCPServer::s_tools_registered = false;
bool OrcaMCPServer::s_initialized = false;

const std::vector<OrcaMCPServer::ToolCategory>& OrcaMCPServer::all_tool_categories()
{
    static const std::vector<ToolCategory> categories = {
        ToolCategory::Scene,     ToolCategory::Models,      ToolCategory::Transforms,      ToolCategory::Plates,
        ToolCategory::Config,    ToolCategory::PerObject,   ToolCategory::LayerRanges,     ToolCategory::FilamentsColour,
        ToolCategory::Painting,  ToolCategory::Slicing,     ToolCategory::Visualization,   ToolCategory::Printers,
        ToolCategory::Adaptive,  ToolCategory::History,     ToolCategory::Info,
    };
    return categories;
}

const char* OrcaMCPServer::tool_category_name(ToolCategory category)
{
    switch (category) {
    case ToolCategory::Scene:           return "Scene";
    case ToolCategory::Models:          return "Models";
    case ToolCategory::Transforms:      return "Transforms";
    case ToolCategory::Plates:          return "Plates";
    case ToolCategory::Config:          return "Config";
    case ToolCategory::PerObject:       return "Per-Object";
    case ToolCategory::LayerRanges:     return "Layer Ranges";
    case ToolCategory::FilamentsColour: return "Filaments & colour";
    case ToolCategory::Painting:        return "Painting";
    case ToolCategory::Slicing:         return "Slicing";
    case ToolCategory::Visualization:   return "Visualization";
    case ToolCategory::Printers:        return "Printers";
    case ToolCategory::Adaptive:        return "Adaptive";
    case ToolCategory::History:         return "History";
    case ToolCategory::Info:            return "Info";
    }
    return "Unknown";
}

void OrcaMCPServer::init()
{
    if (s_initialized) return;

    BOOST_LOG_TRIVIAL(info) << "OrcaMCPServer: Initializing MCP server";

    // Clean up old preview files from previous sessions
    OrcaMCPPlateUtils::CleanupPreviews();

    ensure_tools_registered();
    s_initialized = true;
}

void OrcaMCPServer::ensure_tools_registered()
{
    if (s_tools_registered) return;
    try {
        register_builtin_tools();
    } catch (...) {
        // A half-filled table would make the next attempt report a duplicate of whichever tool
        // happened to register first, instead of the real fault.
        s_tools.clear();
        throw;
    }
    s_tools_registered = true;
}

const std::map<std::string, OrcaMCPServer::ToolDefinition>& OrcaMCPServer::registered_tools()
{
    ensure_tools_registered();
    return s_tools;
}

std::string OrcaMCPServer::version()
{
    return SoftFever_VERSION;
}

void OrcaMCPServer::register_tool(const ToolDefinition& tool)
{
    // Both are programming errors a unit test catches. A second registration used to replace the
    // first silently, so two tools could share a name and only one of them was reachable.
    if (s_tools.count(tool.name) != 0)
        throw std::logic_error("OrcaMCPServer: tool '" + tool.name + "' is registered twice");
    if (!tool.handler)
        throw std::logic_error("OrcaMCPServer: tool '" + tool.name + "' has no handler");
    s_tools.emplace(tool.name, tool);
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

// The plate slice_all was asked to come back to, or -1 when nothing is pending.
//
// Slicing every plate walks the plate selection from the first plate to the last: upstream's own
// chaining selects the next plate each time one finishes (Plater::priv::on_process_completed). Every
// per-plate tool -- get_print_estimate, export_gcode, get_preview_base64 -- answers about the
// *selected* plate, so leaving the caller on a plate they never chose is how an agent ends up
// reading plate 4's estimate believing it is plate 1's. get_slicing_status puts the selection back
// when the run ends, and says so in its response.
//
// Only ever read or written on the GUI thread, from inside run_on_main_thread.
int s_slice_all_restore_plate = -1;

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

    // Ensure initialized. Registration throws on a malformed tool table; that must fail this
    // request, not escape onto the HTTP worker thread and take the process down.
    if (!s_initialized) {
        try {
            init();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "OrcaMCPServer: " << e.what();
            auto error = make_error_response(nlohmann::json(nullptr), -32603, std::string("Internal error: ") + e.what());
            return std::make_shared<HttpServer::ResponseJson>(error.dump(), 200);
        }
    }

    BOOST_LOG_TRIVIAL(debug) << "OrcaMCPServer: Handling " << method << " " << url;

    // Handle GET /mcp for server info
    if (method == "GET") {
        nlohmann::json info = {
            {"name", "orca-slicer"},
            {"version", version()},
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
            {"version", version()}
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

    // quit_app - close the app from MCP without any dialog. MainFrame::on_close asks "save changes?"
    // and runs other vetoable checks only when the close event can be vetoed; Close(true) cannot be,
    // so nothing modal ever opens. The close is scheduled so this reply reaches the caller first.
    register_tool({
        "quit_app",
        ToolCategory::Info,
        "Quit the app with no dialog",
        "Quit OrcaMCP cleanly with no dialog. By default unsaved project changes are discarded; pass "
        "discard_changes=false to refuse while the project is dirty (call save_project first).",
        {
            {"type", "object"},
            {"properties", {
                {"discard_changes", {
                    {"type", "boolean"},
                    {"description", "Discard unsaved project changes (default true)"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            if (params.contains("discard_changes") && !params.at("discard_changes").is_boolean())
                return nlohmann::json{{"status", "error"}, {"message", "discard_changes must be a boolean"}};
            const bool discard = params.value("discard_changes", true);
            return run_on_main_thread([discard]() -> nlohmann::json {
                Plater* plater = wxGetApp().plater();
                if (!discard && plater != nullptr && plater->is_project_dirty())
                    return nlohmann::json{{"status", "error"}, {"message", "project has unsaved changes; call save_project first or pass discard_changes=true"}};
                wxGetApp().CallAfter([]() {
                    if (Plater* p = wxGetApp().plater(); p != nullptr)
                        p->reset_project_dirty_after_save();
                    if (wxGetApp().mainframe != nullptr)
                        wxGetApp().mainframe->Close(true);
                });
                return nlohmann::json{{"status", "quitting"}};
            });
        }
    });

    // get_server_info - every tool's summary, generated from this registry on each call, and the
    // documentation sections (OrcaMCPServerInfo.cpp)
    register_tool({
        "get_server_info",
        ToolCategory::Info,
        "This guide; pass section for the rest",
        "Get documentation about tools, concepts, and workflows",
        {
            {"type", "object"},
            {"properties", {
                {"section", {
                    {"type", "string"},
                    {"enum", server_info_section_names()},
                    {"description", "One documentation section, or all of them. Omit it for every tool's "
                                    "summary, the quick start, and the list of sections with their sizes."}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return server_info(params, s_tools);
        }
    });

    // ==================== READ OPERATIONS ====================

    // get_scene_info - Get current project state
    register_tool({
        "get_scene_info",
        ToolCategory::Scene,
        "Plates, objects, bed and occupancy",
        "Get current project state: plates, objects, positions. Call first to get object_ids. Each "
        "plate also carries `occupancy`, the complete list of what stands on it in plate "
        "millimetres -- every object's printed footprint (brim included), the prime tower's "
        "footprint (brim included) when one is printed, and the printer's excluded bed areas. Use "
        "`occupancy`, not `model_objects`, to work out where there is free space.",
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
        ToolCategory::Config,
        "List presets; filter by type/vendor/name",
        "List the printer, filament and print presets available for the selected printer. "
        "Returns names and identifying fields only; pass summary:false for full configs. "
        "Capped per type (default 25) -- narrow it with type/vendor/name_contains, or raise limit.",
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
                }},
                {"limit", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"description", "Max presets per type. Default 25 with summary, 5 without. "
                                    "0 = no cap (the unfiltered summary list is ~54,600 characters "
                                    "and overflows most MCP clients)."}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            PresetQuery query;
            query.vendor = params.value("vendor", std::string());
            query.name_contains = params.value("name_contains", std::string());
            if (params.contains("summary") && !parse_boolean_param(params["summary"], query.summary))
                return nlohmann::json{{"status", "error"}, {"message", "summary must be a boolean"}};
            if (const std::string limit_error = parse_preset_limit_param(params, query.limit); !limit_error.empty())
                return nlohmann::json{{"status", "error"}, {"message", limit_error}};

            std::string type = params.value("type", std::string());
            if (type == "all")
                type.clear();
            if (!type.empty() && type != "printer" && type != "filament" && type != "print")
                return nlohmann::json{{"status", "error"},
                                      {"message", "type must be one of: printer, filament, print, all"}};

            return run_on_main_thread([query, type]() -> nlohmann::json {
                nlohmann::json result;
                std::map<std::string, PresetListCount> counts;
                if (type.empty()) {
                    result = OrcaMCPPresetConfigUtils::GetAllPresetJson(query, counts);
                } else if (type == "printer") {
                    result = {{"printerPresets", OrcaMCPPresetConfigUtils::GetPresetsJson(
                                                     Preset::TYPE_PRINTER, query, counts["printerPresets"])}};
                } else if (type == "filament") {
                    result = {{"filamentPresets", OrcaMCPPresetConfigUtils::GetPresetsJson(
                                                      Preset::TYPE_FILAMENT, query, counts["filamentPresets"])}};
                } else {
                    result = {{"printProcessPresets", OrcaMCPPresetConfigUtils::GetPresetsJson(
                                                          Preset::TYPE_PRINT, query, counts["printProcessPresets"])}};
                }
                // Say what was searched and how much of it came back, so a caller can tell an empty
                // list from a filter that was too narrow, and knows the list is already restricted
                // to presets compatible with the selected printer. `counts` keeps its published
                // meaning -- every preset that matched -- and `returned` says how many fit.
                nlohmann::json matched_counts = nlohmann::json::object();
                nlohmann::json returned_counts = nlohmann::json::object();
                for (const auto& [key, count] : counts) {
                    matched_counts[key] = count.matched;
                    returned_counts[key] = count.returned;
                }
                const bool truncated = preset_list_truncated(counts);
                result["query"] = {
                    {"type", type.empty() ? nlohmann::json(nullptr) : nlohmann::json(type)},
                    {"vendor", query.vendor},
                    {"name_contains", query.name_contains},
                    {"summary", query.summary},
                    {"limit", preset_query_effective_limit(query.limit, query.summary)},
                    {"compatible_with_selected_printer_only", true},
                    {"counts", matched_counts},
                    {"returned", returned_counts},
                    {"truncated", truncated}
                };
                const std::string hint = preset_truncation_hint(counts);
                if (!hint.empty())
                    result["hint"] = hint;
                return result;
            });
        }
    });

    // get_edited_presets - Get currently edited presets
    register_tool({
        "get_edited_presets",
        ToolCategory::Config,
        "Active presets and their unsaved edits",
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
        ToolCategory::Visualization,
        "Render a plate or object to PNG",
        "Render a plate. Omit views for a contact sheet of iso, top and front fitted to the plate. A view is {preset: iso|top|front|back|left|right|low, fit: \"plate\" | {object_index}} or explicit {camera_position, target} in BED mm (the get_scene_info frame; plate N sits at plates[N].bounding_box) -- or add frame: \"plate_local\" to give them relative to the plate's front-left corner. Only the requested plate's volumes are drawn. Every view returns objects_in_frame, uniform_image (+hint), plate_origin and the camera; overlays (outline, 10 mm grid, origin, labels) are on by default. Use save_to_file=true for PNG paths.",
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
                {"overlays", {
                    {"type", {"boolean", "object"}},
                    {"description", "Self-locating overlays drawn on the image: plate outline, 10 mm grid, origin with X/Y, numbered object labels, hatched excluded areas. true/omitted = all, false = none, or an object {outline, grid, origin, labels, excluded} of booleans."},
                    {"properties", {
                        {"outline", {{"type", "boolean"}}}, {"grid", {{"type", "boolean"}}}, {"origin", {{"type", "boolean"}}},
                        {"labels", {{"type", "boolean"}}}, {"excluded", {{"type", "boolean"}}}
                    }}
                }},
                {"layer_view", {
                    {"type", "string"},
                    {"enum", {"first_layer"}},
                    {"description", "Instead of a 3D render: a top-down plan of the first layer -- object footprints, brim loops, support and wipe tower -- from the sliced plate (source: sliced) or the model footprints when unsliced (source: footprints). Ignores views. This is the view for 'is the brim wide enough' and 'where do the support feet land'."}
                }},
                {"image_format", {
                    {"type", "string"},
                    {"enum", {"png", "jpeg"}},
                    {"description", "png or jpeg. Default: png when save_to_file is true (crisp overlays, alpha kept), jpeg for inline base64 (smaller)."}
                }},
                {"views", {
                    {"type", "array"},
                    {"description", "Views to render. Omit for the default contact sheet (iso, top, front). Each view: a preset with optional fit, or camera_position + target (bed mm unless frame is plate_local)."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"preset", {
                                {"type", "string"},
                                {"enum", {"iso", "top", "front", "back", "left", "right", "low"}},
                                {"description", "Named camera framing `fit` (default the plate). low = bed level from the front, for first layers and support feet."}
                            }},
                            {"fit", {
                                {"description", "\"plate\" (default) or {\"object_index\": n} to frame one object -- a closer camera beats more pixels."}
                            }},
                            {"frame", {
                                {"type", "string"},
                                {"enum", {"bed_mm", "plate_local"}},
                                {"description", "Frame of camera_position/target: bed_mm (default, the get_scene_info frame) or plate_local (from this plate's front-left corner)."}
                            }},
                            {"camera_position", {
                                {"type", "array"},
                                {"items", {{"type", "number"}}},
                                {"description", "[x, y, z] in bed mm, same frame as get_scene_info"}
                            }},
                            {"target", {
                                {"type", "array"},
                                {"items", {{"type", "number"}}},
                                {"description", "[x, y, z] in bed mm; point it at the plate's objects (get_scene_info positions)"}
                            }}
                        }}
                    }}
                }}
            }},
            {"required", {"plate_index"}}
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
        ToolCategory::Visualization,
        "A preview image as base64",
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
        ToolCategory::Config,
        "Switch a preset, or one filament slot's",
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
        ToolCategory::Config,
        "Change settings, several in one call",
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
        ToolCategory::Config,
        "Copy a preset under a new name",
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
        ToolCategory::Config,
        "Save edited settings to a preset",
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
        ToolCategory::Config,
        "Delete a user preset",
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
        ToolCategory::Config,
        "Discard unsaved preset edits",
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
        ToolCategory::Models,
        "Auto-orient all objects for printing",
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
        ToolCategory::Models,
        "Auto-arrange objects on the plates",
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
        ToolCategory::History,
        "Undo the last operation",
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
        ToolCategory::History,
        "Redo the last undone operation",
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
        ToolCategory::PerObject,
        "An object's setting overrides",
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
        ToolCategory::PerObject,
        "Override settings for one object",
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
                            // No "type" constraint, matching apply_config: a list-typed key takes a
                            // JSON array (see config_value_to_string), so advertising string-only
                            // would have a schema-validating client reject an array the handler
                            // accepts, before it ever reached the bridge.
                            {"value", {{"description", "Value, or an array of values for a list-typed key"}}}
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
                                        {"value", {{"description", "Value, or an array of values for a list-typed key"}}}
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

                    std::vector<std::string> unknown_keys;
                    nlohmann::json rejected_values = nlohmann::json::array();

                    for (const auto& item : settings) {
                        std::string key = item["key"];
                        if (++occurrences[key] == 2)
                            duplicate_order.push_back(key);

                        const ConfigOptionDef* def = print_config_def.get(key);
                        if (def == nullptr) {
                            invalid_keys.push_back(key);
                            unknown_keys.push_back(key);
                            continue;
                        }
                        // Same shaping apply_config uses (config_value_to_string): a list-typed key
                        // takes a JSON array, and a shape that cannot work is reported rather than
                        // stored as the literal text of the array.
                        const ConfigValueText shaped = config_value_to_string(item["value"], def->type);
                        if (!shaped.ok) {
                            invalid_keys.push_back(key);
                            rejected_values.push_back({{"key", key},
                                                       {"reason", shaped.reason},
                                                       {"expected", config_value_expected_shape(def->type)}});
                            continue;
                        }

                        try {
                            obj->config.set_deserialize(key, shaped.text, context);
                            if (obj->config.has(key)) {
                                applied_keys.push_back(key);
                            } else {
                                invalid_keys.push_back(key);
                                rejected_values.push_back({{"key", key},
                                                           {"reason", "the override was not stored"},
                                                           {"expected", config_value_expected_shape(def->type)}});
                            }
                        } catch (const std::exception& e) {
                            invalid_keys.push_back(key);
                            rejected_values.push_back({{"key", key},
                                                       {"reason", std::string("could not be read as a value: ") + e.what()},
                                                       {"expected", config_value_expected_shape(def->type)}});
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
                    // Same split apply_config reports: a key that does not exist, versus a value
                    // this key would not take. invalid_keys stays the union.
                    obj_result["unknown_keys"] = unknown_keys;
                    obj_result["rejected_values"] = rejected_values;
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
        ToolCategory::PerObject,
        "Remove an object's setting overrides",
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
        ToolCategory::Config,
        "Discover setting keys by category",
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
        ToolCategory::LayerRanges,
        "An object's per-height-range settings",
        "Get layer-range settings for an object. Range Z is measured from the object's own base, "
        "not from the bed, so it equals plate Z only while the object sits on the bed.",
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
        ToolCategory::LayerRanges,
        "Settings for a Z range of an object",
        "Set settings for a Z height range. z_min/z_max are measured from the object's own base, "
        "not from the bed, so they equal plate Z only while the object sits on the bed -- moving the "
        "object up does not move its ranges.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"z_min", {
                    {"type", "number"},
                    {"description", "Min Z height (mm) above the object's own base"}
                }},
                {"z_max", {
                    {"type", "number"},
                    {"description", "Max Z height (mm) above the object's own base"}
                }},
                {"settings", {
                    {"type", "array"},
                    {"description", "Settings array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"key", {{"type", "string"}, {"description", "Key name"}}},
                            // No "type" constraint, for the same reason as set_object_config: a
                            // list-typed key takes a JSON array.
                            {"value", {{"description", "Value, or an array of values for a list-typed key"}}}
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
            // Through parse_double_param, not get<double>(): the bridge delivered "1.4" as a string
            // on 2026-09-22 and the bare conversion threw past the handler as an "Internal error".
            double z_min = 0.0, z_max = 0.0;
            if (!parse_double_param(params["z_min"], z_min) || !parse_double_param(params["z_max"], z_max))
                return nlohmann::json{{"status", "error"},
                                      {"message", "z_min and z_max must be finite numbers, in millimetres above the object's base"}};
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
                std::vector<std::string> applied_keys;
                std::vector<std::string> invalid_keys;
                std::vector<std::string> unknown_keys;
                nlohmann::json           rejected_values = nlohmann::json::array();

                for (const auto& item : settings) {
                    const std::string key = item["key"];
                    // Same three checks apply_config and set_object_config make, and for the same
                    // reason: without the print_config_def lookup an unknown key was
                    // indistinguishable from a bad value, without config_value_to_string a
                    // coFloats key silently stored zeros (Config.hpp:935 returns true regardless),
                    // and without the try/catch a throw escaped to the dispatcher and abandoned
                    // the whole call. applied_count reported settings.size() through all three.
                    const ConfigOptionDef* def = print_config_def.get(key);
                    if (def == nullptr) {
                        invalid_keys.push_back(key);
                        unknown_keys.push_back(key);
                        continue;
                    }
                    const ConfigValueText shaped = config_value_to_string(item["value"], def->type);
                    if (!shaped.ok) {
                        invalid_keys.push_back(key);
                        rejected_values.push_back({{"key", key},
                                                   {"reason", shaped.reason},
                                                   {"expected", config_value_expected_shape(def->type)}});
                        continue;
                    }
                    try {
                        layer_cfg.set_deserialize(key, shaped.text, context);
                        applied_keys.push_back(key);
                    } catch (const std::exception& e) {
                        invalid_keys.push_back(key);
                        rejected_values.push_back({{"key", key},
                                                   {"reason", std::string("could not be read as a value: ") + e.what()},
                                                   {"expected", config_value_expected_shape(def->type)}});
                    }
                }

                // Notify UI of changes
                wxGetApp().obj_list()->changed_object(object_id);
                plater->update();

                // "error" only when nothing at all was written -- a range with no settings on it is
                // not the success the old unconditional applied_count claimed it was.
                const char* status = invalid_keys.empty() ? "success"
                                                          : (applied_keys.empty() ? "error" : "partial");
                return nlohmann::json{
                    {"status", status},
                    {"object_id", object_id},
                    {"range", {z_min, z_max}},
                    {"applied_count", applied_keys.size()},
                    {"applied_keys", applied_keys},
                    // invalid_keys is the union, as in apply_config; the two below say which
                    // problem it was.
                    {"invalid_keys", invalid_keys},
                    {"unknown_keys", unknown_keys},
                    {"rejected_values", rejected_values}
                };
            });
        }
    });

    // delete_object_layer_range - Remove layer-range config
    register_tool({
        "delete_object_layer_range",
        ToolCategory::LayerRanges,
        "Remove an object's layer ranges",
        "Remove layer range config. If z_min/z_max omitted, removes ALL ranges. Range Z is measured "
        "from the object's own base, not from the bed.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"z_min", {
                    {"type", "number"},
                    {"description", "Min Z height (mm) above the object's own base. Omit both to delete all ranges."}
                }},
                {"z_max", {
                    {"type", "number"},
                    {"description", "Max Z height (mm) above the object's own base. Omit both to delete all ranges."}
                }}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            int object_id = params["object_id"];
            bool has_range = params.contains("z_min") && params.contains("z_max");
            double z_min = 0.0, z_max = 0.0;
            if (has_range && (!parse_double_param(params["z_min"], z_min) || !parse_double_param(params["z_max"], z_max)))
                return nlohmann::json{{"status", "error"},
                                      {"message", "z_min and z_max must be finite numbers, in millimetres above the object's base"}};
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
        ToolCategory::Adaptive,
        "Variable layer height from geometry",
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
        ToolCategory::Adaptive,
        "Back to a fixed layer height",
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
        ToolCategory::Slicing,
        "Slice every plate, or the selected one",
        "Slice every plate in the project, the way the GUI's Slice All button does: one plate at a "
        "time until all are sliced. Pass all_plates=false to slice only the plate that is currently "
        "selected. Poll get_slicing_status until state is \"done\"; its plates array says which "
        "plates have a result. The plate selection walks from the first plate to the last while the "
        "run is in progress, and get_slicing_status puts back the plate that was selected here once "
        "it ends.",
        {
            {"type", "object"},
            {"properties", {
                {"all_plates", {
                    {"type", "boolean"},
                    {"description", "true (default)=slice every plate, false=only the selected plate"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            bool all_plates = true;
            if (params.contains("all_plates") && !parse_boolean_param(params["all_plates"], all_plates)) {
                return nlohmann::json{
                    {"status", "error"},
                    {"message", "all_plates must be a boolean"}
                };
            }
            return run_on_main_thread([all_plates]() {
                Plater*        plater       = wxGetApp().plater();
                PartPlateList& plate_list   = plater->get_partplate_list();
                const int      plate_count  = plate_list.get_plate_count();
                const int      plate_at_call = plate_list.get_curr_plate_index();

                // Suppress any dialogs during slicing initiation
                McpDialogSuppressionGuard suppression_guard;
                const bool slice_every_plate = all_plates && plate_count > 1;
                if (slice_every_plate) {
                    // Plater::reslice() slices the *current* plate and nothing else, which is what
                    // this tool used to do under the name slice_all: with four plates and plate 4
                    // selected it left plates 1-3 with no slice result and reported success.
                    //
                    // The per-plate chaining lives behind Plater::priv::m_slice_all, which only
                    // on_action_slice_all sets, so the plate walk is driven by dispatching the same
                    // event the Slice All button posts (MainFrame.cpp). Dispatched rather than
                    // posted, so the kick-off still happens inside the suppression guard, exactly as
                    // the reslice() call it replaces did.
                    const bool was_preview_shown = plater->is_preview_shown();
                    s_slice_all_restore_plate    = plate_at_call;
                    SimpleEvent slice_all_event(EVT_GLTOOLBAR_SLICE_ALL);
                    plater->GetEventHandler()->ProcessEvent(slice_all_event);
                    // on_action_slice_all also switches the app to the G-code preview. The plate
                    // renderers read whichever canvas is showing, so a caller that was looking at the
                    // 3D scene is put back there; a caller already in the preview is left alone.
                    if (!was_preview_shown)
                        plater->select_view_3D("3D");
                } else {
                    s_slice_all_restore_plate = -1;
                    plater->reslice();
                }
                auto info_messages = suppression_guard.messages();

                nlohmann::json result = {
                    {"status", "slicing_started"},
                    {"scope", slice_every_plate ? "all_plates" : "current_plate"},
                    {"plates_to_slice", slice_every_plate ? plate_count : 1},
                    {"selected_plate_at_call", plate_at_call},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
                if (slice_every_plate) {
                    result["note"] = "Slicing all " + std::to_string(plate_count) +
                                     " plates. The plate selection walks to the last plate while it "
                                     "runs; get_slicing_status restores plate " +
                                     std::to_string(plate_at_call) + " when the run ends.";
                }
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
        ToolCategory::Slicing,
        "Write the sliced plate's G-code",
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
        ToolCategory::Scene,
        "Write the project to a 3MF file",
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
        ToolCategory::Scene,
        "Save the project, in place or as a copy",
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
        ToolCategory::Models,
        "Import STL, 3MF, OBJ or STEP geometry",
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
        ToolCategory::Slicing,
        "Slicing state per plate; poll this",
        "Get the current slicing state: idle (not sliced), slicing (in progress) or done (the "
        "current plate has a valid slice result). Poll until state is done, then get_print_estimate. "
        "The plates array reports every plate's slice result, so a slice_all run can be followed "
        "plate by plate. When a slice_all run over every plate ends, this restores the plate that "
        "was selected when slice_all was called and reports it as restored_selected_plate.",
        {
            {"type", "object"},
            {"properties", nlohmann::json::object()}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([]() {
                Plater*        plater     = wxGetApp().plater();
                PartPlateList& plate_list = plater->get_partplate_list();
                const bool     is_running = plater->is_background_process_slicing();

                nlohmann::json result;

                // Plater::is_background_process_slicing() reports Plater::priv::m_is_slicing, which
                // the plate walk holds true from the first plate to the last and clears only when the
                // run finishes or stops. So "not running" here means the whole slice_all run is over,
                // not merely that one plate finished, and this is the first moment it is safe to put
                // the caller's plate back. See s_slice_all_restore_plate.
                if (!is_running && s_slice_all_restore_plate >= 0) {
                    const int restore_to      = s_slice_all_restore_plate;
                    s_slice_all_restore_plate = -1;
                    if (restore_to >= 0 && restore_to < plate_list.get_plate_count() &&
                        restore_to != plate_list.get_curr_plate_index()) {
                        plater->select_plate(restore_to);
                        result["restored_selected_plate"] = restore_to;
                    }
                }

                PartPlate* plate = plate_list.get_curr_plate();
                // "not running" is not "finished": before the first slice, and after any edit
                // invalidates the result, the background process is equally idle. The plate's own
                // slice-result validity is what the GUI's Print/Export buttons use, so use it here.
                const bool has_result = plate != nullptr && plate->is_slice_result_valid();

                // Per-plate progress. Without it the only readable answer is about the selected
                // plate, and a slice_all run over four plates has no way to say it is three quarters
                // done -- or which plate failed.
                nlohmann::json plates       = nlohmann::json::array();
                int            plates_sliced = 0;
                const int      plate_count  = plate_list.get_plate_count();
                for (int i = 0; i < plate_count; ++i) {
                    PartPlate* p     = plate_list.get_plate(i);
                    const bool valid = p != nullptr && p->is_slice_result_valid();
                    if (valid)
                        ++plates_sliced;
                    plates.push_back({{"index", i}, {"slice_result_valid", valid}});
                }

                result["is_slicing"]         = is_running;
                result["state"]              = is_running ? "slicing" : (has_result ? "done" : "idle");
                result["status"]             = is_running ? "slicing" : "idle";  // kept for older callers
                result["plate_index"]        = plate_list.get_curr_plate_index();
                result["slice_result_valid"] = has_result;
                result["plates"]             = plates;
                result["plates_sliced"]      = plates_sliced;
                result["plates_total"]       = plate_count;
                result["active_warnings"]    = get_active_warnings_json(plater);

                return result;
            });
        }
    });

    // get_print_estimate - Get print time and filament estimates after slicing
    register_tool({
        "get_print_estimate",
        ToolCategory::Slicing,
        "Print time and filament for a plate",
        "Get print time and filament estimates for one plate. Pass plate_index to ask about a "
        "specific plate; omitted, it reports the plate that is currently selected. Requires a valid "
        "slice result for that plate (get_slicing_status state \"done\"). Tool/filament changes are "
        "reported as two separate counters: extruder_changes (the printer switched physical "
        "extruder/tool head) and filament_changes (a nozzle was loaded with a different filament). A "
        "toolchanger reports the former, a single-nozzle AMS/MMU printer the latter.",
        {
            {"type", "object"},
            {"properties", {
                {"plate_index", {
                    {"type", "integer"},
                    {"description", "Which plate to report, 0-based. Omitted = the currently selected "
                                    "plate. Reading another plate does not change the selection."}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            // Schema "required" is not enforced server-side and an unknown key used to be ignored in
            // silence, so an agent asking for plate 2 was handed plate 1's numbers under plate 1's
            // label. Parse it here, and reject a bad value rather than falling back to the selection.
            bool requested_plate = params.contains("plate_index") && !params["plate_index"].is_null();
            int  wanted_plate    = -1;
            // Through parse_integer_param, not is_number_integer(): a client whose JSON layer
            // widens numbers sends 3 as 3.0, and a cached-schema client can send "3". Rejecting
            // those spellings refuses a call the caller made correctly -- which is exactly what
            // the first cut of this parameter did to the very first live call it received.
            if (requested_plate && !parse_integer_param(params["plate_index"], wanted_plate)) {
                return nlohmann::json{{"status", "error"},
                                      {"message", "plate_index must be an integer"}};
            }
            return run_on_main_thread([requested_plate, wanted_plate]() -> nlohmann::json {
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
                PartPlateList& plate_list  = plater->get_partplate_list();
                const int      plate_count = plate_list.get_plate_count();
                const int      plate_index = requested_plate ? wanted_plate : plate_list.get_curr_plate_index();
                if (requested_plate && (wanted_plate < 0 || wanted_plate >= plate_count)) {
                    return nlohmann::json{
                        {"status", "error"},
                        {"state", "idle"},
                        {"message", "plate_index " + std::to_string(wanted_plate) + " is out of range: the "
                                    "project has " + std::to_string(plate_count) + " plate(s), 0.." +
                                    std::to_string(plate_count - 1) + "."}
                    };
                }
                PartPlate* plate = plate_list.get_plate(plate_index);
                if (plate == nullptr) {
                    return nlohmann::json{{"status", "error"}, {"state", "idle"}, {"message", "No current plate"}};
                }
                GCodeProcessorResult* slice_result = plate->get_slice_result();
                if (!plate->is_slice_result_valid() || slice_result == nullptr) {
                    return nlohmann::json{
                        {"status", "error"},
                        {"state", "idle"},
                        {"plate_index", plate_index},
                        {"message", "Plate " + std::to_string(plate_index) + " has no valid slice result. Run "
                                    "slice_all and poll get_slicing_status until state is \"done\"."},
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
                    {"plate_index", plate_index},
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
                    // Two distinct counters, reported under the names they actually mean. They are
                    // the same two the G-code preview's legend shows as "Filament change times" and
                    // "Tool changes" (GCodeViewer.cpp), and GCodeProcessor keeps them apart:
                    // process_filament_change increments filament_changes only when a nozzle is
                    // loaded with a *different* filament, and extruder_changes only when the printer
                    // switches to a *different* physical extruder. On a toolchanger whose heads each
                    // keep their own filament, filament_changes is legitimately 0 while every tool
                    // change is counted in extruder_changes; on a single-nozzle AMS/MMU machine it is
                    // the other way round. This used to report filament_changes as
                    // "total_toolchanges", which is why a 4-head toolchanger interleaving ABS and a
                    // PETG interface was told it made no tool changes at all.
                    {"filament_changes", ps.total_filament_changes},
                    {"extruder_changes", ps.total_extruder_changes},
                    {"active_warnings", get_active_warnings_json(plater)}
                };
            });
        }
    });

    // new_project - Create new project
    register_tool({
        "new_project",
        ToolCategory::Scene,
        "Start a new, empty project",
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
        ToolCategory::Scene,
        "Open a 3MF as the project",
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
        ToolCategory::Plates,
        "Add a new plate",
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

                // Snapshot first, so a plate added over MCP can be undone like one added from the
                // toolbar. Plater::priv::on_action_add_plate is the reference for this whole block.
                plater->take_snapshot("add partplate");

                // Create the new plate
                int new_index = plate_list.create_plate(true);
                int total_plates = plate_list.get_plate_count();
                int current_plate_after = plate_list.get_curr_plate_index();

                // Not optional. When the plate count crosses a column boundary, create_plate
                // re-flows the grid and rebuilds every plate's picking mesh, which frees the
                // MeshRaycasters the canvas registered. SceneRaycasterItem keeps a raw pointer to
                // those, so the next picking pass -- any idle frame with the mouse over the canvas
                // -- dereferenced freed memory and killed the app. Plater::update() runs
                // reload_scene, which drops every Bed raycaster and re-registers the new ones.
                // The GUI never hit this because every plate action of its own ends in update().
                plater->update();

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
        ToolCategory::Plates,
        "Delete a plate; its objects go unplaced",
        "Delete a plate. Cannot delete the last plate. Objects on it are NOT deleted and NOT moved to another plate: they are moved outside every plate, where get_scene_info lists them under unplaced_objects. Delete them first if you do not want them.",
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

                // The validated index, not the raw parameter: -1 means "current plate" and was
                // resolved above. Plater::delete_plate resolves it too, so this is agreement
                // rather than a fix, but the reported index and the deleted one now match.
                int result = plater->delete_plate(actual_plate_to_delete);
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
        ToolCategory::Plates,
        "Make a plate the current one",
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

    // set_prime_tower_position - Move the prime tower on a plate
    //
    // Without this an agent can see the tower and still not resolve a conflict with it: OrcaSlicer
    // refuses to arrange it and the only way to move it was to drag it in the GUI. The position
    // lives in the per-plate project config keys wipe_tower_x / wipe_tower_y, which are PLATE-LOCAL;
    // this tool speaks plate millimetres like every other tool here and converts.
    register_tool({
        "set_prime_tower_position",
        ToolCategory::Plates,
        "Move a plate's prime tower",
        "Move the prime tower on a plate. x/y are the front-left corner (min x, min y) of the "
        "tower BODY in plate millimetres -- the same frame get_object_info and get_scene_info "
        "report object bounding boxes in, and exactly what get_scene_info reports as "
        "plates[].prime_tower.position. The brim prints outside the body on all four sides; the "
        "position is validated so that the body plus its brim stays inside the printable area. "
        "Objects the new footprint lands on are reported as conflicts rather than refused.",
        {
            {"type", "object"},
            {"properties", {
                {"x", {
                    {"type", "number"},
                    {"description", "Tower body front-left corner X, plate millimetres (world frame, not plate-local)"}
                }},
                {"y", {
                    {"type", "number"},
                    {"description", "Tower body front-left corner Y, plate millimetres (world frame, not plate-local)"}
                }},
                {"plate_index", {
                    {"type", "integer"},
                    {"description", "Plate to move the tower on (0-based). Default: the selected plate."}
                }}
            }},
            {"required", {"x", "y"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            // Schema `required` is not enforced server-side, so every read is guarded.
            if (!params.contains("x") || !params.contains("y"))
                return nlohmann::json{{"status", "error"},
                                      {"message", "x and y are required, in plate millimetres"}};
            double x = 0.0, y = 0.0;
            if (!parse_double_param(params["x"], x) || !parse_double_param(params["y"], y))
                return nlohmann::json{{"status", "error"},
                                      {"message", "x and y must be finite numbers, in plate millimetres"}};

            bool has_plate_index = params.contains("plate_index");
            int  plate_index     = 0;
            if (has_plate_index && !parse_integer_param(params["plate_index"], plate_index))
                return nlohmann::json{{"status", "error"}, {"message", "plate_index must be an integer"}};

            return run_on_main_thread([x, y, has_plate_index, plate_index]() -> nlohmann::json {
                Plater*        plater     = wxGetApp().plater();
                PartPlateList& plate_list = plater->get_partplate_list();
                const int      plate_count = plate_list.get_plate_count();
                const int      index = has_plate_index ? plate_index : plate_list.get_curr_plate_index();

                if (index < 0 || index >= plate_count)
                    return nlohmann::json{{"status", "error"},
                                          {"message", "Invalid plate_index: " + std::to_string(index) +
                                                      ". Valid range: 0 to " + std::to_string(plate_count - 1)}};

                const PrimeTowerState before = OrcaMCPPlateUtils::GetPrimeTowerState(index);
                const Vec2d origin(before.plate_origin.x(), before.plate_origin.y());
                const double local_x = x - origin.x();
                const double local_y = y - origin.y();

                // The range is the one PartPlate::estimate_wipe_tower_polygon clamps into. Accepting
                // a position outside it would silently store a number the slicer then moves, and the
                // caller would read back a position it never asked for.
                const OrcaMCP::PrimeTowerRange& range = before.legal_range;
                auto range_json = [&]() {
                    return nlohmann::json{
                        {"min_x", range.min_x + origin.x()}, {"max_x", range.max_x + origin.x()},
                        {"min_y", range.min_y + origin.y()}, {"max_y", range.max_y + origin.y()},
                        {"frame", "plate_mm"}};
                };

                if (!range.fits)
                    return nlohmann::json{
                        {"status", "error"},
                        {"message", "The prime tower (" + std::to_string(before.size.x()) + " x " +
                                    std::to_string(before.size.y()) + " mm plus brim) does not fit inside "
                                    "plate " + std::to_string(index) + "'s printable area, so there is no "
                                    "position to move it to. Reduce prime_tower_width or use a larger plate."},
                        {"plate_index", index},
                        {"prime_tower", OrcaMCPPlateUtils::PrimeTowerJson(before)}};

                constexpr double kEdgeTolerance = 1e-6;  // let a caller pass back a limit verbatim
                if (local_x < range.min_x - kEdgeTolerance || local_x > range.max_x + kEdgeTolerance ||
                    local_y < range.min_y - kEdgeTolerance || local_y > range.max_y + kEdgeTolerance)
                    return nlohmann::json{
                        {"status", "error"},
                        {"message", "Position is outside the range the tower plus its brim fits in on plate " +
                                    std::to_string(index) + ". See allowed_range, in plate millimetres."},
                        {"plate_index", index},
                        {"requested", {{"x", x}, {"y", y}}},
                        {"allowed_range", range_json()},
                        {"prime_tower", OrcaMCPPlateUtils::PrimeTowerJson(before)}};

                DynamicConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
                auto* x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x", true);
                auto* y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y", true);
                if (x_opt == nullptr || y_opt == nullptr)
                    return nlohmann::json{{"status", "error"},
                                          {"message", "wipe_tower_x / wipe_tower_y are missing from the project config"}};

                // Validated; only now is anything mutated, and the snapshot is taken first so `undo`
                // puts the tower back. Plater::take_snapshot copies wipe_tower_x/y into
                // model.wipe_tower.positions on its way, which is the state undo actually restores.
                plater->take_snapshot(_u8L("Move Prime Tower"));

                // These vectors are per plate and are grown lazily elsewhere, so a project that has
                // never had a tower on a later plate can still be shorter than the plate list.
                if (x_opt->values.size() <= size_t(index))
                    x_opt->values.resize(size_t(index) + 1, x_opt->values.empty() ? 0.0 : x_opt->values.front());
                if (y_opt->values.size() <= size_t(index))
                    y_opt->values.resize(size_t(index) + 1, y_opt->values.empty() ? 0.0 : y_opt->values.front());

                ConfigOptionFloat new_x(local_x);
                ConfigOptionFloat new_y(local_y);
                x_opt->set_at(&new_x, index, 0);
                y_opt->set_at(&new_y, index, 0);

                OrcaMCPPresetConfigUtils::RefreshAfterProjectConfigChange();
                plater->update();

                const PrimeTowerState after = OrcaMCPPlateUtils::GetPrimeTowerState(index);

                nlohmann::json result = {
                    {"status", "success"},
                    {"plate_index", index},
                    {"previous_position", {{"x", before.corner.x()}, {"y", before.corner.y()}}},
                    {"position", {{"x", after.corner.x()}, {"y", after.corner.y()}}},
                    {"allowed_range", range_json()},
                    {"prime_tower", OrcaMCPPlateUtils::PrimeTowerJson(after)}
                };

                // Landing on an object is a warning, not a refusal: an agent re-arranging a plate
                // moves things through each other's way on purpose. Refusing here would make the
                // intermediate steps of a legitimate rearrangement impossible.
                nlohmann::json conflicts = nlohmann::json::array();
                if (after.printed) {
                    PartPlate* plate = plate_list.get_plate(index);
                    const DynamicPrintConfig& print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
                    for (ModelObject* obj : plate->get_objects_on_this_plate()) {
                        const ObjectFootprint fp = OrcaMCPPlateUtils::GetObjectFootprint(*obj, print_cfg);
                        if (OrcaMCP::footprints_overlap(after.footprint, fp.rect))
                            conflicts.push_back({{"kind", "object"}, {"name", obj->name}});
                    }
                    for (const BoundingBoxf3& area : plate->get_exclude_areas()) {
                        if (OrcaMCP::footprints_overlap(after.footprint, OrcaMCP::footprint_of(area)))
                            conflicts.push_back({{"kind", "excluded_area"}, {"name", "Excluded bed area"}});
                    }
                }
                result["conflicts"] = conflicts;
                if (!conflicts.empty())
                    result["conflict_note"] = "The tower's footprint (brim included) overlaps " +
                                              std::to_string(conflicts.size()) +
                                              " other occupant(s) of this plate. The move was applied; slicing "
                                              "will report a prime-tower clearance error until it is resolved.";

                result["active_warnings"] = get_active_warnings_json(plater);
                return result;
            });
        }
    });

    // ==================== OBJECT TRANSFORMS ====================

    // move_object - Move/translate an object
    register_tool({
        "move_object",
        ToolCategory::Transforms,
        "Move an object, in plate mm",
        "Move object by offset (relative) or to position (relative=false). X/Y/Z are plate "
        "millimetres along the plate's own axes -- the same frame get_object_info and this tool's "
        "own \"position\" report, and independent of how the object is rotated. Moving an object "
        "into another plate's area re-homes it onto that plate; the response reports the resulting "
        "plate_index and measures on_bed against that plate. Every instance of the object moves by "
        "the same amount, so a multi-instance object keeps its arrangement.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"x", {
                    {"type", "number"},
                    {"description", "Plate X in mm: a displacement along the plate's X axis when "
                                    "relative, else the X the bounding-box centre ends at"}
                }},
                {"y", {
                    {"type", "number"},
                    {"description", "Plate Y in mm, same convention as x"}
                }},
                {"z", {
                    {"type", "number"},
                    {"description", "Plate Z in mm (height above the bed), same convention as x"}
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

                // Relative: the offset for the specified axes (unspecified = 0 offset).
                // Absolute: whatever gets the bounding-box centre to the specified axes' values,
                // leaving the unspecified ones where they are.
                const Vec3d target(has_x ? x : current_center.x(),
                                   has_y ? y : current_center.y(),
                                   has_z ? z : current_center.z());
                const Vec3d requested_delta = relative ? Vec3d(x, y, z) : Vec3d(target - current_center);

                // translate_instances, not translate: translate() moves the *volumes*, beneath the
                // instance transform, so the instance's rotation is applied on top of the caller's
                // displacement and turns it into a move along some other world axis. On an instance
                // rotated 90 degrees about X, a -84 mm Y request came out as a +84 mm Z move and
                // left the part floating above the bed. The instance offset is already in plate
                // coordinates, which is the frame every response here reports. Every instance moves
                // by the same amount, so a multi-instance object keeps its arrangement and the
                // object-level position this tool reports is the one that was asked for.
                if (!requested_delta.isZero()) {
                    plater->take_snapshot(_u8L("Move Object"));
                    obj->translate_instances(requested_delta);
                }

                // Notify UI of changes
                obj->invalidate_bounding_box();
                plater->update();

                // Get resulting state
                BoundingBoxf3 new_bbox = obj->bounding_box_approx();
                Vec3d new_center = new_bbox.center();
                Vec3d rotation = obj->instances[0]->get_rotation();
                Vec3d scale = obj->instances[0]->get_scaling_factor();

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
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // Move the object onto the plate its new position sits in, and report which one that
                // is. A move across a plate boundary that leaves the instance registered on its old
                // plate slices onto the old plate, in that plate's filaments, with no error.
                rehome_and_report_placement(result, object_id);

                // Add movement delta for clarity
                Vec3d delta = new_center - current_center;
                if (delta.norm() > 0.001) {
                    result["movement_delta"] = {{"x", delta.x()}, {"y", delta.y()}, {"z", delta.z()}};
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
        ToolCategory::Transforms,
        "Rotate an object about plate axes",
        "Rotate object around the plate's X, Y and Z axes (degrees), not the object's own axes: a "
        "z=90 turns the object about the vertical whatever its current rotation is. Applied in the "
        "order X, then Y, then Z, about the object's bounding-box centre so it turns in place. The "
        "resulting rotation_degrees are the instance's, the same numbers get_object_info reports. "
        "The response reports the plate the object is on afterwards (plate_index) and measures "
        "on_bed against that plate.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"x", {
                    {"type", "number"},
                    {"description", "Rotation about the plate's X axis (degrees)"}
                }},
                {"y", {
                    {"type", "number"},
                    {"description", "Rotation about the plate's Y axis (degrees)"}
                }},
                {"z", {
                    {"type", "number"},
                    {"description", "Rotation about the plate's Z axis, the vertical (degrees)"}
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

                // Plate axes, not the object's own. ModelObject::rotate() turns the *volumes*,
                // beneath the instance transform, so on an instance already rotated 90 degrees
                // about X a "rotate about Z" was a rotation about a horizontal world axis; it also
                // left instances[0]'s rotation untouched, so the rotation_degrees reported below
                // never changed, and its center_around_origin() re-centred the mesh as a side
                // effect nothing in this tool's contract mentions.
                // Geometry::rotation_transform assembles X, then Y, then Z -- the order the
                // sequence of obj->rotate() calls it replaces applied them in.
                const double deg_to_rad = M_PI / 180.0;
                const Transform3d world_rotation =
                    Geometry::rotation_transform(Vec3d(x_deg, y_deg, z_deg) * deg_to_rad);

                if (!world_rotation.isApprox(Transform3d::Identity())) {
                    plater->take_snapshot(_u8L("Rotate Object"));
                    transform_instances_in_plate_frame(*obj, world_rotation);
                }

                // Notify UI of changes
                obj->invalidate_bounding_box();
                plater->update();

                // Get resulting state
                BoundingBoxf3 new_bbox = obj->bounding_box_approx();
                Vec3d new_center = new_bbox.center();
                Vec3d rotation = obj->instances[0]->get_rotation();
                Vec3d scale = obj->instances[0]->get_scaling_factor();

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
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // A rotation changes the convex hull, so it can push an instance over a plate
                // boundary or off the bed; re-home it and measure against the plate it is on now.
                rehome_and_report_placement(result, object_id);

                // Add turntable preview if requested
                add_turntable_preview_if_requested(result, include_preview, preview_views, preview_resolution);

                return result;
            });
        }
    });

    // scale_object - Scale an object
    register_tool({
        "scale_object",
        ToolCategory::Transforms,
        "Scale an object along plate axes",
        "Scale object along the plate's X, Y and Z axes, not the object's own: with uniform=false, "
        "z is the object's height above the bed whatever its rotation. uniform=true uses x for all "
        "axes and is frame-independent. Scaling is about the object's bounding-box centre, so it "
        "grows in place. Factors must be positive; use mirror_object to flip an axis. A non-uniform "
        "scale along plate axes on an object whose rotation is not a multiple of 90 degrees is a "
        "shear -- it is applied, and the response says so in skew_warning. The response reports the "
        "plate the object is on afterwards (plate_index) and measures on_bed against that plate.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"x", {
                    {"type", "number"},
                    {"description", "Scale factor along the plate's X axis (must be > 0)"}
                }},
                {"y", {
                    {"type", "number"},
                    {"description", "Scale factor along the plate's Y axis (must be > 0)"}
                }},
                {"z", {
                    {"type", "number"},
                    {"description", "Scale factor along the plate's Z axis, the vertical (> 0)"}
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

                const Vec3d factors = uniform ? Vec3d(x, x, x) : Vec3d(x, y, z);
                // A zero factor collapses the instance matrix to a singular one, which every later
                // decomposition of it (rotation, scaling factor, the slicer's own) reads as
                // nonsense; a negative one is a mirror wearing a scale's name. Both are refused
                // here rather than written into the model. ModelObject::scale() accepted them.
                if (!std::isfinite(factors.x()) || !std::isfinite(factors.y()) || !std::isfinite(factors.z()) ||
                    factors.x() <= 0.0 || factors.y() <= 0.0 || factors.z() <= 0.0) {
                    throw std::runtime_error("Scale factors must be positive; use mirror_object to flip an axis");
                }

                // Plate axes, not the object's own. ModelObject::scale() scaled the *volumes*,
                // beneath the instance transform, so a non-uniform scale ran along the object's
                // local axes -- and left instances[0]'s scaling factor at 1, so the "scale" this
                // tool reports never moved. On a multi-volume object it was worse still: each
                // volume was scaled along its own axes, which pulls an assembly apart.
                const bool had_skew = !obj->instances.empty() &&
                                      obj->instances[0]->get_transformation().has_skew();

                if (!factors.isApprox(Vec3d::Ones())) {
                    plater->take_snapshot(_u8L("Scale Object"));
                    transform_instances_in_plate_frame(*obj, Geometry::scale_transform(factors));
                }

                // Plate-axis scaling of a tilted object cannot be anything but a shear. The GUI
                // sidesteps this by refusing world coordinates for such an object; an MCP caller
                // has no such gate, so the result is reported rather than silently produced.
                const bool skew_introduced = !had_skew && !obj->instances.empty() &&
                                             obj->instances[0]->get_transformation().has_skew();

                // Notify UI of changes
                obj->invalidate_bounding_box();
                plater->update();

                // Get resulting state
                BoundingBoxf3 new_bbox = obj->bounding_box_approx();
                Vec3d new_center = new_bbox.center();
                Vec3d rotation = obj->instances[0]->get_rotation();
                Vec3d scale_result = obj->instances[0]->get_scaling_factor();

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
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                if (skew_introduced)
                    result["skew_warning"] = "A non-uniform scale along plate axes on an object whose "
                                             "rotation is not a multiple of 90 degrees sheared it. Use "
                                             "uniform=true, or unrotate the object first.";

                // Scaling grows the convex hull about the object centre, so it can spill over a plate
                // boundary or off the bed; re-home it and measure against the plate it is on now.
                rehome_and_report_placement(result, object_id);

                // Add turntable preview if requested
                add_turntable_preview_if_requested(result, include_preview, preview_views, preview_resolution);

                return result;
            });
        }
    });

    // transform_objects - Batch transform multiple objects
    register_tool({
        "transform_objects",
        ToolCategory::Transforms,
        "Move, rotate, scale many objects at once",
        "Batch transform multiple objects. Position, rotation and scale are all in the plate's own "
        "frame -- the same frame get_object_info reports -- not the object's local axes, and match "
        "move_object, rotate_object and scale_object exactly. Each result reports the plate that "
        "object is on afterwards (plate_index) and measures on_bed against that plate.",
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
                                {"description", "Absolute position in plate mm {x, y, z}, the bounding-box "
                                                "centre - unspecified axes preserved"},
                                {"properties", {
                                    {"x", {{"type", "number"}}},
                                    {"y", {{"type", "number"}}},
                                    {"z", {{"type", "number"}}}
                                }},
                                {"additionalProperties", false}
                            }},
                            {"rotation", {
                                {"type", "object"},
                                {"description", "Rotation about the plate's axes in degrees {x, y, z} - "
                                                "applied incrementally, X then Y then Z, about the "
                                                "object's centre"},
                                {"properties", {
                                    {"x", {{"type", "number"}}},
                                    {"y", {{"type", "number"}}},
                                    {"z", {{"type", "number"}}}
                                }},
                                {"additionalProperties", false}
                            }},
                            {"scale", {
                                {"type", "object"},
                                {"description", "Scale factors along the plate's axes {x, y, z}, or "
                                                "{uniform: value}; all must be positive"},
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
                std::set<int> rejected;  // object_ids already reported as an error below

                const double deg_to_rad = M_PI / 180.0;

                // One snapshot for the batch, taken when the first entry has passed validation, so
                // undo steps back over the whole call and a batch that applied nothing leaves no
                // empty step behind.
                bool snapshot_taken = false;
                auto ensure_snapshot = [&plater, &snapshot_taken]() {
                    if (!snapshot_taken) {
                        plater->take_snapshot(_u8L("Transform Objects"));
                        snapshot_taken = true;
                    }
                };

                // Every branch below is the plate-frame version of what the single-object tool
                // applies, for the reasons spelled out there: ModelObject's translate/rotate/scale
                // all act on the volumes, beneath the instance transform, so on a rotated instance
                // each of them ran along an axis the caller never named.
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

                    // Validated before anything is applied, so a rejected entry leaves the object
                    // exactly as it was rather than moved and rotated but not scaled.
                    Vec3d factors = Vec3d::Ones();
                    if (t.contains("scale")) {
                        auto sc = t["scale"];
                        factors = sc.contains("uniform") ? Vec3d::Constant(sc["uniform"].get<double>())
                                                         : Vec3d(sc.value("x", 1.0), sc.value("y", 1.0),
                                                                 sc.value("z", 1.0));
                        // Same refusal as scale_object: a zero factor makes the instance matrix
                        // singular and a negative one is an unannounced mirror.
                        if (!std::isfinite(factors.x()) || !std::isfinite(factors.y()) ||
                            !std::isfinite(factors.z()) ||
                            factors.x() <= 0.0 || factors.y() <= 0.0 || factors.z() <= 0.0) {
                            results.push_back({
                                {"object_id", object_id},
                                {"status", "error"},
                                {"message", "Scale factors must be positive; use mirror_object to flip an axis"}
                            });
                            rejected.insert(object_id);
                            continue;
                        }
                    }

                    ensure_snapshot();

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
                        obj->translate_instances(target - current_center);
                    }

                    // Apply rotation (incremental)
                    if (t.contains("rotation")) {
                        auto rot = t["rotation"];
                        const Vec3d degrees(rot.value("x", 0.0), rot.value("y", 0.0), rot.value("z", 0.0));
                        const Transform3d world_rotation = Geometry::rotation_transform(degrees * deg_to_rad);
                        if (!world_rotation.isApprox(Transform3d::Identity()))
                            transform_instances_in_plate_frame(*obj, world_rotation);
                    }

                    // Apply scale
                    if (t.contains("scale"))
                        transform_instances_in_plate_frame(*obj, Geometry::scale_transform(factors));

                    obj->invalidate_bounding_box();
                }

                // Single UI update for all transforms
                plater->update();

                // Build results for each object. Each transform applied above is one the single-
                // object tools also apply, so each owes the same plate re-homing, and each object is
                // measured against the plate it landed on rather than the one that happens to be
                // selected.
                for (const auto& t : transforms) {
                    int object_id = t["object_id"];
                    if (object_id < 0 || object_id >= static_cast<int>(model.objects.size()) ||
                        rejected.count(object_id) > 0) {
                        continue;  // Already reported error
                    }

                    ModelObject* obj = model.objects[object_id];
                    BoundingBoxf3 bbox = obj->bounding_box_approx();
                    Vec3d center = bbox.center();

                    nlohmann::json entry = {
                        {"object_id", object_id},
                        {"status", "success"},
                        {"position", {{"x", center.x()}, {"y", center.y()}, {"z", center.z()}}}
                    };
                    rehome_and_report_placement(entry, object_id);
                    results.push_back(entry);
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
        ToolCategory::Transforms,
        "Mirror an object across a plate axis",
        "Mirror an object across a plate axis, not the object's own: axis=z flips it top to bottom "
        "on the bed whatever its rotation. Mirroring is about the object's bounding-box centre, so "
        "it stays where it is. The response reports the plate the object is on afterwards "
        "(plate_index) and measures on_bed against that plate.",
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
                    {"description", "Plate axis to mirror across: x, y, or z"}
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

                // Plate axis, not the object's own. ModelObject::mirror() flipped the *volumes*,
                // beneath the instance transform, so on a rotated instance "mirror z" reflected
                // across some other world plane -- and it reflected about the volume origin rather
                // than the object's centre, which is what moved an asymmetric object somewhere
                // else. A mirror is a scale of -1 on one axis, which is how the GUI expresses it
                // too (Selection::mirror).
                Vec3d mirror_factors = Vec3d::Ones();
                mirror_factors[axis]  = -1.0;

                plater->take_snapshot(_u8L("Mirror Object"));
                transform_instances_in_plate_frame(*obj, Geometry::scale_transform(mirror_factors));

                // Notify UI of changes
                obj->invalidate_bounding_box();
                plater->update();

                nlohmann::json result = {
                    {"status", "success"},
                    {"object_id", object_id},
                    {"axis", axis_str},
                    {"active_warnings", get_active_warnings_json(plater)}
                };

                // A mirror about the object's centre keeps its bounding box, but a left-handed
                // instance re-homes and re-slices like any other change, and this tool reported no
                // placement at all before.
                rehome_and_report_placement(result, object_id);

                // Add turntable preview if requested
                add_turntable_preview_if_requested(result, include_preview, preview_views, preview_resolution);

                return result;
            });
        }
    });

    // clone_object - Duplicate an object
    register_tool({
        "clone_object",
        ToolCategory::Transforms,
        "Copy an object as instances or objects",
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
        ToolCategory::Models,
        "One object's transform, volumes, slots",
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

                // Which plate this object is on, and whether it fits that plate. This used to test
                // against whichever plate happened to be selected, so asking about an object on
                // plate 4 while plate 1 was selected reported it off the bed -- and get_server_info
                // tells agents to read on_bed to verify placement.
                report_placement(result, object_id);

                // Every volume, not just the printable parts get_object_components lists:
                // modifiers carry a filament too, and one pinned to another slot keeps the plate
                // multi-filament however the object is set. own_filament is null when the
                // volume inherits the object's; effective_filament is what prints.
                nlohmann::json volumes = nlohmann::json::array();
                for (const OrcaMCP::VolumeFilament& v : OrcaMCP::describe_volume_filaments(*obj)) {
                    volumes.push_back({
                        {"volume_id", v.volume_id},
                        {"name", v.name},
                        {"type", v.type},
                        {"own_filament", v.own_filament > 0 ? nlohmann::json(v.own_filament) : nlohmann::json(nullptr)},
                        {"effective_filament", v.effective_filament}
                    });
                }
                result["volumes"]        = volumes;
                result["filament"]       = obj->config.has("extruder") ? obj->config.extruder() : 1;
                result["filaments_used"] = OrcaMCP::effective_object_filaments(*obj);

                return result;
            });
        }
    });

    // rename_object - Rename an object
    register_tool({
        "rename_object",
        ToolCategory::Models,
        "Rename an object",
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
        ToolCategory::Transforms,
        "Remove an object from the scene",
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
        ToolCategory::Models,
        "Include or skip an object when slicing",
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
        ToolCategory::Transforms,
        "Lay an object flat on its best face",
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
        ToolCategory::Transforms,
        "Cut an object at a Z height",
        "Cut object at a Z height measured in plate mm (height above the bed), the same frame "
        "get_object_info reports -- the object's own rotation is accounted for. keep: below, above, "
        "or both.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {
                    {"type", "integer"},
                    {"description", "Object index (0-based)"}
                }},
                {"z_height", {
                    {"type", "number"},
                    {"description", "Cut height in plate mm, measured from the bed"}
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
            double z_height = 0.0;
            if (!parse_double_param(params["z_height"], z_height))
                return nlohmann::json{{"status", "error"},
                                      {"message", "z_height must be a finite number, in plate millimetres"}};
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

                // Subtracting only the offset is the whole conversion, rotation included: Cut brings
                // each mesh into the frame the cut matrix lives in with get_matrix_no_offset()
                // (CutUtils.cpp:332, used at :77), so the instance's rotation and scale are already
                // applied there and the only difference left from plate coordinates is the
                // translation. A rotated instance does not break this.
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
        ToolCategory::Visualization,
        "Choose the G-code preview color mode",
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
    register_paint_tools();

    BOOST_LOG_TRIVIAL(info) << "OrcaMCPServer: Registered " << s_tools.size() << " tools";
}

}} // namespace Slic3r::GUI
