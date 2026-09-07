// src/slic3r/GUI/OrcaMCP/OrcaMCPConfigKeys.hpp
//
// Shared config-key sets used by more than one MCP tool. Single source of truth so the
// same list is never copied into multiple .cpp files (see get_valid_config_keys and
// get_toolchanger_config in OrcaMCPServer.cpp / OrcaMCPFilamentTools.cpp).
#pragma once
#include <set>
#include <string>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Toolchanger / multi-extruder settings, spanning printer preset + print preset keys.
inline const std::set<std::string> toolchanger_keys = {
    "nozzle_diameter", "extruder_offset", "extruder_colour", "extruder_type",
    "retract_length_toolchange", "retract_restart_extra_toolchange", "machine_tool_change_time",
    "change_filament_gcode", "single_extruder_multi_material", "manual_filament_change",
    "physical_extruder_map", "master_extruder_id", "printer_extruder_id",
    "purge_in_prime_tower", "wipe_tower_type", "enable_filament_ramming",
    "enable_prime_tower", "prime_tower_width", "prime_volume", "wipe_tower_filament", "toolchange_ordering",
    "prime_tower_brim_width", "prime_tower_skip_points", "wipe_tower_no_sparse_layers",
    "enable_mixed_color_sublayer", "filament_toolchange_delay", "filament_prime_volume", "filament_change_length"
};

// Keys that live in preset_bundle->project_config rather than a preset Tab.
inline const std::set<std::string> project_keys = {
    "filament_colour", "filament_map", "filament_map_mode", "filament_nozzle_map", "filament_volume_map",
    "flush_volumes_matrix", "flush_volumes_vector", "flush_multiplier", "flush_multiplier_fast", "prime_volume_mode",
    "wipe_tower_x", "wipe_tower_y",
    "filament_is_mixed", "filament_mixed_components", "filament_mixed_sublayer_ratios",
    "filament_mixed_gradient", "filament_mixed_gradient_range", "filament_mixed_gradient_curve", "filament_mixed_gradient_per_part"
};

}}} // namespace Slic3r::GUI::OrcaMCP
