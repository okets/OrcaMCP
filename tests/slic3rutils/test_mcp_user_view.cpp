#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPUserView.hpp"

// What show_view decides before it touches the user's window: which tab and View menu camera a name
// means, and what it refuses.

using namespace Slic3r::GUI::OrcaMCP;

namespace {
const std::vector<std::string> k_tabs = {"home", "prepare", "preview", "monitor", "project", "calibration"};

bool contains(const std::optional<std::string>& text, const std::string& part) { return text && text->find(part) != std::string::npos; }

ShowViewRequest tab(const std::string& name)
{
    ShowViewRequest request;
    request.tab = name;
    return request;
}
} // namespace

TEST_CASE("a tab's name maps to the main window's tab id, and back", "[McpUserView][orcamcp]")
{
    CHECK(tab_id_named("device") == std::optional<std::string>("monitor"));
    CHECK(tab_id_named("prepare") == std::optional<std::string>("prepare"));
    CHECK_FALSE(tab_id_named("settings"));
    CHECK(tab_name_of_id("monitor") == "device");
    CHECK(tab_name_of_id("plugin_page") == "plugin_page");
    for (const std::string& name : tab_names())
        CHECK(tab_name_of_id(*tab_id_named(name)) == name);
}

TEST_CASE("the cameras are the View menu's, named as render_plate_view names them", "[McpUserView][orcamcp]")
{
    // back is the menu's Rear; default is its Default View, which also zooms to the bed.
    CHECK(camera_named("back")->direction == "rear");
    CHECK(camera_named("default")->direction == "plate");
    CHECK(camera_named("default")->zoom_to_bed);
    CHECK_FALSE(camera_named("top")->zoom_to_bed);
    CHECK_FALSE(camera_named("low"));
}

TEST_CASE("a camera looking along a named view is reported by that name", "[McpUserView][orcamcp]")
{
    const double h = std::sqrt(0.5);
    CHECK(named_view_of(Eigen::Vector3d(0., 0., -1.)) == std::optional<std::string>("top"));
    CHECK(named_view_of(Eigen::Vector3d(0., 2., 0.)) == std::optional<std::string>("front"));
    CHECK(named_view_of(Eigen::Vector3d(0., -1., 0.)) == std::optional<std::string>("back"));
    CHECK(named_view_of(Eigen::Vector3d(0., h, -h)) == std::optional<std::string>("default"));
    CHECK(named_view_of(Eigen::Vector3d(0.5, 0.5, -h)) == std::optional<std::string>("iso"));
    CHECK_FALSE(named_view_of(Eigen::Vector3d(0.3, 0.4, -0.5)));
}

TEST_CASE("show_view refuses a tab it does not know, or one the window does not have now", "[McpUserView][orcamcp]")
{
    CHECK(contains(show_view_refusal(tab("settings"), k_tabs, "prepare", 1), "not a tab"));
    CHECK(contains(show_view_refusal(tab("multi_device"), k_tabs, "multi_device", 1), "no multi_device tab now"));
    CHECK_FALSE(show_view_refusal(tab("device"), k_tabs, "monitor", 1));
}

TEST_CASE("the camera works only where the 3D view shows", "[McpUserView][orcamcp]")
{
    ShowViewRequest request = tab("device");
    request.camera          = "top";
    CHECK(contains(show_view_refusal(request, k_tabs, "monitor", 1), "pass tab prepare or preview"));
    request.tab = "preview";
    CHECK_FALSE(show_view_refusal(request, k_tabs, "preview", 1));
    ShowViewRequest camera_only;
    camera_only.camera = "sideways";
    CHECK(contains(show_view_refusal(camera_only, k_tabs, "prepare", 1), "not a view"));
}

TEST_CASE("an object is zoomed to in Prepare only, one that exists, and not with another zoom", "[McpUserView][orcamcp]")
{
    ShowViewRequest request;
    request.object_id = 0;
    CHECK_FALSE(show_view_refusal(request, k_tabs, "prepare", 1));
    CHECK(contains(show_view_refusal(request, k_tabs, "preview", 1), "Prepare tab only"));
    request.object_id = 3;
    CHECK(contains(show_view_refusal(request, k_tabs, "prepare", 1), "Invalid object_id 3"));
    request.object_id = 0;
    request.zoom_to   = "bed";
    CHECK(contains(show_view_refusal(request, k_tabs, "prepare", 1), "pass one"));
    ShowViewRequest zoom;
    zoom.zoom_to = "everything";
    CHECK(contains(show_view_refusal(zoom, k_tabs, "prepare", 1), "not a zoom"));
}

TEST_CASE("a call with nothing in it is no refusal: it reports the view", "[McpUserView][orcamcp]")
{
    CHECK_FALSE(show_view_refusal(ShowViewRequest{}, k_tabs, "home", 0));
}
