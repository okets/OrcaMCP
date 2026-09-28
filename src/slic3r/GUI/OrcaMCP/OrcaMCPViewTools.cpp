// src/slic3r/GUI/OrcaMCP/OrcaMCPViewTools.cpp
//
// show_view: what the user sees in the OrcaMCP window -- the tab, and the 3D view's camera -- changed on
// the user's request, through the tab bar's and the View menu's own calls. render_plate_view draws for
// the agent and never moves the user's view; slice_all puts back the view its Slice button's event
// changes. The decisions are OrcaMCPUserView.hpp.

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPartTools.hpp"
#include "OrcaMCPPendingEvents.hpp"
#include "OrcaMCPServer.hpp"
#include "OrcaMCPUserView.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Model.hpp"

#include <string>
#include <vector>

namespace {

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

std::vector<std::string> tabs_present(Notebook& tabs)
{
    std::vector<std::string> ids;
    for (size_t i = 0; i < tabs.GetPageCount(); ++i)
        if (const std::string id = into_u8(tabs.GetPageName(i)); !id.empty())
            ids.push_back(id);
    return ids;
}

// What the user's window shows: the tab, the 3D view's camera (its named view when it looks along one,
// the projection, where it looks at, its zoom) and the current plate. One description, for show_view's
// answers and any later tool that reads the window.
nlohmann::json user_view_json(Plater& plater, Notebook& tabs)
{
    Camera&                          camera = plater.get_camera();
    const std::optional<std::string> view   = named_view_of(camera.get_dir_forward());
    const Vec3d&                     target = camera.get_target();
    return {{"tab", tab_name_of_id(into_u8(tabs.GetSelectedPageName()))},
            {"camera",
             {{"view", view ? nlohmann::json(*view) : nlohmann::json(nullptr)},
              {"projection", camera.get_type_as_string()},
              {"target", {target.x(), target.y(), target.z()}},
              {"zoom", camera.get_zoom()}}},
            {"plate_index", plater.get_partplate_list().get_curr_plate_index()}};
}

// The name of the toolbar tool (gizmo) open in the 3D view, or "".
std::string open_toolbar_tool(Plater& plater)
{
    const GLGizmosManager& gizmos  = plater.get_view3D_canvas3D()->get_gizmos_manager();
    const GLGizmoBase*     current = gizmos.get_current();
    return gizmos.get_current_type() == GLGizmosManager::Undefined || current == nullptr ? std::string() : current->get_name(false);
}

// The tab bar's click on tab `id`: MainFrame::select_tab, whose handler posts the plater its view change,
// which runs here, inside the call's dialog suppression, rather than after it returned, with whatever the
// plater had queued before it. Switching to Preview slices the selected plate when it has no result, as
// the tab does.
void switch_tab(Plater& plater, const std::string& id)
{
    wxGetApp().mainframe->select_tab(from_u8(id));
    process_queued_events(plater);
}

// The View menu's view, on the canvas the tab shows: where the camera looks from (Default View also
// zooms to the bed, as the menu does).
void apply_camera(Plater& plater, const std::string& name)
{
    const CameraChoice choice = *camera_named(name);
    plater.select_view(choice.direction);
    if (choice.zoom_to_bed)
        plater.get_current_canvas3D()->zoom_to_bed();
}

void apply_zoom(Plater& plater, ZoomTarget target)
{
    GLCanvas3D* canvas = plater.get_current_canvas3D();
    switch (target) {
    case ZoomTarget::bed: canvas->zoom_to_bed(); break;
    case ZoomTarget::plate: canvas->zoom_to_plate(REQUIRES_ZOOM_TO_CUR_PLATE); break;
    case ZoomTarget::objects: canvas->zoom_to_volumes(); break;
    }
}

nlohmann::json show_view_on_main_thread(const ShowViewRequest& request)
{
    Plater*      plater = wxGetApp().plater();
    Notebook&    tabs   = *wxGetApp().mainframe->m_tabpanel;
    const std::string current = into_u8(tabs.GetSelectedPageName());
    const std::string after   = request.tab ? *tab_id_named(*request.tab) : current;
    if (const auto refusal = show_view_refusal(request, tabs_present(tabs), after, int(plater->model().objects.size())))
        return error_response(*refusal);

    McpDialogSuppressionGuard guard;
    nlohmann::json            answer   = {{"status", "success"}, {"previous_tab", tab_name_of_id(current)}};
    bool                      changed  = false;
    bool                      slicing  = false;
    if (after != current) {
        const std::string tool          = open_toolbar_tool(*plater);
        const bool        slicing_before = plater->is_background_process_slicing();
        switch_tab(*plater, after);
        if (into_u8(tabs.GetSelectedPageName()) != after)
            return guard.report(error_response("The app did not switch to the " + *request.tab + " tab"));
        changed = true;
        if (!tool.empty() && open_toolbar_tool(*plater).empty())
            answer["closed_toolbar_tool"] = tool;
        slicing = !slicing_before && plater->is_background_process_slicing();
        if (slicing)
            record_selected_plate_slice_run(plater->get_partplate_list());
    }
    // The view first, then the zoom: a zoom keeps the direction, a view keeps the zoom.
    if (request.camera)
        apply_camera(*plater, *request.camera);
    if (request.zoom_to)
        apply_zoom(*plater, *zoom_named(*request.zoom_to));
    if (request.object_id) {
        if (const auto refusal = select_as_a_click_does(*plater, *request.object_id))
            return guard.report(error_response(*refusal));
        plater->get_current_canvas3D()->zoom_to_selection();
        answer["selected_object"] = *request.object_id;
    }
    if (request.camera || request.zoom_to || request.object_id) {
        GLCanvas3D* canvas = plater->get_current_canvas3D();
        canvas->set_as_dirty();
        canvas->request_extra_frame();
        changed = true;
    }
    answer["changed"]   = changed;
    answer["user_view"] = user_view_json(*plater, tabs);
    if (slicing)
        answer["slice_started"] = "the Preview tab slices the selected plate when it has no result, as it does for the user";
    answer["active_warnings"] = get_active_warnings_json(plater);
    add_next_steps(answer, show_view_next_steps(slicing));
    return guard.report(answer);
}

std::optional<std::string> read_request(const nlohmann::json& params, ShowViewRequest& request)
{
    for (const char* key : {"tab", "camera", "zoom_to"}) {
        if (!params.contains(key))
            continue;
        if (!params.at(key).is_string())
            return std::string(key) + " must be text, got " + params.at(key).dump();
        (key == std::string("tab") ? request.tab : key == std::string("camera") ? request.camera : request.zoom_to) =
            params.at(key).get<std::string>();
    }
    if (params.contains("object_id")) {
        int id = -1;
        if (!parse_integer_param(params.at("object_id"), id))
            return "object_id must be a whole number, as get_scene_info gives it; got " + params.at("object_id").dump();
        request.object_id = id;
    }
    return std::nullopt;
}

} // namespace

