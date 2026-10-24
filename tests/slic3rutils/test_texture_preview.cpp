// The texture preview of render_plate_view, apart from the app (OrcaMCPTexturePreview.cpp): the panel's preview from a
// bake's input, what makes a computed preview reusable, the cache and the report. The plan runs headless on a sheet.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPTexturePreview.hpp"
#include "slic3r/GUI/TextureDisplacementObject.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

indexed_triangle_set sheet(int cells)
{
    indexed_triangle_set its;
    for (int y = 0; y <= cells; ++y)
        for (int x = 0; x <= cells; ++x)
            its.vertices.emplace_back(20.f * float(x) / float(cells), 20.f * float(y) / float(cells), 0.f);
    const auto at = [cells](int x, int y) { return y * (cells + 1) + x; };
    for (int y = 0; y < cells; ++y)
        for (int x = 0; x < cells; ++x) {
            its.indices.emplace_back(at(x, y), at(x + 1, y), at(x + 1, y + 1));
            its.indices.emplace_back(at(x, y), at(x + 1, y + 1), at(x, y + 1));
        }
    return its;
}

TextureDisplacementLayer ramp_layer(int slot)
{
    const int            n = 16;
    std::vector<uint8_t> gray;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            gray.push_back(uint8_t((x * 16 + y * 5) % 256));
    ScopedTemporaryFile png(".png");
    REQUIRE(png::write_gray_to_file(png.string(), size_t(n), size_t(n), gray));
    std::ifstream              in(png.string(), std::ios::binary);
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    TextureDisplacementLayer layer;
    layer.slot              = slot;
    layer.name              = "ramp";
    layer.depth_mm          = 1.f;
    layer.image_data        = std::make_shared<std::vector<unsigned char>>(std::move(bytes));
    layer.tiling_scale      = 10.f;
    layer.projection_method = TextureProjectionMethod::Triplanar;
    return layer;
}

void paint(ModelVolume& volume, int slot, int count)
{
    TriangleSelector selector(volume.mesh());
    for (int i = 0; i < count; ++i)
        selector.set_facet(i, EnforcerBlockerType::ENFORCER);
    volume.texture_displacement_facet(slot).set(selector);
}

// An object with one painted sheet on a v2 or classic pipeline.
struct Fixture
{
    Model        model;
    ModelObject* object = nullptr;
    ModelVolume* volume = nullptr;

    explicit Fixture(bool v2, int cells = 12)
    {
        object = model.add_object();
        object->add_instance();
        volume = object->add_volume(TriangleMesh(sheet(cells)), ModelVolumeType::MODEL_PART, false);
        volume->texture_displacement_layers = {ramp_layer(0)};
        volume->texture_displacement_options.pipeline_v2 = v2;
        paint(*volume, 0, int(volume->mesh().its.indices.size()));
    }
    TextureDisplacementBakeInput capture() const { return capture_texture_bake(*object, {}, false); }
};

PrintableColor red()
{
    PrintableColor c;
    c.rgb = Vec3f(1.f, 0.f, 0.f);
    return c;
}

PreviewKey key_of(const TextureDisplacementBakeInput& input, const PreviewColours& colours = {}, int budget = 0)
{
    return preview_key(input, colours, budget);
}

std::shared_ptr<const PreviewDraw> draw_of(const Fixture& f)
{
    const TextureDisplacementBakeInput input = f.capture();
    PreviewResult                      result = compute_preview(input, nullptr, {0}, {}, [](int) { return true; });
    REQUIRE_FALSE(result.canceled);
    return std::make_shared<const PreviewDraw>(std::move(result.draw));
}

} // namespace

