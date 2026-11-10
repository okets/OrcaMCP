#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <nlohmann/json.hpp>
#include "OrcaMCPLayerGeometry.hpp"
#include "OrcaMCPResultIdentity.hpp"

namespace Slic3r {
class ConfigOptionResolver;
namespace GUI { namespace OrcaMCP {

struct PlateAnalysisRequest {
    int plate_index = 0;
    bool detail = false;
    size_t max_moves = 10000;
    nlohmann::json cursor = nullptr;
};
// These are GUI-owned facts, captured after synchronized worker admission and before result reads.
struct AnalysisSourceState {
    bool only_gcode = false;
    bool gcode_3mf = false;
    bool has_print = false;
    bool has_objects = false;
    bool slicing_finished = false;
    bool export_finished = false;
};
void require_analysis_source(const AnalysisSourceState& state);
PlateAnalysisRequest parse_plate_analysis_request(const nlohmann::json& params);
nlohmann::json plate_analysis_schema();
nlohmann::json analysis_controls(const ConfigOptionResolver& config);
// Source admission is the caller's responsibility, before passing any native result to this helper.
// Timing excludes final timing insertion/dump and transport. The callback's start is supplied by GUI.
nlohmann::json plate_analysis_page(const GCodeProcessorResult& result, const PlateAnalysisRequest& request,
    const GeometryIdentity& identity, const nlohmann::json& controls,
    std::chrono::steady_clock::time_point gui_started = std::chrono::steady_clock::now());

}}} // namespace Slic3r::GUI::OrcaMCP
