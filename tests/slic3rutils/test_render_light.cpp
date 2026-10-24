#include <catch2/catch_all.hpp>

#include <cmath>
#include <string>

#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderLight.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderMath.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderRequest.hpp"

// render_plate_view's raking light: the request, the direction in bed axes and in eye space, and the shader settings.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using json = nlohmann::json;

namespace {

std::string refusal_of(const json& light)
{
    try {
        read_light(light);
    } catch (const std::exception& e) {
        return e.what();
    }
    FAIL("the light was accepted: " << light.dump());
    return "";
}

constexpr double k_pi = 3.141592653589793;

} // namespace

TEST_CASE("azimuth and elevation make the direction the light comes from", "[RenderLight]")
{
    const LightRequest from_x = *read_light({{"azimuth_deg", 0}, {"elevation_deg", 90}});
    CHECK_THAT(from_x.toward_light.z(), Catch::Matchers::WithinAbs(1., 1e-12));

    const LightRequest low = *read_light({{"azimuth_deg", 0}, {"elevation_deg", 30}});
    CHECK_THAT(low.toward_light.x(), Catch::Matchers::WithinAbs(std::cos(30. * k_pi / 180.), 1e-12));
    CHECK_THAT(low.toward_light.y(), Catch::Matchers::WithinAbs(0., 1e-12));
    CHECK_THAT(low.toward_light.z(), Catch::Matchers::WithinAbs(0.5, 1e-12));

    // Azimuth counts counter-clockwise seen from above: 90 is from +Y, 270 from -Y.
    CHECK_THAT(read_light({{"azimuth_deg", 90}, {"elevation_deg", 45}})->toward_light.y(), Catch::Matchers::WithinAbs(std::sqrt(0.5), 1e-12));
    CHECK(read_light({{"azimuth_deg", 270}, {"elevation_deg", 45}})->toward_light.y() < 0.);
}

TEST_CASE("a light round-trips between direction and angles", "[RenderLight]")
{
    const LightRequest angles = *read_light({{"azimuth_deg", 30}, {"elevation_deg", 12}});
    const json         answer = light_json(angles, Eigen::Matrix4d::Identity());

    // The answer's direction is the way the light travels: from the light to the scene.
    CHECK_THAT(answer["direction"][2].get<double>(), Catch::Matchers::WithinAbs(-std::sin(12. * k_pi / 180.), 1e-12));
    const LightRequest by_direction = *read_light({{"direction", answer["direction"]}});
    CHECK_THAT(by_direction.azimuth_deg, Catch::Matchers::WithinAbs(30., 1e-9));
    CHECK_THAT(by_direction.elevation_deg, Catch::Matchers::WithinAbs(12., 1e-9));
    CHECK((by_direction.toward_light - angles.toward_light).norm() < 1e-12);

    // The whole answer is accepted back, with what a fallback adds.
    json back = answer;
    back["applied"] = false;
    back["warning"] = "not applied";
    const LightRequest again = *read_light(back);
    CHECK((again.toward_light - angles.toward_light).norm() < 1e-9);

    // A direction and angles that disagree are two different lights.
    CHECK(refusal_of({{"direction", answer["direction"]}, {"azimuth_deg", 200}, {"elevation_deg", 12}}).find("different lights") != std::string::npos);
}

TEST_CASE("the default light is no light", "[RenderLight]")
{
    CHECK_FALSE(read_light("default").has_value());
    CHECK_FALSE(read_light(nullptr).has_value());
}

TEST_CASE("a light that is not one is refused", "[RenderLight]")
{
    CHECK(refusal_of({{"azimuth_deg", 30}, {"elevation_deg", 0}}).find("between 1 and 90") != std::string::npos);
    CHECK(refusal_of({{"azimuth_deg", 30}, {"elevation_deg", 91}}).find("between 1 and 90") != std::string::npos);
    CHECK(refusal_of({{"azimuth_deg", 30}}).find("elevation_deg") != std::string::npos);
    CHECK(refusal_of({{"direction", {0, 0, 0}}}).find("zero vector") != std::string::npos);
    CHECK(refusal_of({{"direction", {1, 0}}}).find("[x, y, z]") != std::string::npos);
    CHECK(refusal_of({{"brightness", 2}}).find("no key \"brightness\"") != std::string::npos);
    CHECK(refusal_of("bright").find("light must be") != std::string::npos);
    CHECK(refusal_of(json::object()).find("light must be") != std::string::npos);
}

