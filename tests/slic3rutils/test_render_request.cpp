#include <catch2/catch_all.hpp>

#include <string>

#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderRequest.hpp"

// render_plate_view's request is read before anything is looked up or drawn (OrcaMCPRenderRequest.cpp):
// defaults, clamps, and every refusal, apart from the app.

using namespace Slic3r::GUI::OrcaMCP;
using json = nlohmann::json;

namespace {

std::string refusal_of(const json& payload)
{
    std::string error;
    const auto  request = read_render_request(payload, error);
    REQUIRE_FALSE(request.has_value());
    return error;
}

RenderRequest read_ok(const json& payload)
{
    std::string error;
    auto        request = read_render_request(payload, error);
    REQUIRE(request.has_value());
    return *request;
}

} // namespace

TEST_CASE("A render request with only a plate is the default contact sheet", "[RenderRequest][orcamcp]")
{
    const RenderRequest r = read_ok({{"plate_index", 0}});
    CHECK(r.plate_index == 0);
    CHECK(r.resolution == 512);
    CHECK_FALSE(r.save_to_file);
    CHECK_FALSE(r.use_png);   // inline base64 defaults to jpeg
    CHECK(r.contact_sheet);
    CHECK(r.layer_view.is_null());
    REQUIRE(r.views.size() == 3);
    CHECK(r.views[0].preset_name == "iso");
    CHECK(r.views[1].preset_name == "top");
    CHECK(r.views[2].preset_name == "front");
    CHECK(r.views[0].fit.kind == FitRequest::Kind::Plate);
    CHECK(r.views[0].fit_json == "plate");
}

TEST_CASE("A render to a file is a png unless jpeg is asked for", "[RenderRequest][orcamcp]")
{
    CHECK(read_ok({{"plate_index", 1}, {"save_to_file", true}}).use_png);
    CHECK_FALSE(read_ok({{"plate_index", 1}, {"save_to_file", true}, {"image_format", "jpeg"}}).use_png);
    CHECK(read_ok({{"plate_index", 1}, {"image_format", "png"}}).use_png);
    CHECK(refusal_of({{"plate_index", 1}, {"image_format", "gif"}}) == "image_format must be \"png\" or \"jpeg\"");
}

TEST_CASE("The resolution is held between 32 and 2048", "[RenderRequest][orcamcp]")
{
    CHECK(read_ok({{"plate_index", 0}, {"resolution", 1}}).resolution == 32);
    CHECK(read_ok({{"plate_index", 0}, {"resolution", 100000}}).resolution == 2048);
    CHECK(read_ok({{"plate_index", 0}, {"resolution", 777}}).resolution == 777);
}

TEST_CASE("A request without a plate is refused", "[RenderRequest][orcamcp]")
{
    CHECK(refusal_of(json::object()) == "Missing required parameter plate_index");
    CHECK(refusal_of(json()) == "Missing required parameter plate_index");
}

TEST_CASE("A layer view takes the place of the views", "[RenderRequest][orcamcp]")
{
    const RenderRequest r = read_ok({{"plate_index", 0}, {"layer_view", "first_layer"}, {"views", json::array({json::object()})}});
    CHECK(r.layer_view == "first_layer");
    CHECK(r.views.empty());
    // "" was always read as no layer view.
    CHECK(read_ok({{"plate_index", 0}, {"layer_view", ""}}).layer_view.is_null());
}

TEST_CASE("Every view refusal keeps its message", "[RenderRequest][orcamcp]")
{
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array()}}) == "views must not be empty");
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({5})}}) == "each view must be an object");
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({{{"preset", "sideways"}}})}}) ==
          "unknown preset 'sideways'; use iso, top, front, back, left, right or low");
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({{{"target", {0, 0, 0}}}})}}) ==
          "each view needs a preset, or camera_position and target");
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({{{"camera_position", {1, 2}}, {"target", {0, 0, 0}}}})}}) ==
          "camera_position must be [x, y, z]");
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({{{"camera_position", {1, 2, 3}}, {"target", {0, 0, 0}}, {"frame", "world"}}})}}) ==
          "frame must be \"bed_mm\" or \"plate_local\"");
}

TEST_CASE("An explicit camera keeps its frame for the caller to convert", "[RenderRequest][orcamcp]")
{
    const RenderRequest r = read_ok({{"plate_index", 0}, {"views", json::array({{{"camera_position", {1, 2, 3}}, {"target", {4, 5, 6}}, {"frame", "plate_local"}}})}});
    REQUIRE(r.views.size() == 1);
    const ViewRequest& v = r.views[0];
    CHECK_FALSE(r.contact_sheet);
    CHECK_FALSE(v.preset.has_value());
    CHECK(v.input_frame == "plate_local");
    REQUIRE(v.camera_position.has_value());
    CHECK(v.camera_position->z() == 3.);   // as given: no plate offset added here
    CHECK(v.target->x() == 4.);
    // An object form of a point is read too.
    const RenderRequest o = read_ok({{"plate_index", 0}, {"views", json::array({{{"camera_position", {{"x", 1}, {"y", 2}, {"z", 3}}}, {"target", {0, 0, 0}}}})}});
    CHECK(o.views[0].camera_position->y() == 2.);
    CHECK(o.views[0].input_frame == "bed_mm");
}

TEST_CASE("A preset is always in bed mm", "[RenderRequest][orcamcp]")
{
    const RenderRequest r = read_ok({{"plate_index", 0}, {"views", json::array({{{"preset", "low"}, {"frame", "plate_local"}}})}});
    CHECK(r.views[0].preset.has_value());
    CHECK(r.views[0].input_frame == "bed_mm");
}

TEST_CASE("fit takes object_id and object_index as the same number", "[RenderRequest][orcamcp]")
{
    const auto fit_of = [](const json& fit) { return read_ok({{"plate_index", 0}, {"views", json::array({{{"preset", "iso"}, {"fit", fit}}})}}).views[0]; };

    const ViewRequest by_id = fit_of({{"object_id", 2}});
    CHECK(by_id.fit.kind == FitRequest::Kind::Object);
    CHECK(by_id.fit.object_id == 2);
    CHECK(by_id.fit_json == json({{"object_id", 2}}));

    const ViewRequest by_index = fit_of({{"object_index", 2}});
    CHECK(by_index.fit.kind == FitRequest::Kind::Object);
    CHECK(by_index.fit.object_id == by_id.fit.object_id);

    CHECK(fit_of({{"object_id", 3}, {"object_index", 3}}).fit.object_id == 3);

    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({{{"preset", "iso"}, {"fit", {{"object_id", 1}, {"object_index", 2}}}}})}}) ==
          "fit.object_id and fit.object_index are the same number; send object_id only");

    // Anything else frames the plate, as before.
    CHECK(fit_of("plate").fit.kind == FitRequest::Kind::Plate);
    CHECK(fit_of(json::object()).fit.kind == FitRequest::Kind::Plate);
    CHECK(fit_of("plate").fit_json == "plate");
}

TEST_CASE("The default shader settings change nothing", "[RenderRequest][orcamcp]")
{
    ShaderSettings s;
    CHECK(s.is_default());
    s.clipping_plane = {1., 0., 0., 0.};
    CHECK_FALSE(s.is_default());
    s = ShaderSettings{};
    s.front_light = false;
    CHECK_FALSE(s.is_default());
}
