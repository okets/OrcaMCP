#include <catch2/catch_all.hpp>

#include <string>

#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderRequest.hpp"

// render_plate_view's request is read before anything is looked up or drawn (OrcaMCPRenderRequest.cpp):
// defaults, clamps, and every refusal, apart from the app.

using namespace Slic3r;
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
    CHECK(fit_of(nullptr).fit.kind == FitRequest::Kind::Plate);
    CHECK(fit_of("plate").fit_json == "plate");
}

TEST_CASE("A fit that is not one of its forms is refused, not read as the plate", "[RenderRequest][orcamcp]")
{
    auto fit_refusal = [](const json& fit) { return refusal_of({{"plate_index", 0}, {"views", json::array({{{"preset", "iso"}, {"fit", fit}}})}}); };
    CHECK(fit_refusal({{"object", 2}}).find("fit has no key \"object\"") != std::string::npos);
    CHECK(fit_refusal(json::object()).find("fit must be") != std::string::npos);
    CHECK(fit_refusal("object 2").find("fit must be") != std::string::npos);
    CHECK(fit_refusal(3).find("fit must be") != std::string::npos);
}

TEST_CASE("A layer plan reads its fit as a view does", "[RenderRequest][orcamcp]")
{
    CHECK(read_fit_request(json({{"object_id", 4}})).object_id == 4);
    CHECK(read_fit_request(json({{"object_index", 4}})).object_id == 4);
    CHECK(read_fit_request(json("plate")).kind == FitRequest::Kind::Plate);
    CHECK_THROWS(read_fit_request(json({{"object", 4}})));
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

// V1: close-ups, the exact camera and camera replay.

namespace {

json one_view(const json& view) { return {{"plate_index", 0}, {"views", json::array({view})}}; }

json square_camera(int size, bool perspective = true)
{
    json projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.02, -2.02, 0, 0, perspective ? -1 : 0, perspective ? 0 : 1};
    return {{"view_matrix", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -50, 0, 0, 0, 1}},
            {"projection_matrix", projection},
            {"viewport", {0, 0, size, size}}};
}

} // namespace

TEST_CASE("A fit takes a box or a point with a radius, in bed mm or plate-local", "[RenderRequest][orcamcp]")
{
    const RenderRequest box = read_ok(one_view({{"preset", "iso"}, {"fit", {{"box", {{"min", {1, 2, 3}}, {"max", {4, 5, 6}}}}}}}));
    CHECK(box.views[0].fit.kind == FitRequest::Kind::Box);
    CHECK(box.views[0].fit_json.contains("box"));
    const auto bb = region_fit_box(box.views[0].fit, Vec3d(100., 200., 0.));
    CHECK(bb.min == Vec3d(101., 202., 3.));
    CHECK(bb.max == Vec3d(104., 205., 6.));

    const RenderRequest point = read_ok(one_view({{"preset", "top"}, {"frame", "plate_local"}, {"fit", {{"point", {10, 20, 5}}, {"radius_mm", 4}}}}));
    CHECK(point.views[0].fit.kind == FitRequest::Kind::Point);
    CHECK(point.views[0].input_frame == "plate_local");   // a preset keeps the frame when its fit is a region
    const auto pb = region_fit_box(point.views[0].fit, Vec3d::Zero());
    CHECK(pb.min == Vec3d(6., 16., 1.));
    CHECK(pb.max == Vec3d(14., 24., 9.));

    CHECK(read_ok(one_view({{"preset", "top"}, {"frame", "plate_local"}})).views[0].input_frame == "bed_mm");
}