TEST_CASE("The preview's input sets the triangle budget of v2 parts to the render's cap, or keeps the asked one up to it", "[TexturePreview][orcamcp]")
{
    Fixture v2(true);
    v2.volume->texture_displacement_options.v2_max_triangles_k = 40;
    const TextureDisplacementBakeInput before = v2.capture();
    TextureDisplacementBakeInput       after  = preview_input(before);
    REQUIRE(after.volumes.size() == 1);
    CHECK(before.volumes[0].options.v2_max_triangles_k == 40);
    CHECK(after.volumes[0].options.v2_max_triangles_k == max_part_triangles_k);
    CHECK(max_part_triangles_k * 1000 == int(max_part_triangles));
    CHECK(preview_input(before, true).volumes[0].options.v2_max_triangles_k == 40);   // an asked budget is kept
    for (const int asked : {0, 4000}) {
        TextureDisplacementBakeInput big = before;
        big.volumes[0].options.v2_max_triangles_k = asked;
        CHECK(preview_input(big, true).volumes[0].options.v2_max_triangles_k == max_part_triangles_k);   // none, or more than a render holds: the cap
    }
    TextureDisplacementBakeInput automatic = before;
    automatic.volumes[0].options.v2_max_triangles_k = -1;
    CHECK(preview_input(automatic, true).volumes[0].options.v2_max_triangles_k == -1);   // automatic is the bake's 750 k, under the cap
    CHECK(key_of(before, {}, 0) != preview_key(before, {}, 0, true));
    after.volumes[0].options.v2_max_triangles_k = 40;
    CHECK(same_bake_volume(after.volumes[0], before.volumes[0], false));

    Fixture classic(false);
    classic.volume->texture_displacement_options.v2_max_triangles_k = 40;
    CHECK(preview_input(classic.capture()).volumes[0].options.v2_max_triangles_k == 40);
}

TEST_CASE("Two captures have the same key until something the preview draws changes", "[TexturePreview][orcamcp]")
{
    Fixture f(true);
    const TextureDisplacementBakeInput first = f.capture();
    const PreviewKey                   key   = key_of(first);
    CHECK(same_preview_input(first, f.capture()));
    CHECK(key == key_of(f.capture()));
    CHECK(key != key_of(first, {}, 750));   // the classic preparation's budget

    SECTION("the budget is not part of what is drawn") {
        f.volume->texture_displacement_options.v2_max_triangles_k = 77;
        CHECK(same_preview_input(first, f.capture()));
        CHECK(key == key_of(f.capture()));
    }
    SECTION("where the panel was left is not either") {
        f.volume->texture_displacement_options.active_slot = 3;
        CHECK(key == key_of(f.capture()));
    }
    SECTION("the paint") {
        f.volume->texture_displacement_facet(0).reset();
        paint(*f.volume, 0, int(f.volume->mesh().its.indices.size()) - 2);
        CHECK_FALSE(same_preview_input(first, f.capture()));
        CHECK(key != key_of(f.capture()));
    }
    SECTION("a layer setting") {
        f.volume->texture_displacement_layers.front().depth_mm += 0.5f;
        CHECK_FALSE(same_preview_input(first, f.capture()));
        CHECK(key != key_of(f.capture()));
    }
    SECTION("an image adjustment the panel shows") {
        f.volume->texture_displacement_layers.front().preview_adjust.brightness = 0.4f;
        CHECK(key != key_of(f.capture()));
    }
    SECTION("the image") {
        auto bytes = std::make_shared<std::vector<unsigned char>>(*f.volume->texture_displacement_layers.front().image_data);
        bytes->back() ^= 0x55;
        f.volume->texture_displacement_layers.front().image_data = bytes;
        CHECK(key != key_of(f.capture()));
    }
    SECTION("an option the bake reads") {
        f.volume->texture_displacement_options.v2_refine_mm = 0.7f;
        CHECK_FALSE(same_preview_input(first, f.capture()));
        CHECK(key != key_of(f.capture()));
    }
    SECTION("the mesh") {
        indexed_triangle_set nudged = f.volume->mesh().its;
        nudged.vertices.front().z() += 0.5f;
        f.volume->set_mesh(TriangleMesh(nudged));
        CHECK(key != key_of(f.capture()));
    }
    SECTION("the part's placement") {
        f.volume->set_offset(Vec3d(0., 0., 3.));
        CHECK(key != key_of(f.capture()));
    }
    SECTION("the palette") {
        TextureDisplacementBakeInput coloured = f.capture();
        coloured.color.palette = {red()};
        CHECK(key != key_of(coloured));
        TextureDisplacementBakeInput other = coloured;
        CHECK(key_of(coloured) == key_of(other));
        other.color.palette.front().rgb = Vec3f(0.f, 1.f, 0.f);
        CHECK(key_of(coloured) != key_of(other));
    }
    SECTION("the colour paint and the extruder colours") {
        PreviewColours colours;
        colours.extruders = {ColorRGBA(1.f, 0.f, 0.f, 1.f)};
        const PreviewKey base = key_of(first, colours);
        CHECK(base != key);
        colours.extruders = {ColorRGBA(0.f, 1.f, 0.f, 1.f)};
        CHECK(key_of(first, colours) != base);
        colours.extruders = {ColorRGBA(1.f, 0.f, 0.f, 1.f)};
        TriangleSelector selector(f.volume->mesh());
        selector.set_facet(0, EnforcerBlockerType::ENFORCER);
        f.volume->mmu_segmentation_facets.set(selector);
        colours.colour_paint = {f.volume->mmu_segmentation_facets.get_data()};
        CHECK(key_of(first, colours) != base);
    }
}

