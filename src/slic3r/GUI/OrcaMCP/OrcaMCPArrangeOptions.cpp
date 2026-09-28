// src/slic3r/GUI/OrcaMCP/OrcaMCPArrangeOptions.cpp
#include "OrcaMCPArrangeOptions.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPartEdits.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

const char* mode_name(ArrangeMode mode)
{
    switch (mode) {
    case ArrangeMode::by_object: return "by object";
    case ArrangeMode::sla: return "sla";
    case ArrangeMode::by_layer: break;
    }
    return "by layer";
}

const char* mode_postfix(ArrangeMode mode)
{
    switch (mode) {
    case ArrangeMode::by_object: return "_fff_seq_print";
    case ArrangeMode::sla: return "_sla";
    case ArrangeMode::by_layer: break;
    }
    return "_fff";
}

} // namespace

bool ArrangeOptions::operator==(const ArrangeOptions& other) const { return changed_arrange_options(*this, other).empty(); }

ArrangeOptionKeys arrange_option_keys(ArrangeMode mode)
{
    const std::string postfix = mode_postfix(mode);
    return {"min_object_distance" + postfix, "enable_rotation" + postfix, "allow_multi_materials_on_same_plate",
            "avoid_extrusion_cali_region"};
}

bool ArrangeOptionsRequest::any() const
{
    return reset || spacing_mm || auto_rotate || allow_multiple_materials || align_to_y_axis || avoid_calibration_region;
}

std::optional<ArrangeOptionsRequest> read_arrange_options(const nlohmann::json& params, std::string& error)
{
    ArrangeOptionsRequest request;
    if (params.contains("spacing_mm")) {
        double spacing = 0.;
        if (!parse_double_param(params.at("spacing_mm"), spacing)) {
            error = "spacing_mm must be a number of millimetres, 0 for automatic spacing; got " + params.at("spacing_mm").dump();
            return std::nullopt;
        }
        request.spacing_mm = spacing;
    }
    request.reset                    = read_flag(params, "reset_options", error).value_or(false);
    request.auto_rotate              = read_flag(params, "auto_rotate", error);
    request.allow_multiple_materials = read_flag(params, "allow_multiple_materials", error);
    request.align_to_y_axis          = read_flag(params, "align_to_y_axis", error);
    request.avoid_calibration_region = read_flag(params, "avoid_calibration_region", error);
    if (!error.empty())
        return std::nullopt;
    return request;
}

std::optional<std::string> apply_arrange_options(const ArrangeOptionsRequest& request, const ArrangeOptions& defaults,
                                                 bool avoid_region_offered, ArrangeOptions& current)
{
    if (request.spacing_mm && *request.spacing_mm < 0.)
        return "spacing_mm must be 0 or more (0 is automatic spacing), as the arrange menu's Spacing is";
    if (request.avoid_calibration_region && !avoid_region_offered)
        return std::string("avoid_calibration_region applies only to a Bambu Lab printer that scans its first layer, where the "
                           "arrange menu offers it; this printer has no calibration area to keep clear of: leave it out");
    ArrangeOptions options = request.reset ? defaults : current;
    if (request.spacing_mm)
        options.spacing_mm = *request.spacing_mm;
    if (request.auto_rotate)
        options.auto_rotate = *request.auto_rotate;
    if (request.allow_multiple_materials)
        options.allow_multiple_materials = *request.allow_multiple_materials;
    if (request.avoid_calibration_region)
        options.avoid_calibration_region = *request.avoid_calibration_region;
    if (request.align_to_y_axis)
        options.align_to_y_axis = *request.align_to_y_axis;
    if (options.auto_rotate && request.align_to_y_axis.value_or(false))
        return std::string("align_to_y_axis cannot be on while auto_rotate is: the arrange menu greys it out then. Send "
                           "auto_rotate: false with it");
    // The menu turns align to Y off whenever auto-rotate is on.
    if (options.auto_rotate)
        options.align_to_y_axis = false;
    current = options;
    return std::nullopt;
}

std::vector<std::string> changed_arrange_options(const ArrangeOptions& before, const ArrangeOptions& after)
{
    std::vector<std::string> changed;
    if (before.spacing_mm != after.spacing_mm)
        changed.emplace_back("spacing_mm");
    if (before.auto_rotate != after.auto_rotate)
        changed.emplace_back("auto_rotate");
    if (before.allow_multiple_materials != after.allow_multiple_materials)
        changed.emplace_back("allow_multiple_materials");
    if (before.align_to_y_axis != after.align_to_y_axis)
        changed.emplace_back("align_to_y_axis");
    if (before.avoid_calibration_region != after.avoid_calibration_region)
        changed.emplace_back("avoid_calibration_region");
    return changed;
}

nlohmann::json arrange_options_json(const ArrangeOptions& options, ArrangeMode mode, bool avoid_region_offered)
{
    nlohmann::json out = {{"spacing_mm", options.spacing_mm},
                          {"auto_rotate", options.auto_rotate},
                          {"allow_multiple_materials", options.allow_multiple_materials},
                          {"align_to_y_axis", options.align_to_y_axis},
                          {"for_print_sequence", mode_name(mode)}};
    if (avoid_region_offered)
        out["avoid_calibration_region"] = options.avoid_calibration_region;
    return out;
}

nlohmann::json arrange_option_properties()
{
    const std::string saved = " Saved as the arrange menu saves it: every later arrange uses it.";
    return {
        {"spacing_mm",
         {{"type", "number"},
          {"minimum", 0},
          {"description", "Gap between objects in mm, 0 for automatic spacing (the arrange menu's Spacing), for the current print "
                          "sequence's set." + saved}}},
        {"auto_rotate",
         {{"type", "boolean"}, {"description", "Turn objects about Z to fit more (Auto rotate for arrangement)." + saved}}},
        {"allow_multiple_materials",
         {{"type", "boolean"},
          {"description", "Let objects of different filaments share a plate (Allow multiple materials on same plate)." + saved}}},
        {"align_to_y_axis",
         {{"type", "boolean"},
          {"description", "Line objects up along Y (Align to Y axis); not with auto_rotate. Kept until the app restarts or the "
                          "printer changes, as the menu keeps it"}}},
        {"avoid_calibration_region",
         {{"type", "boolean"},
          {"description", "Keep clear of the extrusion calibration area; only on a Bambu Lab printer that scans its first layer." +
                              saved}}},
        {"reset_options",
         {{"type", "boolean"},
          {"description", "The arrange menu's Reset, before the options this call gives: automatic spacing, no auto-rotate, several "
                          "materials allowed, align to Y as the printer's default"}}}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
