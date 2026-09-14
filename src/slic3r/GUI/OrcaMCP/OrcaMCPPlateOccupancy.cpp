#include "OrcaMCPPlateOccupancy.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

BoundingBoxf footprint_of(const BoundingBoxf3& bbox)
{
    BoundingBoxf out;
    if (!bbox.defined) return out;
    out.min = Vec2d(bbox.min.x(), bbox.min.y());
    out.max = Vec2d(bbox.max.x(), bbox.max.y());
    out.defined = true;
    return out;
}

BoundingBoxf expand_footprint(const BoundingBoxf& box, double margin)
{
    if (!box.defined) return box;
    const double m = (std::isfinite(margin) && margin > 0.0) ? margin : 0.0;
    BoundingBoxf out;
    out.min = box.min - Vec2d(m, m);
    out.max = box.max + Vec2d(m, m);
    out.defined = true;
    return out;
}

BoundingBoxf prime_tower_footprint(const Vec2d& corner, const Vec2d& size, double brim_width)
{
    BoundingBoxf body;
    body.min = corner;
    body.max = corner + Vec2d(std::max(0.0, size.x()), std::max(0.0, size.y()));
    body.defined = true;
    return expand_footprint(body, brim_width);
}

bool footprint_within(const BoundingBoxf& footprint, const BoundingBoxf& plate)
{
    if (!footprint.defined || !plate.defined) return false;
    return footprint.min.x() >= plate.min.x() && footprint.max.x() <= plate.max.x() &&
           footprint.min.y() >= plate.min.y() && footprint.max.y() <= plate.max.y();
}

bool footprints_overlap(const BoundingBoxf& a, const BoundingBoxf& b)
{
    if (!a.defined || !b.defined) return false;
    return a.min.x() < b.max.x() && b.min.x() < a.max.x() &&
           a.min.y() < b.max.y() && b.min.y() < a.max.y();
}

PrimeTowerRange prime_tower_position_range(double plate_width, double plate_depth,
                                           double tower_width, double tower_depth,
                                           double margin, double brim_width)
{
    PrimeTowerRange range;
    range.min_x = margin;
    range.min_y = margin;
    range.max_x = plate_width - tower_width - margin - brim_width;
    range.max_y = plate_depth - tower_depth - margin - brim_width;
    range.fits  = range.max_x >= range.min_x && range.max_y >= range.min_y;
    return range;
}

PrimeTowerVerdict prime_tower_verdict(const PrimeTowerConditions& c)
{
    if (!c.is_fff) return PrimeTowerVerdict::NotFff;
    if (!c.enable_prime_tower) return PrimeTowerVerdict::Disabled;

    // Two settings force a tower even on a single-filament print: a smooth timelapse parks the head
    // on it between layers, and wrapping detection needs it as a wiping surface.
    const bool forced = c.timelapse_smooth || c.wrapping_detection;

    if (!forced && c.project_filament_count <= 1) return PrimeTowerVerdict::SingleFilamentProject;
    if (c.gcode_only_mode) return PrimeTowerVerdict::GcodeOnlyMode;

    // Printing by object gives each object the whole bed in turn, so a tower would be in the way --
    // unless there is exactly one object, in which case there is no turn-taking and it is allowed.
    if (c.plate_sequential && c.plate_printable_instances != 1) return PrimeTowerVerdict::SequentialMultiObject;
    if (!forced && c.plate_filament_count < 2) return PrimeTowerVerdict::SingleFilamentPlate;
    if (!c.plate_has_objects) return PrimeTowerVerdict::EmptyPlate;

    return PrimeTowerVerdict::Printed;
}

