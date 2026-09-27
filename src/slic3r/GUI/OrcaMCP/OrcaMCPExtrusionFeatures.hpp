// src/slic3r/GUI/OrcaMCP/OrcaMCPExtrusionFeatures.hpp
#pragma once
#include "libslic3r/ExtrusionEntity.hpp"

// The features MCP tools group extrusion roles into, so every tool names the same toolpath the same
// way: get_print_estimate's time_by_feature and the sliced layer plan read this one table. No wx:
// unit-tested in tests/slic3rutils/test_extrusion_features.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

enum class ExtrusionFeature
{
    perimeters,        // inner, outer and overhang walls, and gap fill
    infill,            // sparse, internal solid, top and bottom surfaces, bridges, ironing
    support,           // support and its transition layers
    support_interface,
    brim,
    skirt,
    prime_tower,       // the wipe tower; named as set_prime_tower_position and get_scene_info name it
    other,             // custom extrusions, and roles that name no feature (none, mixed)
};

inline ExtrusionFeature extrusion_feature_of(ExtrusionRole role)
{
    switch (role) {
    case erPerimeter:
    case erExternalPerimeter:
    case erOverhangPerimeter:
    case erGapFill: return ExtrusionFeature::perimeters;
    case erInternalInfill:
    case erSolidInfill:
    case erTopSolidInfill:
    case erBottomSurface:
    case erBridgeInfill:
    case erInternalBridgeInfill:
    case erIroning: return ExtrusionFeature::infill;
    case erSupportMaterial:
    case erSupportTransition: return ExtrusionFeature::support;
    case erSupportMaterialInterface: return ExtrusionFeature::support_interface;
    case erBrim: return ExtrusionFeature::brim;
    case erSkirt: return ExtrusionFeature::skirt;
    case erWipeTower: return ExtrusionFeature::prime_tower;
    default: return ExtrusionFeature::other;
    }
}

// The key a JSON response names a feature by.
inline const char* extrusion_feature_key(ExtrusionFeature feature)
{
    switch (feature) {
    case ExtrusionFeature::perimeters: return "perimeters";
    case ExtrusionFeature::infill: return "infill";
    case ExtrusionFeature::support: return "support";
    case ExtrusionFeature::support_interface: return "support_interface";
    case ExtrusionFeature::brim: return "brim";
    case ExtrusionFeature::skirt: return "skirt";
    case ExtrusionFeature::prime_tower: return "prime_tower";
    case ExtrusionFeature::other: return "other";
    }
    return "other";
}

// The perimeters feature split by wall, for a tool that needs the finer grain (the estimate: an outer
// wall printed slower is a common reason two slices differ). none for a role outside perimeters.
enum class WallKind { outer_wall, inner_wall, overhang_wall, gap_fill, none };

inline WallKind wall_kind_of(ExtrusionRole role)
{
    switch (role) {
    case erExternalPerimeter: return WallKind::outer_wall;
    case erPerimeter: return WallKind::inner_wall;
    case erOverhangPerimeter: return WallKind::overhang_wall;
    case erGapFill: return WallKind::gap_fill;
    default: return WallKind::none;
    }
}

// The key a JSON response names a wall by; nullptr for a role outside perimeters.
inline const char* wall_key(ExtrusionRole role)
{
    switch (wall_kind_of(role)) {
    case WallKind::outer_wall: return "outer_wall";
    case WallKind::inner_wall: return "inner_wall";
    case WallKind::overhang_wall: return "overhang_wall";
    case WallKind::gap_fill: return "gap_fill";
    case WallKind::none: break;
    }
    return nullptr;
}

}}} // namespace Slic3r::GUI::OrcaMCP
