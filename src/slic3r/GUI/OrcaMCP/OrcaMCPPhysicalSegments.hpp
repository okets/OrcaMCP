#pragma once

#include <cstddef>
#include <functional>
#include <vector>
#include "OrcaMCPLayerGeometry.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

inline bool is_physical_record(const GcodeMove& move) { return !move.internal_only; }
// References already observed native records; this primitive performs no source-vector inspection.
struct PhysicalSegment {
    size_t index;
    size_t previous_index;
    const GcodeMove& previous;
    const GcodeMove& move;
    ExtrusionFeature feature;
    int filament;
    bool extrusion; // Supported feature extrusion; other roles still remain physical records.
};
inline PhysicalSegment physical_segment(size_t index, size_t previous_index, const GcodeMove& previous, const GcodeMove& move)
{
    const auto feature = extrusion_feature_of(move.extrusion_role);
    return {index, previous_index, previous, move, feature, int(move.extruder_id) + 1,
        move.type == EMoveType::Extrude && feature != ExtrusionFeature::other};
}
// Geometry-only predecessor search. Detail analysis must use its cached physical predecessor instead
// of this unbounded backward search, because every native inspection counts toward its hard budget.
size_t physical_predecessor(const std::vector<GcodeMove>& moves, size_t begin);
void visit_physical_layer_segments(const std::vector<GcodeMove>& moves, const GcodeLayer& layer,
    const std::function<bool(const PhysicalSegment&)>& visitor);
std::vector<GcodeLayer> physical_gcode_layers(const std::vector<GcodeMove>& moves);

}}} // namespace Slic3r::GUI::OrcaMCP