TEST_CASE("The stamp follows the masks' timestamps, not their content", "[TexturePreview][orcamcp]")
{
    Fixture f(true);
    const PreviewStamp stamp = capture_preview_stamp(*f.object);
    CHECK(preview_stamp_current(*f.object, stamp));

    SECTION("a mask written again with the same content is a new timestamp") {
        const auto data   = f.volume->texture_displacement_facet(0).get_data();
        const auto before = f.volume->texture_displacement_facet(0).timestamp();
        f.volume->texture_displacement_facet(0).set_data(TriangleSelector::TriangleSplittingData(data));
        REQUIRE(f.volume->texture_displacement_facet(0).get_data() == data);
        REQUIRE(f.volume->texture_displacement_facet(0).timestamp() != before);
        CHECK_FALSE(preview_stamp_current(*f.object, stamp));
        CHECK(key_of(capture_texture_bake(*f.object, {}, false)) == key_of(f.capture()));   // the content key still matches
    }
    SECTION("a layer edited") {
        f.volume->texture_displacement_layers.front().depth_mm += 1.f;
        CHECK_FALSE(preview_stamp_current(*f.object, stamp));
    }
    SECTION("an image replaced by a different buffer") {
        auto bytes = std::make_shared<std::vector<unsigned char>>(*f.volume->texture_displacement_layers.front().image_data);
        f.volume->texture_displacement_layers.front().image_data = bytes;   // same bytes: still the same layer
        CHECK(preview_stamp_current(*f.object, stamp));
        bytes->back() ^= 1;
        f.volume->texture_displacement_layers.front().image_data = std::make_shared<std::vector<unsigned char>>(*bytes);
        CHECK_FALSE(preview_stamp_current(*f.object, stamp));
    }
    SECTION("the mesh, the placement and the options") {
        f.volume->texture_displacement_options.active_slot = 2;
        CHECK(preview_stamp_current(*f.object, stamp));
        f.volume->texture_displacement_options.v2_refine_mm = 0.9f;
        CHECK_FALSE(preview_stamp_current(*f.object, stamp));
        f.volume->texture_displacement_options.v2_refine_mm = stamp.options.v2_refine_mm;
        CHECK(preview_stamp_current(*f.object, stamp));
        f.volume->set_offset(Vec3d(1., 0., 0.));
        CHECK_FALSE(preview_stamp_current(*f.object, stamp));
    }
    SECTION("a part that is gone") {
        f.object->delete_volume(0);
        CHECK_FALSE(preview_stamp_current(*f.object, stamp));
    }
}

TEST_CASE("The preview is the bake without simplification: it moves vertices and keeps more triangles than a budgeted bake", "[TexturePreview][orcamcp]")
{
    Fixture f(true, 16);
    f.volume->texture_displacement_options.v2_max_triangles_k = 1;
    const TextureDisplacementBakeInput input = f.capture();
    REQUIRE(input.volumes.size() == 1);

    const TextureBakePlan budgeted = plan_texture_bake(input, [](int) { return true; });
    const TextureBakePlan preview  = plan_texture_bake(preview_input(input), [](int) { return true; });
    REQUIRE_FALSE(budgeted.canceled);
    REQUIRE_FALSE(preview.canceled);
    CHECK(texture_bake_moved(preview_input(input), preview, 0));
    float highest = 0.f;
    for (const Vec3f& v : preview.results.front().mesh.its.vertices)
        highest = std::max(highest, std::abs(v.z()));
    CHECK(highest > 0.1f);
    CHECK(preview.results.front().mesh.its.indices.size() > budgeted.results.front().mesh.its.indices.size());

    const PreviewResult computed = compute_preview(input, nullptr, {0}, {}, [](int) { return true; });
    REQUIRE_FALSE(computed.canceled);
    REQUIRE(computed.draw.parts.size() == 1);
    CHECK(computed.draw.parts[0].triangles == preview.results.front().mesh.its.indices.size());
    CHECK(computed.draw.parts[0].triangles_base == input.volumes[0].base_mesh.indices.size());
    CHECK(computed.draw.preparation.empty());
}

