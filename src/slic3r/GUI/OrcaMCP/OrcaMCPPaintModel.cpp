// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.cpp
#include "OrcaMCPPaintModel.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

bool parse_paint_mode(const std::string& name, PaintMode& out)
{
    if (name == "color")      { out = PaintMode::Color;     return true; }
    if (name == "support")    { out = PaintMode::Support;   return true; }
    if (name == "seam")       { out = PaintMode::Seam;      return true; }
    if (name == "fuzzy_skin") { out = PaintMode::FuzzySkin; return true; }
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

bool parse_paint_state(PaintMode mode, const std::string& name, int& out_state)
{
    if (mode == PaintMode::Color)
        return false;
    if (name == "none")     { out_state = int(EnforcerBlockerType::NONE);     return true; }
    if (name == "enforcer") { out_state = int(EnforcerBlockerType::ENFORCER); return true; }
    if (name == "blocker" && mode != PaintMode::FuzzySkin) {
        out_state = int(EnforcerBlockerType::BLOCKER);
        return true;
    }
    return false;
}

std::string paint_state_label(PaintMode mode, int state)
{
    if (mode == PaintMode::Color)
        return state <= 0 ? std::string("unpainted") : "filament " + std::to_string(state);
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

}}} // namespace Slic3r::GUI::OrcaMCP