TEST_CASE("a light from the camera's side lights a facet that faces the camera", "[RenderLight]")
{
    // The camera in front of the part looks along +Y; a facet facing it has the normal (0, -1, 0).
    const Vec3d position(0., -100., 0.), target(0., 0., 0.);
    const CameraFrame frame = exact_camera_frame(position, target, ExactCameraSpec{true, 30., 0.}, 256, 256,
                                                 BoundingBoxf3(Vec3d(-50., -50., -50.), Vec3d(50., 50., 50.)));
    const Vec3d normal_eye = frame.view.block<3, 3>(0, 0) * Vec3d(0., -1., 0.);

    const LightRequest camera_side = *read_light({{"azimuth_deg", 270}, {"elevation_deg", 30}});   // from -Y, above
    const Vec3d        lit         = light_dir_eye(camera_side.toward_light, frame.view);
    CHECK(lit.dot(normal_eye) > 0.8);
    CHECK_THAT(lit.norm(), Catch::Matchers::WithinAbs(1., 1e-12));

    const LightRequest behind = *read_light({{"azimuth_deg", 90}, {"elevation_deg", 30}});         // from +Y: behind the part
    CHECK(light_dir_eye(behind.toward_light, frame.view).dot(normal_eye) < 0.);
}

TEST_CASE("a raking light is one light with little fill and no highlight", "[RenderLight]")
{
    const LightRequest light = *read_light({{"azimuth_deg", 30}, {"elevation_deg", 12}});
    const ShaderSettings s   = raking_light_settings(light, Eigen::Matrix4d::Identity());
    CHECK(s.use_light_dir);
    CHECK_FALSE(s.front_light);
    CHECK(s.ambient < ShaderSettings{}.ambient);
    CHECK(s.specular == 0.f);
    CHECK_FALSE(s.is_default());
    CHECK((s.light_dir_eye - light.toward_light).norm() < 1e-12);   // identity view: eye axes are bed axes
}

TEST_CASE("the light is part of the request, and a layer plan refuses it", "[RenderLight][RenderRequest]")
{
    std::string error;
    auto r = read_render_request({{"plate_index", 0}, {"light", {{"azimuth_deg", 30}, {"elevation_deg", 12}}}}, error);
    REQUIRE(r.has_value());
    REQUIRE(r->light.has_value());
    CHECK_THAT(r->light->elevation_deg, Catch::Matchers::WithinAbs(12., 1e-12));

    r = read_render_request({{"plate_index", 0}}, error);
    REQUIRE(r.has_value());
    CHECK_FALSE(r->light.has_value());

    r = read_render_request({{"plate_index", 0}, {"light", "default"}, {"layer_view", "first_layer"}}, error);
    CHECK(r.has_value());   // the default is no light, so nothing to ignore

    r = read_render_request({{"plate_index", 0}, {"light", {{"azimuth_deg", 30}, {"elevation_deg", 12}}}, {"layer_view", "first_layer"}}, error);
    CHECK_FALSE(r.has_value());
    CHECK(error.find("3D views") != std::string::npos);

    r = read_render_request({{"plate_index", 0}, {"light", {{"elevation_deg", 12}}}}, error);
    CHECK_FALSE(r.has_value());
}

namespace {

CameraFrame front_camera()
{
    // In front of the part (-Y), looking along +Y: the image's right is +X, up is +Z.
    return exact_camera_frame(Vec3d(0., -100., 0.), Vec3d(0., 0., 0.), ExactCameraSpec{true, 30., 0.}, 256, 256,
                              BoundingBoxf3(Vec3d(-50., -50., -50.), Vec3d(50., 50., 50.)));
}

} // namespace

