// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp
#include "OrcaMCPPaintModel.hpp"

#include <algorithm>
#include <cctype>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// Both parse_paint_mode and parse_paint_state fold case before comparing: they read two
// parameters of the same MCP tool, and a caller whose `mode` is accepted in capitals while its
// `state` is rejected pays a round trip for a distinction that means nothing.
std::string to_lower_copy(const std::string& s)
{
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower;
}

} // namespace

bool parse_paint_mode(const std::string& name, PaintMode& out)
{
    const std::string lower = to_lower_copy(name);
    if (lower == "color")      { out = PaintMode::Color;     return true; }
    if (lower == "support")    { out = PaintMode::Support;   return true; }
    if (lower == "seam")       { out = PaintMode::Seam;      return true; }
    if (lower == "fuzzy_skin") { out = PaintMode::FuzzySkin; return true; }
    return false;
}

const char* paint_mode_name(PaintMode mode)
{
    switch (mode) {
    case PaintMode::Color:     return "color";
    case PaintMode::Support:   return "support";
    case PaintMode::Seam:      return "seam";
    case PaintMode::FuzzySkin: return "fuzzy_skin";
    }
    return "color";
}

FacetsAnnotation& annotation_for_mode(ModelVolume& mv, PaintMode mode)
{
    switch (mode) {
    case PaintMode::Support:   return mv.supported_facets;
    case PaintMode::Seam:      return mv.seam_facets;
    case PaintMode::FuzzySkin: return mv.fuzzy_skin_facets;
    case PaintMode::Color:     break;
    }
    return mv.mmu_segmentation_facets;
}

const FacetsAnnotation& annotation_for_mode(const ModelVolume& mv, PaintMode mode)
{
    return annotation_for_mode(const_cast<ModelVolume&>(mv), mode);
}

int max_paint_state() { return int(EnforcerBlockerType::ExtruderMax); }

int max_paint_state_for(PaintMode mode)
{
    switch (mode) {
    // Support and Seam enforcers and blockers are the only two states their gizmos write.
    case PaintMode::Support:
    case PaintMode::Seam:      return int(EnforcerBlockerType::BLOCKER);
    // FUZZY_SKIN is an alias of ENFORCER, and the fuzzy skin gizmo has no blocker at all.
    case PaintMode::FuzzySkin: return int(EnforcerBlockerType::FUZZY_SKIN);
    case PaintMode::Color:     break;
    }
    return max_paint_state();
}

bool parse_paint_state(PaintMode mode, const std::string& name, int& out_state)
{
    if (mode == PaintMode::Color)
        return false;
    const std::string lower = to_lower_copy(name);
    if (lower == "none") { out_state = int(EnforcerBlockerType::NONE); return true; }
    // "fuzzy_skin" is the token paint_state_label hands back for FuzzySkin's enforcer state, so it
    // has to parse back to the same state or a caller who echoes a read into a write gets rejected.
    if (lower == "enforcer" || (mode == PaintMode::FuzzySkin && lower == "fuzzy_skin")) {
        out_state = int(EnforcerBlockerType::ENFORCER);
        return true;
    }
    if (lower == "blocker" && mode != PaintMode::FuzzySkin) {
        out_state = int(EnforcerBlockerType::BLOCKER);
        return true;
    }
    return false;
}

std::string paint_state_label(PaintMode mode, int state)
{
    if (mode == PaintMode::Color) {
        if (state <= 0)
            return "unpainted";
        // Above max_paint_state() is a value the slicer could never have written (EnforcerBlockerType
        // stops at ExtruderMax): report it as out of domain rather than as a plausible filament slot.
        if (state > max_paint_state())
            return "state " + std::to_string(state);
        return "filament " + std::to_string(state);
    }
    if (state == int(EnforcerBlockerType::NONE))
        return "none";
    if (state == int(EnforcerBlockerType::ENFORCER))
        return mode == PaintMode::FuzzySkin ? "fuzzy_skin" : "enforcer";
    if (state == int(EnforcerBlockerType::BLOCKER))
        return "blocker";
    return "state " + std::to_string(state);
}

