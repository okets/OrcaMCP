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

#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
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

std::shared_ptr<const TextureBakePlan> plan_of(const TextureDisplacementBakeInput& input)
{
    auto plan = std::make_shared<TextureBakePlan>(plan_texture_bake(preview_input(input), [](int) { return true; }));
    return plan;
}

PrintableColor red()
{
    PrintableColor c;
    c.rgb = Vec3f(1.f, 0.f, 0.f);
    return c;
}

} // namespace

TEST_CASE("The preview's input zeroes the triangle budget of v2 parts only and touches nothing else", "[TexturePreview][orcamcp]")
{
    Fixture v2(true);
    v2.volume->texture_displacement_options.v2_max_triangles_k = 40;
    const TextureDisplacementBakeInput before = v2.capture();
    TextureDisplacementBakeInput       after  = preview_input(before);
    REQUIRE(after.volumes.size() == 1);
    CHECK(before.volumes[0].options.v2_max_triangles_k == 40);   // the original is kept
    CHECK(after.volumes[0].options.v2_max_triangles_k == 0);
    after.volumes[0].options.v2_max_triangles_k = 40;
    CHECK(same_bake_volume(after.volumes[0], before.volumes[0], false));   // nothing else changed
    CHECK(after.volumes[0].base_mesh.indices == before.volumes[0].base_mesh.indices);

    Fixture classic(false);
    classic.volume->texture_displacement_options.v2_max_triangles_k = 40;
    CHECK(preview_input(classic.capture()).volumes[0].options.v2_max_triangles_k == 40);
}

TEST_CASE("Two captures are the same preview until something the preview draws changes", "[TexturePreview][orcamcp]")
{
    Fixture f(true);
    const TextureDisplacementBakeInput first = f.capture();
    CHECK(same_preview_input(first, f.capture()));

    SECTION("the budget is not part of what is drawn") {
        f.volume->texture_displacement_options.v2_max_triangles_k = 77;
        CHECK(same_preview_input(first, f.capture()));
    }
    SECTION("a mask timestamp") {
        f.volume->texture_displacement_facet(0).reset();
        paint(*f.volume, 0, int(f.volume->mesh().its.indices.size()) - 2);
        CHECK_FALSE(same_preview_input(first, f.capture()));
    }
    SECTION("a layer setting") {
        f.volume->texture_displacement_layers.front().depth_mm += 0.5f;
        CHECK_FALSE(same_preview_input(first, f.capture()));
    }
    SECTION("an option the bake reads") {
        f.volume->texture_displacement_options.v2_refine_mm = 0.7f;
        CHECK_FALSE(same_preview_input(first, f.capture()));
    }
    SECTION("the mesh") {
        indexed_triangle_set nudged = f.volume->mesh().its;
        nudged.vertices.front().z() += 0.5f;
        f.volume->set_mesh(TriangleMesh(nudged));
        CHECK_FALSE(same_preview_input(first, f.capture()));
    }
    SECTION("the part's placement") {
        f.volume->set_offset(Vec3d(0., 0., 3.));
        CHECK_FALSE(same_preview_input(first, f.capture()));
    }
    SECTION("the palette") {
        TextureDisplacementBakeInput coloured = f.capture();
        coloured.color.palette = {red()};
        CHECK_FALSE(same_preview_input(first, coloured));
        TextureDisplacementBakeInput other = coloured;
        CHECK(same_preview_input(coloured, other));
        other.color.palette.front().rgb = Vec3f(0.f, 1.f, 0.f);
        CHECK_FALSE(same_preview_input(coloured, other));
        other = coloured;
        other.color.despeckle_passes += 1;
        CHECK_FALSE(same_preview_input(coloured, other));
    }
    SECTION("the parts painted") {
        TextureDisplacementBakeInput none = first;
        none.volumes.clear();
        CHECK_FALSE(same_preview_input(first, none));
    }
}