TEST_CASE("A fit region that is not one is refused", "[RenderRequest][orcamcp]")
{
    auto fit_refusal = [](const json& fit) { return refusal_of(one_view({{"preset", "iso"}, {"fit", fit}})); };
    CHECK(fit_refusal({{"point", {0, 0, 0}}}).find("radius_mm") != std::string::npos);
    CHECK(fit_refusal({{"point", {0, 0, 0}}, {"radius_mm", 0}}).find("above 0") != std::string::npos);
    CHECK(fit_refusal({{"radius_mm", 3}}).find("goes with fit.point") != std::string::npos);
    CHECK(fit_refusal({{"box", {{"min", {5, 0, 0}}, {"max", {1, 1, 1}}}}}).find("must not exceed") != std::string::npos);
    CHECK(fit_refusal({{"box", {{"min", {0, 0}}, {"max", {1, 1, 1}}}}}).find("fit.box.min") != std::string::npos);
    CHECK(fit_refusal({{"box", {{"min", {0, 0, 0}}}}}).find("{min") != std::string::npos);
    CHECK(fit_refusal({{"object_id", 1}, {"point", {0, 0, 0}}, {"radius_mm", 2}}).find("one of") != std::string::npos);
}

TEST_CASE("An exact camera needs a position and target and takes a lens", "[RenderRequest][orcamcp]")
{
    const json pose = {{"camera_position", {0, -60, 20}}, {"target", {0, 0, 10}}};
    json       perspective = pose;
    perspective["camera_mode"] = "exact";
    const ViewRequest p = read_ok(one_view(perspective)).views[0];
    CHECK(p.exact());
    CHECK_FALSE(p.orthographic);
    CHECK(exact_spec_of(p).fov_deg == 30.);   // the default

    perspective["fov_deg"] = 60;
    CHECK(exact_spec_of(read_ok(one_view(perspective)).views[0]).fov_deg == 60.);

    json ortho = pose;
    ortho["camera_mode"] = "exact";
    ortho["projection"]  = "orthographic";
    ortho["ortho_height_mm"] = 20;
    const ViewRequest o = read_ok(one_view(ortho)).views[0];
    CHECK(o.orthographic);
    CHECK_FALSE(exact_spec_of(o).perspective);
    CHECK(exact_spec_of(o).ortho_height_mm == 20.);

    // Fit mode may be orthographic too.
    json fit_ortho = pose;
    fit_ortho["projection"] = "orthographic";
    const ViewRequest f = read_ok(one_view(fit_ortho)).views[0];
    CHECK(f.orthographic);
    CHECK_FALSE(f.exact());
}

TEST_CASE("The exact camera's refusals say what to send instead", "[RenderRequest][orcamcp]")
{
    const json pose = {{"camera_position", {0, -60, 20}}, {"target", {0, 0, 10}}};
    auto with = [&pose](const json& extra) {
        json view = pose;
        view.update(extra);
        return refusal_of(one_view(view));
    };
    CHECK(refusal_of(one_view({{"camera_mode", "exact"}, {"preset", "iso"}})).find("frame a region with fit") != std::string::npos);
    CHECK(with({{"camera_mode", "exact"}, {"fit", {{"object_id", 0}}}}).find("frame a region with fit") != std::string::npos);
    CHECK(refusal_of(one_view({{"camera_mode", "exact"}})).find("needs camera_position and target") != std::string::npos);
    CHECK(with({{"fov_deg", 40}}).find("camera_mode exact") != std::string::npos);
    CHECK(with({{"ortho_height_mm", 20}, {"projection", "orthographic"}}).find("camera_mode exact") != std::string::npos);
    CHECK(with({{"camera_mode", "exact"}, {"fov_deg", 4}}).find("between 5 and 120") != std::string::npos);
    CHECK(with({{"camera_mode", "exact"}, {"fov_deg", 121}}).find("between 5 and 120") != std::string::npos);
    CHECK(with({{"camera_mode", "exact"}, {"projection", "orthographic"}}).find("ortho_height_mm") != std::string::npos);
    CHECK(with({{"camera_mode", "exact"}, {"projection", "orthographic"}, {"ortho_height_mm", 0}}).find("above 0") != std::string::npos);
    CHECK(with({{"camera_mode", "exact"}, {"projection", "orthographic"}, {"ortho_height_mm", 5}, {"fov_deg", 40}}).find("perspective") != std::string::npos);
    CHECK(with({{"camera_mode", "exact"}, {"ortho_height_mm", 5}}).find("projection orthographic") != std::string::npos);
    CHECK(with({{"camera_mode", "wide"}}).find("\"fit\" or \"exact\"") != std::string::npos);
    CHECK(with({{"projection", "fisheye"}}).find("perspective") != std::string::npos);
    CHECK(refusal_of(one_view({{"camera_position", {1, 2, 3}}, {"target", {1, 2, 3}}})).find("same point") != std::string::npos);
}