TEST_CASE("A preview whose progress hook says no is cancelled", "[TexturePreview][orcamcp]")
{
    Fixture f(true);
    CHECK(compute_preview(f.capture(), nullptr, {0}, {}, [](int) { return false; }).canceled);
}

TEST_CASE("The classic pipeline previews the mesh its preparation leaves, as the bake does", "[TexturePreview][orcamcp]")
{
    Fixture f(false);
    const TextureDisplacementBakeInput input = f.capture();
    // A tiny budget leaves the sheet as it is; a mesh coarser than the texture is refined by the standard preparation.
    TextureDisplacementPrepareInput prepare = capture_texture_prepare(*f.object, GLGizmoTextureDisplacement::standard_prepare_params(750),
                                                                      TexturePrepareScope::PaintedParts, {}, std::string());
    REQUIRE(prepare.volumes.size() == 1);
    const PreviewResult plain    = compute_preview(input, nullptr, {0}, {}, [](int) { return true; });
    const PreviewResult prepared = compute_preview(input, &prepare, {0}, {}, [](int) { return true; });
    REQUIRE_FALSE(plain.canceled);
    REQUIRE_FALSE(prepared.canceled);
    CHECK(plain.draw.preparation.empty());
    CHECK((prepared.draw.preparation == "applied" || prepared.draw.preparation == "unchanged"));
    REQUIRE(prepared.draw.parts.size() == 1);
    if (prepared.draw.preparation == "applied") {
        // the preparation planned on a copy: the model is untouched, the preview has the refined mesh's triangles
        CHECK(prepared.draw.parts[0].triangles != plain.draw.parts[0].triangles);
        CHECK(f.volume->mesh().its.indices.size() == input.volumes[0].base_mesh.indices.size());
    } else {
        CHECK(prepared.draw.parts[0].triangles == plain.draw.parts[0].triangles);
    }
    CHECK(compute_preview(input, &prepare, {0}, {}, [](int) { return false; }).canceled);
}

TEST_CASE("The part's own colour paint is carried onto the preview and the rest draws in its colour", "[TexturePreview][orcamcp]")
{
    Fixture f(false);   // classic without a preparation: the triangles stay, so triangle i is still triangle i
    const int triangles = int(f.volume->mesh().its.indices.size());
    TriangleSelector selector(f.volume->mesh());
    for (int i = 0; i < 10; ++i)
        selector.set_facet(i, EnforcerBlockerType::ENFORCER);   // extruder 1
    f.volume->mmu_segmentation_facets.set(selector);

    PreviewColours colours;
    colours.extruders    = {ColorRGBA(1.f, 0.f, 0.f, 1.f), ColorRGBA(0.f, 1.f, 0.f, 1.f)};
    colours.colour_paint = {f.volume->mmu_segmentation_facets.get_data()};
    const PreviewResult result = compute_preview(f.capture(), nullptr, {0}, colours, [](int) { return true; });
    REQUIRE_FALSE(result.canceled);
    const PreviewPart& part = result.draw.parts.at(0);
    REQUIRE(part.mesh.indices.size() == size_t(triangles));
    REQUIRE(part.runs.size() == 2);
    CHECK(part.runs[0].base);
    CHECK(part.runs[0].range == std::make_pair(size_t(0), size_t(triangles - 10) * 3));
    CHECK_FALSE(part.runs[1].base);
    CHECK(part.runs[1].color.r() == 1.f);
    CHECK(part.runs[1].range == std::make_pair(size_t(triangles - 10) * 3, size_t(triangles) * 3));

    SECTION("without colour paint there is one run in the part's colour") {
        const PreviewResult plain = compute_preview(f.capture(), nullptr, {0}, {}, [](int) { return true; });
        REQUIRE(plain.draw.parts[0].runs.size() == 1);
        CHECK(plain.draw.parts[0].runs[0].base);
    }
}