TEST_CASE("The preview is the bake without simplification: it moves vertices and keeps more triangles than a budgeted bake", "[TexturePreview][orcamcp]")
{
    Fixture f(true, 16);
    f.volume->texture_displacement_options.v2_max_triangles_k = 1;   // 1000 triangles at most for the bake
    const TextureDisplacementBakeInput input = f.capture();
    REQUIRE(input.volumes.size() == 1);

    const TextureBakePlan budgeted = plan_texture_bake(input, [](int) { return true; });
    const TextureBakePlan preview  = plan_texture_bake(preview_input(input), [](int) { return true; });
    REQUIRE_FALSE(budgeted.canceled);
    REQUIRE_FALSE(preview.canceled);
    REQUIRE(preview.results.size() == 1);
    CHECK(texture_bake_moved(preview_input(input), preview, 0));
    float highest = 0.f;
    for (const Vec3f& v : preview.results.front().mesh.its.vertices)
        highest = std::max(highest, std::abs(v.z()));
    CHECK(highest > 0.1f);   // the ramp really relieved the sheet
    CHECK(preview_triangles(preview) > preview_triangles(budgeted));
    CHECK(preview_triangles(preview) > input.volumes[0].base_mesh.indices.size());   // refined, not the base mesh
}

TEST_CASE("A preview whose progress hook says no is cancelled", "[TexturePreview][orcamcp]")
{
    Fixture f(true);
    const TextureBakePlan stopped = plan_texture_bake(preview_input(f.capture()), [](int) { return false; });
    CHECK(stopped.canceled);
}

TEST_CASE("The cache keeps one entry per object, four objects and two million triangles, least recently used first", "[TexturePreview][orcamcp]")
{
    Fixture f(false);   // the classic pipeline: quick, and the plan is a real one
    const TextureDisplacementBakeInput input = f.capture();
    const auto                         plan  = plan_of(input);
    REQUIRE(plan->results.size() == 1);
    const std::size_t each = preview_triangles(*plan);
    REQUIRE(each > 0);

    PreviewCache cache;
    CHECK(cache.find(ObjectID(1), input) == nullptr);
    REQUIRE(cache.put(ObjectID(1), input, plan, 1.5));
    const auto hit = cache.find(ObjectID(1), input);
    REQUIRE(hit != nullptr);
    CHECK(hit->seconds == 1.5);
    CHECK(hit->plan == plan);
    CHECK(cache.find(ObjectID(2), input) == nullptr);   // another object, same capture

    SECTION("a new capture of an object replaces its entry") {
        Fixture g(false);
        g.volume->texture_displacement_layers.front().depth_mm = 3.f;
        const TextureDisplacementBakeInput changed = g.capture();
        REQUIRE(cache.put(ObjectID(1), changed, plan, 2.));
        CHECK(cache.size() == 1);
        CHECK(cache.find(ObjectID(1), input) == nullptr);
        CHECK(cache.find(ObjectID(1), changed) != nullptr);
    }
    SECTION("the fifth object evicts the least recently used") {
        for (std::size_t id = 2; id <= PreviewCache::max_objects; ++id)
            REQUIRE(cache.put(ObjectID(id), input, plan, 0.));
        CHECK(cache.size() == PreviewCache::max_objects);
        CHECK(cache.find(ObjectID(1), input) != nullptr);   // object 1 is now the most recently used
        REQUIRE(cache.put(ObjectID(99), input, plan, 0.));
        CHECK(cache.size() == PreviewCache::max_objects);
        CHECK(cache.find(ObjectID(1), input) != nullptr);
        CHECK(cache.find(ObjectID(2), input) == nullptr);   // the oldest unused went
        CHECK(cache.find(ObjectID(99), input) != nullptr);
    }
    SECTION("entries of objects that are gone are dropped") {
        REQUIRE(cache.put(ObjectID(5), input, plan, 0.));
        cache.prune([](const ObjectID& id) { return id == ObjectID(5); });
        CHECK(cache.size() == 1);
        CHECK(cache.find(ObjectID(1), input) == nullptr);
        CHECK(cache.find(ObjectID(5), input) != nullptr);
    }
    SECTION("a cancelled or empty plan is not kept, and drops the object's old entry") {
        auto cancelled      = std::make_shared<TextureBakePlan>(*plan);
        cancelled->canceled = true;
        CHECK_FALSE(cache.put(ObjectID(1), input, cancelled, 0.));
        CHECK_FALSE(cache.put(ObjectID(1), input, nullptr, 0.));
        CHECK(cache.size() == 0);
    }
    SECTION("past the triangle cap the least recently used goes, and one over the cap alone is not kept") {
        auto big = std::make_shared<TextureBakePlan>(*plan);
        big->results.front().mesh.its.indices.resize(PreviewCache::max_triangles / 2 + 1);   // two of these are over the cap
        REQUIRE(cache.put(ObjectID(2), input, big, 0.));
        CHECK(cache.size() == 2);
        REQUIRE(cache.put(ObjectID(3), input, big, 0.));
        CHECK(cache.triangles() <= PreviewCache::max_triangles);
        CHECK(cache.find(ObjectID(3), input) != nullptr);
        CHECK(cache.find(ObjectID(1), input) == nullptr);
        auto huge = std::make_shared<TextureBakePlan>(*plan);
        huge->results.front().mesh.its.indices.resize(PreviewCache::max_triangles + 1);
        CHECK_FALSE(cache.put(ObjectID(4), input, huge, 0.));
        CHECK(cache.find(ObjectID(4), input) == nullptr);
    }
}