TEST_CASE("A camera a render returned is replayed at its own resolution", "[RenderRequest][orcamcp]")
{
    const RenderRequest r = read_ok(one_view({{"camera", square_camera(768)}}));
    REQUIRE(r.views[0].replay.has_value());
    CHECK(r.resolution == 768);
    CHECK(r.views[0].replay->viewport[2] == 768);
    CHECK(r.views[0].exact());
    CHECK_FALSE(r.views[0].camera_position.has_value());   // the camera carried none

    json with_pose = square_camera(512);
    with_pose["camera_position"] = {1, 2, 3};
    with_pose["target"] = {4, 5, 6};
    const ViewRequest v = read_ok(one_view({{"camera", with_pose}})).views[0];
    REQUIRE(v.camera_position.has_value());
    CHECK(*v.camera_position == Vec3d(1., 2., 3.));
    CHECK(*v.target == Vec3d(4., 5., 6.));

    CHECK(read_ok(one_view({{"camera", square_camera(512, false)}})).views[0].orthographic);
    CHECK_FALSE(read_ok(one_view({{"camera", square_camera(512)}})).views[0].orthographic);
}

TEST_CASE("A replay refuses another resolution, another key and a camera it cannot draw", "[RenderRequest][orcamcp]")
{
    json payload = one_view({{"camera", square_camera(512)}});
    payload["resolution"] = 1024;
    CHECK(refusal_of(payload).find("leave resolution out") != std::string::npos);
    payload["resolution"] = 512;
    CHECK(read_ok(payload).resolution == 512);

    CHECK(refusal_of(one_view({{"camera", square_camera(512)}, {"preset", "iso"}})).find("takes no other key") != std::string::npos);
    CHECK(refusal_of(one_view({{"camera", square_camera(512)}, {"fit", {{"object_id", 1}}}})).find("(got fit)") != std::string::npos);
    CHECK(refusal_of(one_view({{"camera", {{"view_matrix", json::array()}}}})).find("view_matrix, projection_matrix and viewport") != std::string::npos);

    json wide = square_camera(512);
    wide["viewport"] = {0, 0, 512, 256};
    CHECK(refusal_of(one_view({{"camera", wide}})).find("square") != std::string::npos);

    json views = {{"plate_index", 0}, {"views", json::array({{{"camera", square_camera(512)}}, {{"camera", square_camera(256)}}})}};
    CHECK(refusal_of(views).find("different resolutions") != std::string::npos);
}

TEST_CASE("An exact camera, written as the answer writes it, is read back as the same frame", "[RenderRequest][orcamcp][RenderMath]")
{
    const Vec3d position(30., -80., 45.), target(40., 10., 12.);
    const BoundingBoxf3 depth(Vec3d(0., 0., 0.), Vec3d(270., 270., 300.));
    const CameraFrame drawn = exact_camera_frame(position, target, ExactCameraSpec{true, 33., 0.}, 640, 640, depth);

    json camera = camera_frame_to_json(drawn);
    camera["camera_position"] = {position.x(), position.y(), position.z()};
    camera["target"] = {target.x(), target.y(), target.z()};
    camera["type"] = "perspective";
    camera["moved"] = false;
    camera["mm_per_pixel"] = 0.1;
    camera["fit"] = nullptr;

    const RenderRequest r = read_ok(one_view({{"camera", camera}}));
    REQUIRE(r.views[0].replay.has_value());
    CHECK(r.resolution == 640);
    CHECK((r.views[0].replay->view - drawn.view).norm() == 0.);
    CHECK((r.views[0].replay->projection - drawn.projection).norm() == 0.);
    CHECK(r.views[0].replay->viewport == drawn.viewport);
    CHECK(*r.views[0].camera_position == position);
}

