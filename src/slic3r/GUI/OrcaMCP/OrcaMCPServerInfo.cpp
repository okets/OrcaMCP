#include "OrcaMCPServerInfo.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

using json = nlohmann::json;

constexpr const char* all_sections = "all";

json server()
{
    return json{
        {"name", "OrcaSlicer MCP Server"},
        {"version", OrcaMCPServer::version()},
        {"protocol", "JSON-RPC 2.0 over HTTP"},
        {"endpoint", "http://localhost:13618/mcp"}
    };
}

// What every agent should know first. Part of the default response.
json quick_start()
{
    return json{
        {"first_steps", {
            "1. Call get_scene_info to understand current project state",
            "2. Use render_plate_view with save_to_file=true to visualize",
            "3. For more, call get_server_info with a section from the sections list below"
        }},
        {"common_tasks", {
            {"load_and_slice", "load_model -> arrange_objects -> slice_all -> poll get_slicing_status -> export_gcode"},
            {"change_settings", "apply_config with settings array"},
            {"modify_object", "get_scene_info (get object_id) -> transform tools"},
            {"visualize", "render_plate_view with save_to_file=true (omit views for a 3-view contact sheet), then Read the PNG; check uniform_image first"}
        }}
    };
}

// Multi-tool recipes, one per common job.
json suggested_flows()
{
    return json{
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
                {"example4", R"({"start_print": false})"},
                {"note3", "On a print host a bare call uploads AND STARTS the print, with no dialog. Pass start_print=false to upload only, or direct=false to open the send dialog for the user."}
            }},
            {"octoprint_note", "For print hosts (Flashforge, Moonraker/Klipper, OctoPrint, ...): print_host must be configured in the printer preset. The upload runs without a dialog unless direct=false."},
            {"bambu_note", "For Bambu: use select_printer with dev_id first if needed. Dialog shows printer selection."}
        }}
    };
}

// Example arguments for the tools agents most often get wrong.
json tool_examples()
{
    return json{
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
            {"contact_sheet", R"({"plate_index": 1, "save_to_file": true})"},
            {"preset_views", R"({"plate_index": 1, "save_to_file": true, "views": [{"preset": "iso"}, {"preset": "low", "fit": {"object_index": 8}}]})"},
            {"explicit_camera", R"({"plate_index": 0, "save_to_file": true, "views": [{"camera_position": [300, -200, 150], "target": [128, 128, 30]}]})"},
            {"first_layer_plan", R"({"plate_index": 1, "save_to_file": true, "layer_view": "first_layer"})"},
            {"coordinate_frame", "camera_position/target are BED mm, the get_scene_info frame; plate N is at plates[N].bounding_box. Add frame: \"plate_local\" to give them from the plate's front-left corner. Presets never need coordinates."},
            {"read_the_numbers_first", "Check uniform_image (and its hint) and objects_in_frame before reading the image; a flat image means the camera saw nothing on that plate."},
            {"tip", "ALWAYS use save_to_file=true (PNG paths). Prefer fit: {object_index} over a higher resolution."},
            {"when_to_use", "Before/after transforms, to verify object state, to analyze geometry; layer_view first_layer for brim, support feet and adhesion questions"}
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
            {"upload_only", R"({"start_print": false})"},
            {"all_plates_via_dialog", R"({"direct": false, "all_plates": true})"},
            {"when_to_use", "After slicing completes. On a print host a bare call uploads AND STARTS the print on real hardware"},
            {"auto_detect", "Print host configured: uploads with no dialog, and starts unless start_print=false; direct=false opens the send dialog instead. Bambu: opens the send dialog for the user."}
        }}
    };
}