TEST_CASE("The resolution that fits the cap is computed from the refinement's size", "[TexturePreview][orcamcp]")
{
    // 4x the triangles at a 0.25 mm edge: twice the edge (0.5 mm), plus the 5 % margin, rounded up to 0.01
    CHECK(suggested_resolution_mm(0.25, 4000000, 1000000) == Catch::Approx(0.53));
    CHECK(suggested_resolution_mm(0.25, 1000000, 1000000) == Catch::Approx(0.27));
    CHECK(suggested_resolution_mm(0.25, 0, 1000000) == 0.);       // not known
    CHECK(suggested_resolution_mm(1.9, 4000000, 1000000) == 0.);   // beyond the bake's 2 mm: no resolution fits
}

TEST_CASE("A part the cap simplified says so, with the unsimplified count and a real argument that fits", "[TexturePreview][orcamcp]")
{
    Fixture f(true);
    f.volume->texture_displacement_options.v2_refine_mm = 0.25f;
    const TextureDisplacementBakeInput input = f.capture();
    TextureBakePlan plan;
    plan.results.resize(1);
    plan.results[0].mesh.its.vertices = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0)};
    plan.results[0].mesh.its.indices.assign(900000, stl_triangle_vertex_indices(0, 1, 2));
    plan.results[0].stats.triangles_refined = 2247936;
    plan.results[0].stats.budget_limited    = true;
    const PreviewDraw draw = make_preview_draw(std::move(plan), input, {0}, {});
    REQUIRE(draw.parts.size() == 1);
    const PreviewPart& part = draw.parts[0];
    CHECK(part.skipped.empty());
    CHECK(part.simplified_to_cap);
    CHECK(part.triangles_unsimplified == 2247936);
    CHECK(part.suggested_resolution_mm == Catch::Approx(suggested_resolution_mm(0.25, 2247936, max_part_triangles)));
    const std::string note = preview_part_note(part);
    CHECK(note.find("texture_preview: {resolution_mm: ") != std::string::npos);
    CHECK(note.find("2247936") != std::string::npos);
    CHECK(note.find("simplified to that") != std::string::npos);
    CHECK(note.find("coarser texture") == std::string::npos);   // the old advice named no argument

    // an asked budget under the cap is the user's: not the cap's doing
    Fixture asked(true);
    asked.volume->texture_displacement_options.v2_refine_mm = 0.25f;
    asked.volume->texture_displacement_options.v2_max_triangles_k = 300;
    TextureBakePlan plan2;
    plan2.results.resize(1);
    plan2.results[0].mesh.its.vertices = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0)};
    plan2.results[0].mesh.its.indices.assign(300000, stl_triangle_vertex_indices(0, 1, 2));
    plan2.results[0].stats.triangles_refined = 2247936;
    const PreviewDraw kept = make_preview_draw(std::move(plan2), asked.capture(), {0}, {}, true);
    CHECK_FALSE(kept.parts[0].simplified_to_cap);
    CHECK(preview_part_note(kept.parts[0]).empty());

    // still over the cap after the budget: unbaked, and the note names the arguments that help
    TextureBakePlan plan3;
    plan3.results.resize(1);
    plan3.results[0].mesh.its.vertices = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0)};
    plan3.results[0].mesh.its.indices.assign(max_part_triangles + 5, stl_triangle_vertex_indices(0, 1, 2));
    plan3.results[0].stats.triangles_refined = 3000000;
    const PreviewDraw over = make_preview_draw(std::move(plan3), input, {0}, {});
    CHECK(over.parts[0].skipped == "too_large");
    CHECK(preview_part_note(over.parts[0]).find("texture_preview: {resolution_mm: ") != std::string::npos);
}

TEST_CASE("A part over the triangle limit is not drawn and says how large it is", "[TexturePreview][orcamcp]")
{
    Fixture f(false);
    TextureDisplacementBakeInput input = f.capture();
    TextureBakePlan              plan;
    plan.results.resize(1);
    plan.results[0].mesh.its.vertices = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0)};
    plan.results[0].mesh.its.indices.assign(max_part_triangles + 1, stl_triangle_vertex_indices(0, 1, 2));
    const PreviewDraw draw = make_preview_draw(std::move(plan), input, {4}, {});
    REQUIRE(draw.parts.size() == 1);
    CHECK(draw.parts[0].skipped == "too_large");
    CHECK(draw.parts[0].triangles == max_part_triangles + 1);
    CHECK(draw.parts[0].mesh.indices.empty());
    CHECK(draw.parts[0].volume_id == 4);
    CHECK(draw.triangles() == 0);
}