const char* prime_tower_verdict_token(PrimeTowerVerdict verdict)
{
    switch (verdict) {
    case PrimeTowerVerdict::Printed:               return "printed";
    case PrimeTowerVerdict::NotFff:                return "not_fff";
    case PrimeTowerVerdict::Disabled:              return "disabled";
    case PrimeTowerVerdict::SingleFilamentProject: return "single_filament_project";
    case PrimeTowerVerdict::GcodeOnlyMode:         return "gcode_only_mode";
    case PrimeTowerVerdict::SequentialMultiObject: return "sequential_multi_object";
    case PrimeTowerVerdict::EmptyPlate:            return "empty_plate";
    case PrimeTowerVerdict::SingleFilamentPlate:   return "single_filament_plate";
    }
    return "unknown";
}

const char* prime_tower_verdict_explanation(PrimeTowerVerdict verdict)
{
    switch (verdict) {
    case PrimeTowerVerdict::Printed:
        return "A prime tower is printed on this plate and occupies the reported footprint.";
    case PrimeTowerVerdict::NotFff:
        return "The selected printer is not an FFF printer, which is the only technology with a prime tower.";
    case PrimeTowerVerdict::Disabled:
        return "The print preset has enable_prime_tower turned off.";
    case PrimeTowerVerdict::SingleFilamentProject:
        return "The project has a single filament, so nothing ever needs priming.";
    case PrimeTowerVerdict::GcodeOnlyMode:
        return "A G-code-only project has no tower to place.";
    case PrimeTowerVerdict::SequentialMultiObject:
        return "This plate prints by object with more than one object, so no tower is generated.";
    case PrimeTowerVerdict::EmptyPlate:
        return "This plate has no objects on it.";
    case PrimeTowerVerdict::SingleFilamentPlate:
        return "This plate uses a single filament, so no tower is generated even though the project is multi-filament.";
    }
    return "Unknown.";
}

ObjectBrimExtent object_brim_extent(const std::string& brim_type, double brim_width, double brim_object_gap)
{
    const double width = (std::isfinite(brim_width) && brim_width > 0.0) ? brim_width : 0.0;
    const double gap   = (std::isfinite(brim_object_gap) && brim_object_gap > 0.0) ? brim_object_gap : 0.0;

    ObjectBrimExtent out;
    if (brim_type == "no_brim" || brim_type == "inner_only") {
        // Neither reaches past the object's outline: no_brim prints nothing, and an inner brim is
        // printed inside the object's holes.
        out.extent_mm      = 0.0;
        out.upper_bound_mm = 0.0;
        out.exact          = true;
        return out;
    }

    if (brim_type == "outer_only" || brim_type == "outer_and_inner") {
        out.extent_mm      = gap + width;
        out.upper_bound_mm = out.extent_mm;
        out.exact          = true;
        return out;
    }

    // auto_brim recomputes the width per volume group at slice time; brim ears and painted brims
    // only touch part of the outline. The configured width is the best estimate available before
    // slicing, and the auto formula's own cap is the worst case.
    out.extent_mm      = gap + width;
    out.upper_bound_mm = gap + std::max(width, kAutoBrimWidthCapMm);
    out.exact          = false;
    return out;
}

Vec3d stable_camera_up(const Vec3d& camera_position, const Vec3d& target, const Vec3d& preferred_up)
{
    const Vec3d view_direction = camera_position - target;
    const double view_norm = view_direction.norm();
    const double up_norm   = preferred_up.norm();
    if (view_norm <= 0.0 || up_norm <= 0.0) return preferred_up;

    const Vec3d view_unit = view_direction / view_norm;
    const Vec3d up_unit   = preferred_up / up_norm;

    // sin of the angle between them. Below this the cross product is too short to normalize into a
    // trustworthy basis, long before it is exactly zero.
    constexpr double kParallelEpsilon = 1e-6;
    if (up_unit.cross(view_unit).norm() > kParallelEpsilon) return preferred_up;

    // Parallel: pick an axis that is not. +Y for the vertical view a plan render asks for, which is
    // the usual convention and puts plate +Y at the top of the image; +Z for anything else.
    const Vec3d fallback = (std::abs(view_unit.z()) > 0.5) ? Vec3d::UnitY() : Vec3d::UnitZ();
    return fallback;
}

}}} // namespace Slic3r::GUI::OrcaMCP