TEST_CASE("A camera a render returned is a view as it stands, and as views[i].camera", "[RenderRequest][orcamcp]")
{
    // The whole camera object of an answer: the three replay keys and every informational one.
    json camera = square_camera(512);
    camera.update({{"type", "perspective"}, {"projection", "perspective"}, {"camera_mode", "fit"}, {"fov_deg", 30.0},
                   {"pixel_origin", "top_left"}, {"preset", nullptr}, {"fit", nullptr}, {"input_frame", "bed_mm"},
                   {"mm_per_pixel", 0.1}, {"moved", true}, {"requested_position", {1, 2, 3}}, {"replayed", false},
                   {"camera_position", {4, 5, 6}}, {"target", {0, 0, 0}}});
    const RenderRequest inline_view = read_ok(one_view(camera));
    REQUIRE(inline_view.views[0].replay.has_value());
    CHECK(*inline_view.views[0].camera_position == Vec3d(4., 5., 6.));
    const RenderRequest nested = read_ok(one_view({{"camera", camera}}));
    CHECK(nested.views[0].replay->view == inline_view.views[0].replay->view);

    // A key a camera does not carry is refused, never ignored.
    json extra = camera;
    extra["zoom"] = 2;
    CHECK(refusal_of(one_view(extra)).find("(got zoom)") != std::string::npos);
    // views[i].camera takes nothing beside it, null preset or not.
    CHECK(refusal_of(one_view({{"camera", camera}, {"preset", nullptr}, {"frame", "bed_mm"}})).find("takes no other key") != std::string::npos);
}

TEST_CASE("A null preset is no preset", "[RenderRequest][orcamcp]")
{
    const json pose = {{"preset", nullptr}, {"camera_position", {0, -60, 20}}, {"target", {0, 0, 10}}};
    CHECK_FALSE(read_ok(one_view(pose)).views[0].preset.has_value());
}

TEST_CASE("A camera is replayed only on the plate it was drawn of", "[RenderRequest][orcamcp]")
{
    json camera = square_camera(512);
    camera["plate_index"] = 2;
    json payload = one_view({{"camera", camera}});
    payload["plate_index"] = 0;
    const std::string error = refusal_of(payload);
    CHECK(error.find("drawn of plate 2, not plate 0") != std::string::npos);
    CHECK(error.find("plate_index 2") != std::string::npos);
    CHECK(refusal_of(one_view(camera)).find("drawn of plate 2") != std::string::npos);   // the inline form too

    payload["plate_index"] = 2;
    CHECK(read_ok(payload).views[0].replay.has_value());
    camera.erase("plate_index");   // a camera from before plates were named is taken on any
    payload["plate_index"] = 0;
    payload["views"]       = json::array({{{"camera", camera}}});
    CHECK(read_ok(payload).views[0].replay.has_value());
}

TEST_CASE("texture_preview is read as a flag and refused where it cannot be drawn", "[RenderRequest][orcamcp][TexturePreview]")
{
    CHECK_FALSE(read_ok({{"plate_index", 0}}).texture_preview);
    CHECK(read_ok({{"plate_index", 0}, {"texture_preview", true}}).texture_preview);
    CHECK_FALSE(read_ok({{"plate_index", 0}, {"texture_preview", false}, {"layer_view", "first_layer"}}).texture_preview);
    CHECK(refusal_of({{"plate_index", 0}, {"texture_preview", "yes"}}).find("true or false") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"texture_preview", true}, {"layer_view", "first_layer"}}).find("3D views") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"texture_preview", true}, {"paint", {"texture"}}}).find("separately") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"texture_preview", true}, {"section", {{"plane", "z"}, {"at_mm", 5}}}}).find("separately") != std::string::npos);
    // the other channels are checked against the previewed parts when the scene is read, not here
    CHECK(read_ok({{"plate_index", 0}, {"texture_preview", true}, {"paint", {"support"}}}).texture_preview);
}