TEST_CASE("The cache keeps one entry per object, four objects and two million triangles, least recently used first", "[TexturePreview][orcamcp]")
{
    Fixture f(false);
    const auto draw = draw_of(f);
    const PreviewKey key{1, 2}, other{3, 4};
    REQUIRE(draw->triangles() > 0);

    PreviewCache cache;
    CHECK(cache.find(ObjectID(1), key) == nullptr);
    REQUIRE(cache.put(ObjectID(1), key, draw, 1.5));
    const auto hit = cache.find(ObjectID(1), key);
    REQUIRE(hit != nullptr);
    CHECK_THAT(hit->seconds, Catch::Matchers::WithinAbs(1.5, 1e-9));
    CHECK(hit->draw == draw);
    CHECK(cache.find(ObjectID(2), key) == nullptr);   // another object, same key
    CHECK(cache.find(ObjectID(1), other) == nullptr);   // same object, another capture
    CHECK(cache.bytes() == draw->bytes());

    SECTION("a new preview of an object replaces its entry") {
        REQUIRE(cache.put(ObjectID(1), other, draw, 2.));
        CHECK(cache.size() == 1);
        CHECK(cache.find(ObjectID(1), key) == nullptr);
        CHECK(cache.find(ObjectID(1), other) != nullptr);
    }
    SECTION("the fifth object evicts the least recently used") {
        for (std::size_t id = 2; id <= PreviewCache::max_objects; ++id)
            REQUIRE(cache.put(ObjectID(id), key, draw, 0.));
        CHECK(cache.find(ObjectID(1), key) != nullptr);   // object 1 is now the most recently used
        REQUIRE(cache.put(ObjectID(99), key, draw, 0.));
        CHECK(cache.size() == PreviewCache::max_objects);
        CHECK(cache.find(ObjectID(1), key) != nullptr);
        CHECK(cache.find(ObjectID(2), key) == nullptr);
        CHECK(cache.find(ObjectID(99), key) != nullptr);
    }
    SECTION("entries of objects that are gone are dropped, and a project reset drops all") {
        REQUIRE(cache.put(ObjectID(5), key, draw, 0.));
        cache.prune([](const ObjectID& id) { return id == ObjectID(5); });
        CHECK(cache.size() == 1);
        CHECK(cache.find(ObjectID(5), key) != nullptr);
        texture_preview_cache().put(ObjectID(7), key, draw, 0.);
        REQUIRE(texture_preview_cache().size() >= 1);
        drop_texture_previews();   // what Plater::priv::reset calls
        CHECK(texture_preview_cache().size() == 0);
    }
    SECTION("an empty draw is not kept, and drops the object's old entry") {
        CHECK_FALSE(cache.put(ObjectID(1), key, nullptr, 0.));
        CHECK(cache.size() == 0);
    }
    SECTION("past the triangle cap the least recently used goes, and one over the cap alone is not kept") {
        const auto big = [](std::size_t triangles) {
            auto d = std::make_shared<PreviewDraw>();
            d->parts.emplace_back();
            d->parts[0].mesh.indices.resize(triangles);
            return std::shared_ptr<const PreviewDraw>(d);
        };
        REQUIRE(cache.put(ObjectID(2), key, big(PreviewCache::max_triangles / 2 + 1), 0.));
        CHECK(cache.size() == 2);
        REQUIRE(cache.put(ObjectID(3), key, big(PreviewCache::max_triangles / 2 + 1), 0.));
        CHECK(cache.triangles() <= PreviewCache::max_triangles);
        CHECK(cache.find(ObjectID(3), key) != nullptr);
        CHECK(cache.find(ObjectID(1), key) == nullptr);
        CHECK_FALSE(cache.put(ObjectID(4), key, big(PreviewCache::max_triangles + 1), 0.));
        CHECK(cache.find(ObjectID(4), key) == nullptr);
    }
}

