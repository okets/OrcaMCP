#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

#include "slic3r/GUI/OrcaMCP/OrcaMCPExtrusionFeatures.hpp"

// The one table that groups extrusion roles into the features MCP tools report. Two tools with two
// tables named the same toolpath two ways; this is what keeps the estimate and the layer plan agreeing.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;

TEST_CASE("every wall role and gap fill belong to perimeters", "[orcamcp][ExtrusionFeatures]")
{
    const ExtrusionRole role = GENERATE(erPerimeter, erExternalPerimeter, erOverhangPerimeter, erGapFill);
    CHECK(extrusion_feature_of(role) == ExtrusionFeature::perimeters);
}

TEST_CASE("sparse, solid, surface, bridge and ironing roles belong to infill", "[orcamcp][ExtrusionFeatures]")
{
    const ExtrusionRole role = GENERATE(erInternalInfill, erSolidInfill, erTopSolidInfill, erBottomSurface, erBridgeInfill,
                                        erInternalBridgeInfill, erIroning);
    CHECK(extrusion_feature_of(role) == ExtrusionFeature::infill);
}

TEST_CASE("support and its transition layers are support, and the interface is its own feature", "[orcamcp][ExtrusionFeatures]")
{
    CHECK(extrusion_feature_of(erSupportMaterial) == ExtrusionFeature::support);
    CHECK(extrusion_feature_of(erSupportTransition) == ExtrusionFeature::support);
    CHECK(extrusion_feature_of(erSupportMaterialInterface) == ExtrusionFeature::support_interface);
}

TEST_CASE("the wipe tower is reported as the prime tower", "[orcamcp][ExtrusionFeatures]")
{
    CHECK(extrusion_feature_of(erWipeTower) == ExtrusionFeature::prime_tower);
    CHECK(std::string(extrusion_feature_key(ExtrusionFeature::prime_tower)) == "prime_tower");
}

TEST_CASE("brim and skirt are features of their own", "[orcamcp][ExtrusionFeatures]")
{
    CHECK(extrusion_feature_of(erBrim) == ExtrusionFeature::brim);
    CHECK(extrusion_feature_of(erSkirt) == ExtrusionFeature::skirt);
}

TEST_CASE("roles that name no feature fall into other", "[orcamcp][ExtrusionFeatures]")
{
    const ExtrusionRole role = GENERATE(erNone, erCustom, erMixed);
    CHECK(extrusion_feature_of(role) == ExtrusionFeature::other);
}

TEST_CASE("every feature has the key the layer plan and the estimate share", "[orcamcp][ExtrusionFeatures]")
{
    CHECK(std::string(extrusion_feature_key(ExtrusionFeature::perimeters)) == "perimeters");
    CHECK(std::string(extrusion_feature_key(ExtrusionFeature::infill)) == "infill");
    CHECK(std::string(extrusion_feature_key(ExtrusionFeature::support)) == "support");
    CHECK(std::string(extrusion_feature_key(ExtrusionFeature::support_interface)) == "support_interface");
    CHECK(std::string(extrusion_feature_key(ExtrusionFeature::brim)) == "brim");
    CHECK(std::string(extrusion_feature_key(ExtrusionFeature::skirt)) == "skirt");
    CHECK(std::string(extrusion_feature_key(ExtrusionFeature::other)) == "other");
}

TEST_CASE("the wall split names each perimeters role and nothing else", "[orcamcp][ExtrusionFeatures]")
{
    CHECK(std::string(wall_key(erExternalPerimeter)) == "outer_wall");
    CHECK(std::string(wall_key(erPerimeter)) == "inner_wall");
    CHECK(std::string(wall_key(erOverhangPerimeter)) == "overhang_wall");
    CHECK(std::string(wall_key(erGapFill)) == "gap_fill");
    CHECK(wall_key(erInternalInfill) == nullptr);
}