Transform3d volume_to_plate(const ModelObject& obj, const ModelVolume& mv, std::size_t instance_idx)
{
    if (obj.instances.empty())
        return mv.get_matrix();
    const std::size_t idx = instance_idx < obj.instances.size() ? instance_idx : 0;
    return obj.instances[idx]->get_matrix() * mv.get_matrix();
}

namespace {
// The one piece both brim-ear conversions share: the instance transform, defaulting the same way
// volume_to_plate does. Not folded into volume_to_plate itself -- that function composes a
// volume matrix on top, which brim_points (stored on the ModelObject, not a ModelVolume) has none
// of.
Transform3d brim_instance_matrix(const ModelObject& obj, std::size_t instance_idx)
{
    if (obj.instances.empty())
        return Transform3d::Identity();
    const std::size_t idx = instance_idx < obj.instances.size() ? instance_idx : 0;
    return obj.instances[idx]->get_matrix();
}
} // namespace

Vec3d brim_point_to_plate(const ModelObject& obj, const Vec3f& local_pos, std::size_t instance_idx)
{
    return brim_instance_matrix(obj, instance_idx) * local_pos.cast<double>();
}

Vec3f brim_point_to_object(const ModelObject& obj, double plate_x, double plate_y, std::size_t instance_idx)
{
    // BBS brim ear position is placed on the bottom side (GLGizmoBrimEars.cpp ~395-397).
    constexpr double k_underside_z = -0.0001;
    const Vec3d world(plate_x, plate_y, k_underside_z);
    const Vec3d local = brim_instance_matrix(obj, instance_idx).inverse() * world;
    return local.cast<float>();
}

bool apply_facet_states(ModelVolume& mv, PaintMode mode, const std::vector<int>& states, bool replace)
{
    // Validate before touching anything, so a rejected call is a no-op rather than a partial paint.
    // set_facet only accepts original triangles (TriangleSelector.hpp, "Only works for original
    // triangles", and its assert in TriangleSelector::set_facet), so one entry per original facet
    // is the only shape that means anything here.
    const std::size_t facet_count = mv.mesh().its.indices.size();
    if (facet_count == 0 || states.size() != facet_count)
        return false;
    const int max_state = max_paint_state_for(mode);
    for (int state : states)
        if (state < -1 || state > max_state)
            return false;

    FacetsAnnotation& annotation = annotation_for_mode(mv, mode);

    TriangleSelector selector(mv.mesh());
    if (!replace) {
        // needs_reset = false: the TriangleSelector constructor already reset, exactly as the
        // gizmo relies on (GLGizmoMmuSegmentation::init_model_triangle_selectors passes false for
        // the same reason).
        selector.deserialize(annotation.get_data(), false);
    }

    // No bounds clamp on the loop: the guard above already proved states.size() == facet_count.
    for (std::size_t i = 0; i < states.size(); ++i)
        if (states[i] >= 0)
            selector.set_facet(int(i), EnforcerBlockerType(states[i]));

    return annotation.set(selector);
}

std::vector<PaintedStateInfo> read_volume_paint(const ModelVolume& mv, PaintMode mode)
{
    const FacetsAnnotation& annotation = annotation_for_mode(mv, mode);

    TriangleSelector selector(mv.mesh());
    selector.deserialize(annotation.get_data(), false);

    const double total_area = its_surface_area(mv.mesh().its);

    // Scanned over the whole scheme's range, not max_paint_state_for(mode): a report has to show
    // whatever is actually on the volume, including a state some other tool put there.
    std::vector<PaintedStateInfo> painted;
    for (int state = int(EnforcerBlockerType::NONE); state <= max_paint_state(); ++state) {
        const int count = selector.num_facets(EnforcerBlockerType(state));
        if (count == 0)
            continue;
        PaintedStateInfo info;
        info.state       = state;
        info.facet_count = count;
        info.area_ratio  = total_area > 0.0
                               ? its_surface_area(selector.get_facets(EnforcerBlockerType(state))) / total_area
                               : 0.0;
        painted.push_back(info);
    }
    return painted;
}

bool clear_volume_paint(ModelVolume& mv, PaintMode mode)
{
    FacetsAnnotation& annotation = annotation_for_mode(mv, mode);
    if (annotation.empty())
        return false;
    annotation.reset();
    return true;
}

}}} // namespace Slic3r::GUI::OrcaMCP
