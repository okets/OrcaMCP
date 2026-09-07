// src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.hpp
#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include "slic3r/GUI/MixedFilamentDialog.hpp"   // MixedFilamentResult

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Snapshot of every filament slot (physical first, then mixed). Main thread only.
nlohmann::json describe_filaments();

// Builds a MixedFilamentResult from tool params; returns false and fills `error` on bad input.
// Only shape/parse checks (array sizes, numeric types, ratio sum) live here -- domain rules
// (component range, physical-only components, minimum printer filament count, ...) are left to
// Sidebar::apply_mixed_filament so they are validated in exactly one place.
bool mixed_result_from_params(const nlohmann::json& params, MixedFilamentResult& out, std::string& error);

// 1-based slot -> 0-based config index, validating range. Returns -1 and sets `error` when invalid.
int slot_to_config_index(int slot, std::string& error);

// Assigns a filament slot to an object or one of its volumes. Returns false and sets `error` on failure.
// Main thread only.
bool set_object_filament(int object_id, int volume_id /* -1 = object */, int slot, std::string& error);

}}} // namespace Slic3r::GUI::OrcaMCP