TEST_CASE("compare_with is read as a view of its own and refused where it cannot work", "[RenderRequest][orcamcp][RenderCompare]")
{
    const std::string path = "/tmp/orcamcp_render_1_2_3_0.png";
    const RenderRequest r  = read_ok({{"plate_index", 0}, {"views", json::array({{{"compare_with", path}}})}});
    REQUIRE(r.views.size() == 1);
    REQUIRE(r.views[0].compare_with.has_value());
    CHECK(*r.views[0].compare_with == path);
    CHECK_FALSE(r.views[0].replay.has_value());   // the camera is the record's, found by the caller
    CHECK_FALSE(r.resolution_given);
    CHECK(read_ok({{"plate_index", 0}, {"resolution", 256}, {"views", json::array({{{"compare_with", path}}})}}).resolution_given);

    // no camera of its own, and no contact sheet
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({{{"compare_with", path}, {"preset", "iso"}}})}}).find("takes no preset") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({{{"compare_with", path}, {"fit", "plate"}}})}}).find("takes no fit") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({{{"compare_with", path}}, {{"preset", "top"}}})}}).find("one view") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({{{"compare_with", 3}}})}}).find("file_path") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"views", json::array({{{"compare_with", ""}}})}}).find("file_path") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"layer_view", "first_layer"}, {"views", json::array({{{"compare_with", path}}})}}).find("3D views") != std::string::npos);
}

TEST_CASE("color_by relief and detail take their own scale and nothing else's", "[RenderRequest][orcamcp][Relief]")
{
    CHECK(read_ok({{"plate_index", 0}, {"color_by", "relief"}}).color_by == ColorBy::Relief);
    CHECK(read_ok({{"plate_index", 0}, {"color_by", "detail"}}).color_by == ColorBy::Detail);
    CHECK(read_ok({{"plate_index", 0}, {"color_by", "triangle_size"}}).color_by == ColorBy::TriangleSize);
    CHECK(refusal_of({{"plate_index", 0}, {"color_by", "triangle_size"}, {"detail_range_deg", 12}}).find("add color_by detail") != std::string::npos);
    CHECK_FALSE(read_ok({{"plate_index", 0}, {"color_by", "relief"}}).relief_range_mm.has_value());
    const RenderRequest r = read_ok({{"plate_index", 0}, {"color_by", "relief"}, {"relief_range_mm", 0.25}});
    REQUIRE(r.relief_range_mm.has_value());
    CHECK_THAT(*r.relief_range_mm, Catch::Matchers::WithinAbs(0.25, 1e-12));
    REQUIRE(read_ok({{"plate_index", 0}, {"color_by", "detail"}, {"detail_range_deg", 12}}).detail_range_deg.has_value());

    CHECK(refusal_of({{"plate_index", 0}, {"relief_range_mm", 0.25}}).find("add color_by relief") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"color_by", "detail"}, {"relief_range_mm", 0.25}}).find("add color_by relief") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"color_by", "relief"}, {"detail_range_deg", 5}}).find("add color_by detail") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"color_by", "relief"}, {"relief_range_mm", 0}}).find("above 0") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"color_by", "relief"}, {"relief_range_mm", "wide"}}).find("above 0") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"color_by", "sepia"}}).find("relief") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"layer_view", "first_layer"}, {"color_by", "relief"}}).find("3D views") != std::string::npos);
    CHECK(refusal_of({{"plate_index", 0}, {"layer_view", "first_layer"}, {"relief_range_mm", 1}}).find("color_by relief") != std::string::npos);
}
