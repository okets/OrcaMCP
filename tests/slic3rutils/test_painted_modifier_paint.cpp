#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintedModifier.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/SurfaceCollection.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

// Painted modifiers (upstream's ModelVolumeType::PAINTED_MODIFIER): a volume that shares the mesh and
// placement of the model part it is painted on and applies its own settings to the painted region.
// The paint is that volume's own FacetsAnnotation, so the fifth PaintMode writes it exactly as the
// gizmo's selector does; which volume a call addresses, what a depth may be, and what the refusals
// say are the decisions in OrcaMCPPaintedModifier.cpp. Only the slice check needs a Print, and it
// proves the write the MCP path produces is the data the slicer honours.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;
using json = nlohmann::json;

namespace {

bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Indexes into object.volumes: ids are read back from the vector, never assumed from call order
// (the object list re-sorts volumes, and the tests sort the same way).
int volume_index_of(const ModelObject& object, const ModelVolume* volume)
{
    for (std::size_t i = 0; i < object.volumes.size(); ++i)
        if (object.volumes[i] == volume)
            return int(i);
    return -1;
}

// One per facet: 1 on the facets facing +X, -1 (leave alone) everywhere else. The +X side of a cube.
std::vector<int> states_facing_x(const indexed_triangle_set& its)
{
    std::vector<int> states(its.indices.size(), -1);
    for (int facet = 0; facet < int(its.indices.size()); ++facet)
        if (its_face_normal(its, facet).dot(Vec3f::UnitX()) > 0.99f)
            states[facet] = 1;
    return states;
}

// A part, a second part, a mesh modifier, and a painted modifier over the first part -- the object
// list's order. `without_painted_modifier` leaves the modifier out, for the "there is none" refusals.
struct Scene
{
    Model       model;
    ModelObject* object  = nullptr;
    ModelVolume* host    = nullptr;
    ModelVolume* second  = nullptr;
    ModelVolume* modifier = nullptr;
    ModelVolume* pm      = nullptr;

    explicit Scene(bool without_painted_modifier = false)
    {
        object    = model.add_object();
        host      = object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
        second    = object->add_volume(TriangleMesh(its_make_cube(8., 8., 8.)));
        modifier  = object->add_volume(TriangleMesh(its_make_cube(30., 30., 30.)), ModelVolumeType::PARAMETER_MODIFIER);
        if (!without_painted_modifier)
            pm = object->add_painted_modifier(*host);
        object->sort_volumes(true);
        object->add_instance();
    }

    int index_of(const ModelVolume* volume) const { return volume_index_of(*object, volume); }
    int host_id() const { return index_of(host); }
    int second_id() const { return index_of(second); }
    int modifier_id() const { return index_of(modifier); }
    int pm_id() const { return index_of(pm); }
};

const PaintedModifierRequest* request_of(const std::variant<PaintedModifierRequest, std::string>& read)
{
    return std::get_if<PaintedModifierRequest>(&read);
}

const std::string* refusal_of(const std::variant<PaintedModifierRequest, std::string>& read)
{
    return std::get_if<std::string>(&read);
}

// A 20 mm cube with a painted modifier over it, painted on +X through the MCP write path. Built as
// upstream's tests/fff_print/test_painted_modifier.cpp builds its scene, but the facets go through
// build_paint_write + apply_paint_data -- what paint_object does -- rather than a gizmo selector.
struct SlicedPaintedCube
{
    Model              model;
    Print              print;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    ModelObject*       object = nullptr;
    ModelVolume*       host   = nullptr;
    ModelVolume*       pm     = nullptr;