void Slic3r::GUI::OrcaMCPServer::register_view_tools()
{
    register_tool({
        "show_view",
        ToolCategory::Visualization,
        "Show the user a tab, view or object",
        "Change what the user sees in the OrcaMCP window, when the user asks to be shown something: the tab (prepare, "
        "preview, device, home, project, calibration, multi_device, as the window has them now) and the 3D view's camera, as "
        "the tab bar and the View menu change them. camera is one of the View menu's views (default, top, bottom, front, "
        "back, left, right) or iso; zoom_to fits the view to the current plate, the bed or every object; object_id selects "
        "that object as a click on its row does and zooms to it (Prepare only). The camera works in Prepare and Preview only. "
        "Switching to preview slices the selected plate when it has no result, as the tab does for the user (slice_started, "
        "and next_steps names wait_for_slice), and closes an open toolbar tool (closed_toolbar_tool). No other tool moves the "
        "user's view: render_plate_view draws images for you. Called with nothing, it changes nothing and says what the user "
        "sees (user_view: tab, camera, plate). No undo step.",
        {{"type", "object"},
         {"properties",
          {{"tab",
            {{"type", "string"},
             {"enum", tab_names()},
             {"description", "The tab to show: prepare (the 3D view), preview (the sliced G-code), device, home, project, calibration or "
                             "multi_device; device_web in printer-agents mode"}}},
           {"camera", {{"type", "string"}, {"enum", camera_names()}, {"description", "A View menu view: default (and zoom to the bed), iso, top, bottom, front, back, left, right"}}},
           {"zoom_to", {{"type", "string"}, {"enum", {"plate", "bed", "objects"}}, {"description", "Fit the view to the current plate, the bed or every object"}}},
           {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Select this object and zoom to it (Prepare only)"}}}}}},
        [](const nlohmann::json& params) -> nlohmann::json {
            ShowViewRequest request;
            if (const auto error = read_request(params, request))
                return error_response(*error);
            return run_on_main_thread([request]() -> nlohmann::json { return show_view_on_main_thread(request); });
        }});
}