// How OrcaSlicer models a project: presets, plates, coordinates, object ids.
json concepts()
{
    return json{
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
            {"delete_constraint", "Cannot delete the last remaining plate. Objects on a deleted plate are neither deleted nor moved to another plate: they go outside every plate, where get_scene_info lists them under unplaced_objects."}
        }},
        {"coordinate_system", {
            {"origin", "CORNER origin (0,0) = front-left of bed. NOT center origin!"},
            {"valid_range", "X: 0 to max_x, Y: 0 to max_y. Negative coordinates are OFF the bed."},
            {"z_axis", "Z=0 is the bed surface. Object bottoms rest at Z=0. Object center Z = half the object height."},
            {"get_bed_bounds", "Call get_scene_info and read bed.min_x, bed.max_x, bed.min_y, bed.max_y"},
            {"find_free_space", "Call get_scene_info and subtract plates[].occupancy footprints from the "
                                "plate's bounding box. The occupancy list includes the prime tower and "
                                "excluded bed areas, which model_objects does not."},
            {"transform_response", "All transforms return position, rotation_degrees, scale, on_bed. Use on_bed to verify placement."},
            {"rotation_degrees_note", "rotation_degrees is the instance's rotation, the same numbers get_object_info reports. rotate_object turns the object about the plate's axes and updates it."},
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
    };
}

// The config keys agents reach for most, by preset type.
json settings()
{
    return json{
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
        }}
    };
}

// Token costs, pitfalls and habits that save calls.
json warnings_and_best_practices()
{
    return json{
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
    };
}

struct Section
{
    const char* name;
    json (*content)();
};

// The sections fetched by name. The default response indexes them instead of carrying them.
const std::vector<Section>& documentation_sections()
{
    static const std::vector<Section> sections = {
        {"concepts", concepts},
        {"suggested_flows", suggested_flows},
        {"tool_examples", tool_examples},
        {"warnings_and_best_practices", warnings_and_best_practices},
        {"settings", settings},
    };
    return sections;
}

// Every tool's summary, by category. Built from the registry, so a new tool is listed the moment
// it is registered.
json tool_catalogue(const ToolMap& tools)
{
    json catalogue = json::object();
    for (const auto& [name, tool] : tools)
        catalogue[OrcaMCPServer::tool_category_name(tool.category)][name] = tool.summary;
    return catalogue;
}

// The tools orcamcp-bridge.py answers itself: listed like any other, but served only through it.
json bridge_only_tools(const ToolMap& tools)
{
    json names = json::array();
    for (const auto& [name, tool] : tools)
        if (tool.bridge_only)
            names.push_back(name);
    return names;
}

// Each section's size in bytes, so a caller can see what a fetch costs before making it. The sections
// are fixed text, so they are built and measured once rather than on every call.
const json& section_index()
{
    static const json index = [] {
        json sizes = json::object();
        for (const Section& section : documentation_sections())
            sizes[section.name] = section.content().dump().size();
        return sizes;
    }();
    return index;
}

json default_response(const ToolMap& tools)
{
    return json{
        {"server", server()},
        {"quick_start", quick_start()},
        {"tools", tool_catalogue(tools)},
        {"bridge_only", bridge_only_tools(tools)},
        {"sections", section_index()},
        {"more", "Pass section=<a name from sections> for one of them, or section=all for everything."}
    };
}

json every_section(const ToolMap& tools)
{
    json response = json{
        {"server", server()},
        {"quick_start", quick_start()},
        {"tools", tool_catalogue(tools)},
        {"bridge_only", bridge_only_tools(tools)}
    };
    for (const Section& section : documentation_sections())
        response[section.name] = section.content();
    return response;
}

json unknown_section(const std::string& requested)
{
    std::string valid;
    for (const std::string& name : server_info_section_names())
        valid += (valid.empty() ? "" : ", ") + name;
    return json{{"status", "error"}, {"message", "Unknown section " + requested + ". Valid sections: " + valid}};
}

} // namespace

const std::vector<std::string>& server_info_section_names()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const Section& section : documentation_sections())
            out.emplace_back(section.name);
        out.emplace_back(all_sections);
        return out;
    }();
    return names;
}

json server_info(const json& params, const ToolMap& tools)
{
    if (!params.contains("section") || params.at("section").is_null())
        return default_response(tools);

    const json& section = params.at("section");
    if (!section.is_string())
        return unknown_section(section.dump());
    const std::string requested = section.get<std::string>();
    if (requested == all_sections)
        return every_section(tools);
    for (const Section& candidate : documentation_sections())
        if (requested == candidate.name)
            return json{{candidate.name, candidate.content()}};
    return unknown_section("'" + requested + "'");
}

}}} // namespace Slic3r::GUI::OrcaMCP