    explicit SlicedPaintedCube(int painted_walls = 5)
    {
        config.set_deserialize_strict({{"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}, {"wall_loops", "2"}});
        object = model.add_object();
        host   = object->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
        object->add_instance();
        pm = object->add_painted_modifier(*host);
        pm->config.set_key_value("wall_loops", new ConfigOptionInt(painted_walls)); // painted_modifier_depth stays 0
        PaintWrite write;
        REQUIRE(build_paint_write(pm->mesh(), PaintMode::PaintedModifier, states_facing_x(pm->mesh().its), true, PaintData{}, write));
        REQUIRE(apply_paint_data(*pm, PaintMode::PaintedModifier, std::move(write.data)));
    }

    void slice()
    {
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.process();
    }

    const Layer& layer_at(double z) const
    {
        const PrintObject& print_object = *print.objects().front();
        const auto         it           = std::find_if(print_object.layers().begin(), print_object.layers().end(),
                                                       [z](const Layer* l) { return l->slice_z > z; });
        REQUIRE(it != print_object.layers().end());
        return **it;
    }
};

// Area in mm² of the layer's regions printed with the given number of walls.
double area_with_walls(const Layer& layer, int walls)
{
    double area = 0.;
    for (const LayerRegion* region : layer.regions())
        if (region->region().config().wall_loops.value == walls)
            for (const Surface& surface : region->slices.surfaces)
                area += surface.expolygon.area();
    return area * SCALING_FACTOR * SCALING_FACTOR;
}

} // namespace

// ---- Task 1: the paint mode ----

TEST_CASE("mode painted_modifier parses, names itself and reads its two states back", "[McpPaint][orcamcp]")
{
    PaintMode mode{};
    REQUIRE(parse_paint_mode("Painted_Modifier", mode));
    CHECK(mode == PaintMode::PaintedModifier);
    CHECK(std::string(paint_mode_name(mode)) == "painted_modifier");
    CHECK(max_paint_state_for(mode) == 1);
    int s = -1;
    REQUIRE(parse_paint_state(mode, "painted", s));
    CHECK(s == 1);
    REQUIRE(parse_paint_state(mode, "enforcer", s));
    CHECK(s == 1);
    REQUIRE(parse_paint_state(mode, "none", s));
    CHECK(s == 0);
    CHECK_FALSE(parse_paint_state(mode, "blocker", s));
    CHECK(paint_state_label(mode, 1) == "painted");
    CHECK(paint_state_label(mode, 0) == "none");
}

TEST_CASE("mode painted_modifier writes the modifier's own facets and no other annotation", "[McpPaint][orcamcp]")
{
    Scene scene;
    REQUIRE(apply_facet_states(*scene.pm, PaintMode::PaintedModifier, states_facing_x(scene.pm->mesh().its), true));

    CHECK_FALSE(scene.pm->painted_modifier_facets.empty());
    CHECK(scene.pm->mmu_segmentation_facets.empty());
    CHECK(scene.pm->supported_facets.empty());
    CHECK(scene.pm->seam_facets.empty());
    CHECK(scene.pm->fuzzy_skin_facets.empty());
    // The host keeps its own annotations -- its own painted_modifier_facets stays empty (a part has none).
    CHECK(scene.host->painted_modifier_facets.empty());
    CHECK(scene.host->supported_facets.empty());
}

TEST_CASE("a painted modifier written by paint_object's path equals the gizmo's selector", "[McpPaint][orcamcp]")
{
    Scene scene;
    // What GLGizmoPaintedModifier writes: one TriangleSelector over the modifier's mesh, ENFORCER on
    // the painted facets, serialized.
    TriangleSelector selector(scene.pm->mesh());
    for (int facet = 0; facet < int(scene.pm->mesh().its.indices.size()); ++facet)
        if (its_face_normal(scene.pm->mesh().its, facet).dot(Vec3f::UnitX()) > 0.99f)
            selector.set_facet(facet, EnforcerBlockerType::ENFORCER);
    const PaintData gizmo_data = selector.serialize();

    REQUIRE(apply_facet_states(*scene.pm, PaintMode::PaintedModifier, states_facing_x(scene.pm->mesh().its), true));
    CHECK(scene.pm->painted_modifier_facets.get_data() == gizmo_data);
}

TEST_CASE("a cube painted on +X through paint_object's path slices with the modifier's walls on that side only", "[McpPaint][orcamcp]")
{
    SlicedPaintedCube scene;
    scene.slice();
    const Layer& layer = scene.layer_at(10.);

    // Painting one side of a 20 mm square claims the triangle of the square nearest that side: 100 mm²
    // at 5 walls, the rest of the 400 mm² at the object's 2 (upstream's nearest_side_area scene).
    const double nearest_side_area = 20. * 10. / 2.;
    CHECK_THAT(area_with_walls(layer, 5), WithinAbs(nearest_side_area, 1.));
    CHECK_THAT(area_with_walls(layer, 2), WithinAbs(400. - nearest_side_area, 1.));
}

// ---- Task 2: the decisions apart from the app ----

TEST_CASE("painted modifier refusals name what to send", "[McpPaint][orcamcp]")
{
    const Scene scene;

    // Without volume_id the call is pointed at the modifier it should name.
    const auto needs_id = painted_modifier_target_refusal(0, *scene.object, std::nullopt);
    REQUIRE(needs_id.has_value());
    CHECK(mentions(*needs_id, "volume_id"));
    CHECK(mentions(*needs_id, std::to_string(scene.pm_id())));

    // With no painted modifier at all, the refusal is the call that adds one.
    const Scene bare(true);
    const auto  no_modifier = painted_modifier_target_refusal(0, *bare.object, std::nullopt);
    REQUIRE(no_modifier.has_value());
    CHECK(mentions(*no_modifier, "add_volume"));
    CHECK(mentions(*no_modifier, "painted_modifier"));

    // A part is not a painted modifier.
    const auto on_part = painted_modifier_target_refusal(0, *scene.object, scene.host_id());
    REQUIRE(on_part.has_value());
    CHECK(mentions(*on_part, "is a part, not a painted modifier"));

    // Out of range gives the object's volume range.
    const auto out_of_range = painted_modifier_target_refusal(0, *scene.object, int(scene.object->volumes.size()));
    REQUIRE(out_of_range.has_value());
    CHECK(mentions(*out_of_range, "0 to " + std::to_string(int(scene.object->volumes.size()) - 1)));

    // The modifier on its part addresses fine.
    CHECK(painted_modifier_target_refusal(0, *scene.object, scene.pm_id()) == std::nullopt);

    // Once the host is not a part, the refusal is the gizmo's own words for it, and the delete that cleans up.
    Scene broken;
    broken.host->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    const auto orphan = painted_modifier_target_refusal(0, *broken.object, broken.pm_id());
    REQUIRE(orphan.has_value());
    CHECK(mentions(*orphan, "The part this modifier was painted on has been removed."));
    CHECK(mentions(*orphan, "delete_object"));
}

TEST_CASE("add_volume's painted_modifier arguments default as the object list's menu does", "[McpPaint][orcamcp]")
{
    const Scene scene;

    const auto bare = read_painted_modifier_request(0, *scene.object, json::object());
    REQUIRE(request_of(bare) != nullptr);
    CHECK(request_of(bare)->host_volume_id == scene.host_id()); // the first model part, as the menu's default
    CHECK(request_of(bare)->depth_mm == std::nullopt);

    const auto named = read_painted_modifier_request(0, *scene.object, {{"host_volume_id", scene.second_id()}});
    REQUIRE(request_of(named) != nullptr);
    CHECK(request_of(named)->host_volume_id == scene.second_id());

    // Only model parts host painted modifiers (upstream's rule).
    const auto on_modifier = read_painted_modifier_request(0, *scene.object, {{"host_volume_id", scene.modifier_id()}});
    REQUIRE(refusal_of(on_modifier) != nullptr);
    CHECK(mentions(*refusal_of(on_modifier), "model part"));
    const auto out_of_range = read_painted_modifier_request(0, *scene.object, {{"host_volume_id", 99}});
    CHECK(refusal_of(out_of_range) != nullptr);

    // depth_mm: the gizmo's Depth field, 0 to 1000 inclusive, never clamped.
    for (const json& bad : {json(-1.f), json(1000.5), json("3"), json(nullptr), json(true)}) {
        const auto   read = read_painted_modifier_request(0, *scene.object, {{"depth_mm", bad}});
        INFO("depth_mm " << bad.dump());
        REQUIRE(refusal_of(read) != nullptr);
        CHECK(mentions(*refusal_of(read), "0 to 1000"));
    }
    const auto shallow = read_painted_modifier_request(0, *scene.object, {{"depth_mm", 0}});
    REQUIRE(request_of(shallow) != nullptr);
    CHECK(request_of(shallow)->depth_mm == std::optional<float>(0.f));
    const auto deep = read_painted_modifier_request(0, *scene.object, {{"depth_mm", 1000}});
    REQUIRE(request_of(deep) != nullptr);
    CHECK(request_of(deep)->depth_mm == std::optional<float>(1000.f));

    // The type shares the host's mesh: it has no shape, size or file of its own.
    for (const char* key : {"shape", "size", "file_path"}) {
        const auto read = read_painted_modifier_request(0, *scene.object, {{key, "cube"}});
        INFO(key);
        REQUIRE(refusal_of(read) != nullptr);
        CHECK(mentions(*refusal_of(read), "has no shape of its own"));
    }

    // And the two painted-modifier arguments belong to that type alone.
    const auto misplaced_host = painted_modifier_argument_refusal("modifier", {{"host_volume_id", scene.host_id()}});
    REQUIRE(misplaced_host.has_value());
    CHECK(mentions(*misplaced_host, "type painted_modifier"));
    CHECK(painted_modifier_argument_refusal("modifier", {{"depth_mm", 2}}).has_value());
    CHECK(painted_modifier_argument_refusal("painted_modifier", {{"host_volume_id", scene.host_id()}}) == std::nullopt);
    CHECK(painted_modifier_argument_refusal("part", json::object()) == std::nullopt);
}

TEST_CASE("a painted modifier's row says what it does and its coverage is opt-in", "[McpPaint][orcamcp]")
{
    Scene scene;
    const json fresh = painted_modifier_row_fields(*scene.object, scene.pm_id());
    CHECK(fresh == json({{"host_volume_id", scene.host_id()}, {"depth_mm", 0.f}, {"painted", false}, {"settings", json::array()}}));

    // Paint the +X side and give the modifier walls of its own.
    REQUIRE(apply_facet_states(*scene.pm, PaintMode::PaintedModifier, states_facing_x(scene.pm->mesh().its), true));
    scene.pm->config.set_key_value("wall_loops", new ConfigOptionInt(6));
    const json painted = painted_modifier_row_fields(*scene.object, scene.pm_id());
    CHECK(painted["painted"] == true);
    CHECK(painted["settings"] == json::array({"wall_loops"}));
    CHECK_FALSE(painted.contains("painted_facets")); // the row stays cheap: coverage builds a selector

    const json coverage = painted_modifier_coverage_json(*scene.pm);
    CHECK(coverage["painted_facets"].get<int>() > 0);
    CHECK_THAT(coverage["coverage_percent"].get<double>(), WithinAbs(100. / 6., 0.5)); // the +X face of a cube: 1/6 of its area

    // A part's row carries none of this.
    CHECK(painted_modifier_row_fields(*scene.object, scene.host_id()) == json::object());
    CHECK(painted_modifier_row_fields(*scene.object, scene.modifier_id()) == json::object());
}

TEST_CASE("a stale modifier is brought back onto its host's mesh before it is read", "[McpPaint][orcamcp]")
{
    Scene scene;
    REQUIRE(apply_facet_states(*scene.pm, PaintMode::PaintedModifier, states_facing_x(scene.pm->mesh().its), true));
    // A repair leaves the host a new mesh over the same shape -- what the modifier must paint on now.
    scene.host->set_mesh(TriangleMesh(its_make_cube(10., 10., 10.)));
    CHECK(scene.pm->mesh_ptr() != scene.host->mesh_ptr());
    CHECK(ensure_painted_modifiers_synced(*scene.object));
    CHECK(scene.pm->mesh_ptr() == scene.host->mesh_ptr());
    CHECK_FALSE(ensure_painted_modifiers_synced(*scene.object));
}

TEST_CASE("turning a host into anything but a part leaves its painted modifiers without one", "[McpPaint][orcamcp]")
{
    const Scene scene;
    const std::vector<int> orphaned =
        painted_modifiers_orphaned_by_type_change(*scene.object, scene.host_id(), ModelVolumeType::PARAMETER_MODIFIER);
    CHECK(orphaned == std::vector<int>{scene.pm_id()});
    CHECK(painted_modifiers_orphaned_by_type_change(*scene.object, scene.host_id(), ModelVolumeType::MODEL_PART).empty());
    CHECK(painted_modifiers_orphaned_by_type_change(*scene.object, scene.second_id(), ModelVolumeType::PARAMETER_MODIFIER).empty());
}

// ---- Task 4: paint_object's painted_modifier arguments ----

TEST_CASE("paint_object without a selection is refused, except a painted modifier's depth alone", "[McpPaint][orcamcp]")
{
    const auto color = missing_selection_refusal(PaintChannel(PaintMode::Color), false, false);
    REQUIRE(color.has_value());
    CHECK(mentions(*color, "bands, box, sphere, all, visible, connected, component or state"));
    CHECK(missing_selection_refusal(PaintChannel(PaintMode::Support), false, true).has_value()); // depth belongs to one mode
    CHECK(missing_selection_refusal(PaintChannel(PaintMode::PaintedModifier), false, true) == std::nullopt);
    const auto neither = missing_selection_refusal(PaintChannel(PaintMode::PaintedModifier), false, false);
    REQUIRE(neither.has_value());
    CHECK(mentions(*neither, "depth_mm"));
    CHECK(missing_selection_refusal(PaintChannel(PaintMode::Color), true, false) == std::nullopt);
}

TEST_CASE("depth_mm belongs to mode painted_modifier and its range", "[McpPaint][orcamcp]")
{
    const auto on_support = depth_argument_refusal(PaintChannel(PaintMode::Support), {{"depth_mm", 3}});
    REQUIRE(on_support.has_value());
    CHECK(mentions(*on_support, "mode painted_modifier"));
    const auto negative = depth_argument_refusal(PaintChannel(PaintMode::PaintedModifier), {{"depth_mm", -1}});
    REQUIRE(negative.has_value());
    CHECK(mentions(*negative, "0 to 1000"));
    CHECK(depth_argument_refusal(PaintChannel(PaintMode::PaintedModifier), {{"depth_mm", 3}}) == std::nullopt);
    CHECK(depth_argument_refusal(PaintChannel(PaintMode::Color), json::object()) == std::nullopt);
}

TEST_CASE("a connected seed on the host's facet is taken as the painted modifier's", "[McpPaint][orcamcp]")
{
    const Scene         scene;
    const PaintChannel  pm_channel(PaintMode::PaintedModifier);
    // pick_facet names the part the ray hit: the host. Same mesh, same facet numbers.
    CHECK(seed_volume_for_mode(*scene.object, pm_channel, scene.pm_id(), scene.host_id()) == scene.pm_id());
    CHECK(seed_volume_for_mode(*scene.object, pm_channel, scene.pm_id(), scene.second_id()) == scene.second_id());
    CHECK(seed_volume_for_mode(*scene.object, PaintChannel(PaintMode::Color), scene.pm_id(), scene.host_id()) == scene.host_id());
}

// ---- Task 5: reading and clearing ----

TEST_CASE("get_object_paint and clear_object_paint name painted_modifier as a mode of its own", "[McpPaint][orcamcp]")
{
    PaintSelection selection;
    std::string    error;
    REQUIRE(resolve_paint_selection({{"mode", "painted_modifier"}}, {}, true, selection, error));
    CHECK(selection.modes == std::vector<PaintMode>{PaintMode::PaintedModifier});
    CHECK(selection.named_mode);
    CHECK(selection.label == "painted_modifier");
    CHECK_FALSE(selection.texture);
    // Without a mode, the print-paint modes only: a painted modifier's area is neither read nor cleared.
    REQUIRE(resolve_paint_selection(json::object(), {}, true, selection, error));
    CHECK(selection.modes == std::vector<PaintMode>{PaintMode::Color, PaintMode::Support, PaintMode::Seam, PaintMode::FuzzySkin});

    // The target follows the mode: painted modifiers for this one, parts for every other (and for a bad mode,
    // which the selection then refuses).
    CHECK(target_channel_of({{"mode", "painted_modifier"}}).mode == PaintMode::PaintedModifier);
    CHECK(target_channel_of({{"mode", "support"}}).mode == PaintMode::Color);
    CHECK(target_channel_of(json::object()).mode == PaintMode::Color);
    CHECK(target_channel_of({{"mode", 3}}).mode == PaintMode::Color);

    const Scene scene;
    CHECK(first_model_part_id(*scene.object) == scene.host_id());
}
