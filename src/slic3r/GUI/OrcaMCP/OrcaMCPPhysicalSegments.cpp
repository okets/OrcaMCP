#include "OrcaMCPPhysicalSegments.hpp"
#include <algorithm>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

size_t physical_predecessor(const std::vector<GcodeMove>& moves, size_t begin)
{
    if (moves.empty() || begin > moves.size()) throw GeometryError("unsupported_source", "No native physical predecessor");
    size_t index = begin > 0 ? begin - 1 : 0;
    while (index > 0 && !is_physical_record(moves[index])) --index;
    if (!is_physical_record(moves[index])) throw GeometryError("unsupported_source", "The native dummy is not a physical predecessor");
    return index;
}
void visit_physical_layer_segments(const std::vector<GcodeMove>& moves, const GcodeLayer& layer,
    const std::function<bool(const PhysicalSegment&)>& visitor)
{
    if (layer.begin > layer.end || layer.end > moves.size()) throw GeometryError("invalid_arguments", "Invalid native layer bounds");
    const size_t begin = std::max<size_t>(1, layer.begin);
    if (begin >= layer.end) return;
    size_t previous = physical_predecessor(moves, begin);
    for (size_t i = begin; i < layer.end; ++i) {
        if (!is_physical_record(moves[i])) continue;
        if (!visitor(physical_segment(i, previous, moves[previous], moves[i]))) break;
        previous = i;
    }
}
std::vector<GcodeLayer> physical_gcode_layers(const std::vector<GcodeMove>& moves)
{
    auto layers = gcode_layers(moves);
    double previous_z = 0.;
    for (auto& layer : layers) {
        bool endpoint = false;
        double z = previous_z;
        for (size_t i = layer.end; i > layer.begin; --i) {
            const auto& move = moves[i - 1];
            if (!is_physical_record(move)) continue;
            if (!endpoint) { z = move.position.z(); endpoint = true; }
            if (move.type == EMoveType::Extrude && extrusion_feature_of(move.extrusion_role) != ExtrusionFeature::other) {
                z = move.position.z(); break;
            }
        }
        previous_z = layer.z = z;
    }
    return layers;
}

}}} // namespace Slic3r::GUI::OrcaMCP