TEST_CASE("The report says what was drawn, what was cached, what timed out and why nothing was shown", "[TexturePreview][orcamcp]")
{
    Fixture f(false);
    PreviewObject done;
    done.object_id  = 2;
    done.captured   = f.capture();
    done.volume_ids = {0};
    done.plan       = plan_of(done.captured);
    done.seconds    = 1.234;
    done.cached     = true;
    done.panel_open = true;

    PreviewReportInput input;
    input.plate_index = 1;
    input.wait_s      = 100.;
    input.objects.push_back(done);
    input.without_layers = {0, 3};
    PreviewSkip skip;
    skip.object_id = 4;
    skip.reason    = "painted nowhere";
    skip.next_steps = nlohmann::json::array({{{"tool", "paint_object"}}});
    input.skipped.push_back(skip);

    const nlohmann::json report = preview_report_json(input, 1, 1234);
    CHECK(report["requested"] == true);
    CHECK(report["previewed"] == 1);
    CHECK(report["complete"] == true);
    REQUIRE(report["objects"].size() == 1);
    const nlohmann::json& object = report["objects"][0];
    CHECK(object["object_id"] == 2);
    CHECK(object["cached"] == true);
    CHECK(object["seconds"] == 1.23);
    CHECK(object["panel_open"] == true);
    REQUIRE(object["parts"].size() == 1);
    CHECK(object["parts"][0]["volume_id"] == 0);
    CHECK(object["parts"][0]["triangles_base"] == done.captured.volumes[0].base_mesh.indices.size());
    CHECK(object["parts"][0]["triangles_preview"] == preview_triangles(*done.plan));
    CHECK(report["skipped"][0]["next_steps"][0]["tool"] == "paint_object");
    CHECK(report["no_texture_layers"] == nlohmann::json::array({0, 3}));
    CHECK(report["cache"]["triangles"] == 1234);
    CHECK(report["cache"]["max_objects"] == 4);
    CHECK(report["cache"]["max_triangles"] == 2000000);
    CHECK_FALSE(report.contains("note"));

    SECTION("an object whose plan did not finish is timed out and the report is incomplete") {
        input.objects.front().plan.reset();
        const nlohmann::json timed = preview_report_json(input, 0, 0);
        CHECK(timed["complete"] == false);
        CHECK(timed["previewed"] == 0);
        CHECK(timed["objects"][0]["timed_out"] == true);
        const std::string note = timed["objects"][0]["note"];
        CHECK(note.find("object 2") != std::string::npos);
        CHECK(note.find("100 s") != std::string::npos);
        CHECK(note.find("ORCAMCP_TIMEOUT") != std::string::npos);
        CHECK_FALSE(timed.contains("note"));   // the object's note says it
    }
    SECTION("nothing to preview says so") {
        PreviewReportInput none;
        none.plate_index = 3;
        const nlohmann::json empty = preview_report_json(none, 0, 0);
        CHECK(empty["previewed"] == 0);
        CHECK(empty["complete"] == true);
        CHECK(std::string(empty["note"]).find("plate 3") != std::string::npos);
    }
}