TEST_CASE("a camera-relative light comes from the side of the image it names", "[RenderLight]")
{
    const CameraFrame frame = front_camera();
    const double      el    = 15. * k_pi / 180.;
    const LightRequest left = *read_light({{"from", "left"}, {"elevation_deg", 15}});
    const Vec3d        toward = light_toward_world(left, frame.view);
    CHECK_THAT(toward.x(), Catch::Matchers::WithinAbs(-std::cos(el), 1e-9));   // the image's left is -X here
    CHECK_THAT(toward.y(), Catch::Matchers::WithinAbs(-std::sin(el), 1e-9));   // and it is raised toward the camera (-Y)
    CHECK_THAT(toward.z(), Catch::Matchers::WithinAbs(0., 1e-9));
    CHECK(light_toward_world(*read_light({{"from", "right"}, {"elevation_deg", 15}}), frame.view).x() > 0.9);
    CHECK(light_toward_world(*read_light({{"from", "top"}, {"elevation_deg", 15}}), frame.view).z() > 0.9);
    CHECK(light_toward_world(*read_light({{"from", "bottom"}, {"elevation_deg", 15}}), frame.view).z() < -0.9);
    CHECK((light_toward_world(*read_light({{"from", "front"}}), frame.view) - Vec3d(0., -1., 0.)).norm() < 1e-9);

    // It grazes the surface the camera sees: a facet facing the camera is lit at sin(elevation), the shader's n.l.
    const ShaderSettings s = raking_light_settings(left, frame.view);
    CHECK_THAT(s.light_dir_eye.z(), Catch::Matchers::WithinAbs(std::sin(el), 1e-9));
    CHECK_THAT(s.light_dir_eye.norm(), Catch::Matchers::WithinAbs(1., 1e-9));

    // The same request turns with another camera: from above, the image's left is -X still, the toward-camera is +Z.
    const CameraFrame top = exact_camera_frame(Vec3d(0., 0., 100.), Vec3d(0., 0., 0.), ExactCameraSpec{true, 30., 0.}, 256, 256,
                                               BoundingBoxf3(Vec3d(-50., -50., -50.), Vec3d(50., 50., 50.)));
    CHECK(light_toward_world(left, top.view).z() > 0.2);
}

TEST_CASE("a camera-relative light's answer names the bed-axes light and is accepted back", "[RenderLight]")
{
    const CameraFrame  frame  = front_camera();
    const LightRequest left   = *read_light({{"from", "left"}, {"elevation_deg", 15}});
    const json         answer = light_json(left, frame.view);
    CHECK(answer["from"] == "left");
    CHECK_THAT(answer["elevation_deg"].get<double>(), Catch::Matchers::WithinAbs(15., 1e-12));   // as asked
    CHECK(answer.contains("bed_elevation_deg"));
    CHECK(answer["direction"][0].get<double>() > 0.9);   // travelling +X: from the left

    json back = answer;
    back["applied"] = true;
    const LightRequest again = *read_light(back);
    CHECK(again.from == "left");
    CHECK_THAT(again.elevation_deg, Catch::Matchers::WithinAbs(15., 1e-12));

    CHECK(read_light({{"from", "left"}})->elevation_deg == 20.);   // the default
    CHECK(refusal_of({{"from", "behind"}}).find("light.from") != std::string::npos);
    CHECK(refusal_of({{"from", "left"}, {"elevation_deg", 0}}).find("between 1 and 90") != std::string::npos);
}

TEST_CASE("a light straight above is accepted back with any azimuth", "[RenderLight]")
{
    const LightRequest above  = *read_light({{"azimuth_deg", 17}, {"elevation_deg", 90}});
    const json         answer = light_json(above, Eigen::Matrix4d::Identity());
    CHECK_NOTHROW(read_light(answer));
    json other = answer;
    other["azimuth_deg"] = 200;   // the azimuth means nothing at 90 degrees
    CHECK_NOTHROW(read_light(other));
    other["elevation_deg"] = 60;
    CHECK(refusal_of(other).find("different lights") != std::string::npos);
}

TEST_CASE("the raking light draws a lit face near 0.9 and a shadowed one near 0.28 of its colour", "[RenderLight]")
{
    for (const double emission : {0.3, 0.0, 0.2}) {
        for (const json& request : {json({{"azimuth_deg", 30}, {"elevation_deg", 12}}), json({{"from", "left"}})}) {
            const ShaderSettings s = raking_light_settings(*read_light(request), Eigen::Matrix4d::Identity(), emission);
            INFO(request.dump() << " emission " << emission);
            const double lit = raking_brightness(1., s, emission), shadow = raking_brightness(0., s, emission);
            CHECK(lit >= 0.88);
            CHECK(lit <= 0.92);
            CHECK(shadow >= 0.25);
            CHECK(shadow <= 0.30);
            CHECK(raking_brightness(-0.5, s, emission) == shadow);   // turned away adds nothing
            CHECK(raking_brightness(0.5, s, emission) > shadow);
        }
    }
    // The default settings are the thumbnail shader's: its brightest face is 0.3 + 0.3 + 0.48 + the second light.
    CHECK(raking_brightness(1., ShaderSettings{}, 0.3) == Catch::Approx(0.3 + 0.3 + k_top_diffuse));
}