TEST_CASE("The report says what was drawn, cached, timed out, not reached, left undrawn and why nothing was shown", "[TexturePreview][orcamcp]")
{
    Fixture f(false);
    PreviewObject done;
    done.object_id  = 2;
    done.draw       = draw_of(f);
    done.seconds    = 1.234;
    done.cached     = true;
    done.panel_open = true;

    PreviewReportInput input;
    input.plate_index = 1;
    input.wait_s      = 100.;
    input.objects.push_back(done);
    input.without_layers = {0, 3};
    input.outside_fit    = {6};
    PreviewSkip skip;
    skip.object_id  = 4;
    skip.reason     = "painted nowhere";
    skip.next_steps = nlohmann::json::array({{{"tool", "paint_object"}}});
    input.skipped.push_back(skip);
    PreviewCache cache;
    cache.put(ObjectID(2), PreviewKey{1, 1}, done.draw, 1.234);

    const nlohmann::json report = preview_report_json(input, cache);
    CHECK(report["requested"] == true);
    input.requested = nlohmann::json({{"resolution_mm", 0.5}, {"budget_k", nullptr}});
    CHECK(preview_report_json(input, cache)["requested"] == nlohmann::json({{"resolution_mm", 0.5}, {"budget_k", nullptr}}));
    CHECK(report["previewed"] == 1);
    CHECK(report["complete"] == true);
    const nlohmann::json& object = report["objects"][0];
    CHECK(object["cached"] == true);
    CHECK_THAT(object["seconds"].get<double>(), Catch::Matchers::WithinAbs(1.23, 1e-9));
    CHECK(object["panel_open"] == true);
    REQUIRE(object["parts"].size() == 1);
    CHECK(object["parts"][0]["triangles_preview"] == done.draw->parts[0].triangles);
    CHECK(report["skipped"][0]["next_steps"][0]["tool"] == "paint_object");
    CHECK(report["no_texture_layers"] == nlohmann::json::array({0, 3}));
    CHECK(report["outside_fit"] == nlohmann::json::array({6}));
    CHECK(report["cache"]["objects"] == 1);
    CHECK(report["cache"]["bytes"] == done.draw->bytes());
    CHECK(report["cache"]["max_triangles"] == 2000000);
    CHECK_FALSE(report.contains("note"));

    SECTION("an object whose preview ran out of the wait is timed out, one never started is not reached, and fitting is advised only unnarrowed") {
        PreviewObject slow;
        slow.object_id = 3;
        slow.attempted = true;
        PreviewObject later;
        later.object_id = 5;
        input.objects.push_back(slow);
        input.objects.push_back(later);
        const nlohmann::json out = preview_report_json(input, cache);
        CHECK(out["complete"] == false);
        CHECK(out["previewed"] == 1);   // the cached one is still drawn
        CHECK(out["objects"][0]["cached"] == true);
        CHECK(out["objects"][1]["timed_out"] == true);
        CHECK_FALSE(out["objects"][1].contains("not_reached"));
        CHECK(out["objects"][2]["not_reached"] == true);
        CHECK_FALSE(out["objects"][2].contains("timed_out"));
        const std::string note = out["objects"][1]["note"];
        CHECK(note.find("100 s") != std::string::npos);
        CHECK(note.find("fit a view to one object") != std::string::npos);
        input.narrowed = true;
        CHECK(std::string(preview_report_json(input, cache)["objects"][1]["note"]).find("fit a view") == std::string::npos);
    }
    SECTION("a preview kept but not drawn says so") {
        input.objects.front().undrawn = "over_render_total";
        const nlohmann::json out = preview_report_json(input, cache);
        CHECK(out["complete"] == false);
        CHECK(out["previewed"] == 0);
        CHECK(out["objects"][0]["undrawn"] == "over_render_total");
    }
    SECTION("a part too large to hold is listed with its count") {
        auto draw = std::make_shared<PreviewDraw>(*done.draw);
        draw->parts[0].skipped   = "too_large";
        draw->parts[0].triangles = 1234567;
        input.objects.front().draw = draw;
        const nlohmann::json out = preview_report_json(input, cache);
        CHECK(out["objects"][0]["parts"][0]["skipped"] == "too_large");
        CHECK(std::string(out["objects"][0]["parts"][0]["note"]).find("1234567") != std::string::npos);
    }
    SECTION("the classic preparation is reported") {
        auto draw = std::make_shared<PreviewDraw>(*done.draw);
        draw->preparation = "applied";
        input.objects.front().draw = draw;
        CHECK(preview_report_json(input, cache)["objects"][0]["preparation"] == "applied");
    }
    SECTION("nothing to preview says so") {
        PreviewReportInput none;
        none.plate_index = 3;
        const nlohmann::json empty = preview_report_json(none, cache);
        CHECK(empty["previewed"] == 0);
        CHECK(empty["complete"] == true);
        CHECK(std::string(empty["note"]).find("plate 3") != std::string::npos);
    }
}
