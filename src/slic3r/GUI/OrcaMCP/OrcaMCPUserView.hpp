// src/slic3r/GUI/OrcaMCP/OrcaMCPUserView.hpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <Eigen/Core>

// What show_view decides before it touches the user's window: which tab a name means, which of the View
// menu's cameras, and why a call is refused. show_view changes what the user sees, on the user's request
// -- render_plate_view draws for the agent and never moves the user's view. No wx: the tests drive it
// with plain values (tests/slic3rutils/test_mcp_user_view.cpp).
//
// The description of the user's view (user_view: tab, camera, plate) is built in one place,
// OrcaMCPViewTools.cpp, so a later tool that reads the window describes it in the same terms.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A tab of the main window by the name show_view takes and reports (device is the Device tab, whose id is
// "monitor"), or nullopt for an unknown name.
std::optional<std::string> tab_id_named(const std::string& name);
// The name show_view reports for a tab id (MainFrame's TAB_ID_*); the id itself for one it does not name.
std::string tab_name_of_id(const std::string& id);
// Every name show_view takes, for a refusal.
const std::vector<std::string>& tab_names();

// One of the View menu's cameras: Camera::select_view's direction, and whether the menu zooms to the bed
// after it (Default View, Ctrl+0).
struct CameraChoice
{
    std::string direction;
    bool        zoom_to_bed = false;
};
// By the name show_view takes -- default, iso, top, bottom, front, back, left, right, as render_plate_view
// names its cameras (back is the menu's Rear) -- or nullopt.
std::optional<CameraChoice> camera_named(const std::string& name);
const std::vector<std::string>& camera_names();

// The camera's named view (camera_names) from the direction it looks in, or nullopt when it looks in any
// other (the user turned it).
std::optional<std::string> named_view_of(const Eigen::Vector3d& forward);

enum class ZoomTarget { plate, bed, objects };
std::optional<ZoomTarget> zoom_named(const std::string& name);

// A show_view call.
struct ShowViewRequest
{
    std::optional<std::string> tab;
    std::optional<std::string> camera;
    std::optional<std::string> zoom_to;
    std::optional<int>         object_id;
};

// Why show_view refuses `request`, or nullopt: an unknown tab, camera or zoom; a tab the window does not
// have now (`tabs_present`: tab ids); a camera, a zoom or an object with a tab other than Prepare or
// Preview showing after the call (`tab_after`: tab id) -- the View menu works only there; an object in
// Preview, where the app zooms to no object; an object_id out of range; zoom_to and object_id together.
std::optional<std::string> show_view_refusal(const ShowViewRequest& request, const std::vector<std::string>& tabs_present,
                                             const std::string& tab_after, int object_count);

}}} // namespace Slic3r::GUI::OrcaMCP
