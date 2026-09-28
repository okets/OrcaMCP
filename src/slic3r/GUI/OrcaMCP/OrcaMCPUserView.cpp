// src/slic3r/GUI/OrcaMCP/OrcaMCPUserView.cpp
#include "OrcaMCPUserView.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// show_view's tab names and MainFrame's tab ids (TAB_ID_* in MainFrame.hpp).
const std::vector<std::pair<std::string, std::string>>& tab_table()
{
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"home", "home"},         {"prepare", "prepare"},         {"preview", "preview"},         {"device", "monitor"},
        {"device_web", "monitor_web"}, {"multi_device", "multi_device"}, {"project", "project"}, {"calibration", "calibration"},
    };
    return table;
}

// The View menu's cameras, and render_plate_view's names for them.
struct NamedCamera
{
    std::string  name;
    CameraChoice choice;
    Eigen::Vector3d forward; // where Camera::select_view(direction) looks
};
const std::vector<NamedCamera>& camera_table()
{
    const double h = std::sqrt(0.5);
    static const std::vector<NamedCamera> table = {
        {"default", {"plate", true}, Eigen::Vector3d(0., h, -h)},
        {"iso", {"iso", false}, Eigen::Vector3d(0.5, 0.5, -h)},
        {"top", {"top", false}, Eigen::Vector3d(0., 0., -1.)},
        {"bottom", {"bottom", false}, Eigen::Vector3d(0., 0., 1.)},
        {"front", {"front", false}, Eigen::Vector3d(0., 1., 0.)},
        {"back", {"rear", false}, Eigen::Vector3d(0., -1., 0.)},
        {"left", {"left", false}, Eigen::Vector3d(1., 0., 0.)},
        {"right", {"right", false}, Eigen::Vector3d(-1., 0., 0.)},
    };
    return table;
}

std::string joined(const std::vector<std::string>& names)
{
    std::string list;
    for (const std::string& name : names)
        list += (list.empty() ? "" : ", ") + name;
    return list;
}

bool shows_the_3d_view(const std::string& tab_id) { return tab_id == "prepare" || tab_id == "preview"; }

} // namespace

std::optional<std::string> tab_id_named(const std::string& name)
{
    for (const auto& [tab_name, id] : tab_table())
        if (tab_name == name)
            return id;
    return std::nullopt;
}

std::string tab_name_of_id(const std::string& id)
{
    for (const auto& [tab_name, tab_id] : tab_table())
        if (tab_id == id)
            return tab_name;
    return id;
}

const std::vector<std::string>& tab_names()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const auto& entry : tab_table())
            out.push_back(entry.first);
        return out;
    }();
    return names;
}

std::optional<CameraChoice> camera_named(const std::string& name)
{
    for (const NamedCamera& camera : camera_table())
        if (camera.name == name)
            return camera.choice;
    return std::nullopt;
}

const std::vector<std::string>& camera_names()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const NamedCamera& camera : camera_table())
            out.push_back(camera.name);
        return out;
    }();
    return names;
}

std::optional<std::string> named_view_of(const Eigen::Vector3d& forward)
{
    if (forward.norm() == 0.)
        return std::nullopt;
    const Eigen::Vector3d looking = forward.normalized();
    for (const NamedCamera& camera : camera_table())
        if (looking.dot(camera.forward.normalized()) > 0.9999)
            return camera.name;
    return std::nullopt;
}

std::optional<ZoomTarget> zoom_named(const std::string& name)
{
    if (name == "plate")
        return ZoomTarget::plate;
    if (name == "bed")
        return ZoomTarget::bed;
    if (name == "objects")
        return ZoomTarget::objects;
    return std::nullopt;
}

std::optional<std::string> show_view_refusal(const ShowViewRequest& request, const std::vector<std::string>& tabs_present,
                                             const std::string& tab_after, int object_count)
{
    if (request.tab) {
        const std::optional<std::string> id = tab_id_named(*request.tab);
        if (!id)
            return "tab \"" + *request.tab + "\" is not a tab: " + joined(tab_names());
        if (std::find(tabs_present.begin(), tabs_present.end(), *id) == tabs_present.end()) {
            std::vector<std::string> present;
            for (const std::string& tab : tabs_present)
                present.push_back(tab_name_of_id(tab));
            return "The window has no " + *request.tab + " tab now; it has " + joined(present);
        }
    }
    if (request.camera && !camera_named(*request.camera))
        return "camera \"" + *request.camera + "\" is not a view: " + joined(camera_names());
    if (request.zoom_to && !zoom_named(*request.zoom_to))
        return "zoom_to \"" + *request.zoom_to + "\" is not a zoom: plate, bed or objects";
    if (request.zoom_to && request.object_id)
        return std::string("zoom_to and object_id both say where to zoom: pass one");
    const bool moves_camera = request.camera || request.zoom_to || request.object_id;
    if (moves_camera && !shows_the_3d_view(tab_after))
        return "The camera belongs to the 3D view, which the " + tab_name_of_id(tab_after) +
               " tab does not show: pass tab prepare or preview with it";
    if (request.object_id) {
        if (*request.object_id < 0 || *request.object_id >= object_count)
            return "Invalid object_id " + std::to_string(*request.object_id) + ": the scene has " + std::to_string(object_count) + " objects";
        if (tab_after == "preview")
            return std::string("The app zooms to an object in the Prepare tab only (Preview zooms to the plate): pass tab prepare");
    }
    return std::nullopt;
}

}}} // namespace Slic3r::GUI::OrcaMCP
