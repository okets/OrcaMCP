// src/slic3r/GUI/OrcaMCP/OrcaMCPPartTools.cpp
#include "OrcaMCPPartTools.hpp"

#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPFilamentModel.hpp"
#include "OrcaMCPInstanceEdits.hpp"
#include "OrcaMCPMeshHealth.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPPlateUtils.hpp"
#include "OrcaMCPUiJob.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// ---- Reading a call ----
// (read_volume_id, read_flag and read_text, which never read a value given as left out, are in
// OrcaMCPPartEdits.)

// A text argument `key`, or "" when the call leaves it out; `error` says what is wrong with one given.
std::string string_arg(const nlohmann::json& params, const std::string& key, std::string& error)
{
    return read_text(params, key, error).value_or(std::string());
}

// Why an edit must wait (edit_job_refusal): an arrange, orient or bed fill runs.
std::optional<std::string> ui_job_refusal(Plater& plater, const std::string& tool)
{
    return edit_job_refusal(!plater.get_ui_job_worker().is_idle(), tool);
}

// keep_painting as the call gives it, else the app's "Keep painted feature after mesh change" setting,
// which the object list's split and boolean read.
struct KeepPainting
{
    bool value = false;
    bool given = false;
};
KeepPainting read_keep_painting(const nlohmann::json& params, std::string& error)
{
    const std::optional<bool> given = read_flag(params, "keep_painting", error);
    return {given.value_or(wxGetApp().app_config->get_bool("keep_painting")), given.has_value()};
}

void add_keep_painting(nlohmann::json& answer, const KeepPainting& keep)
{
    answer["keep_painting"]      = keep.value;
    answer["keep_painting_from"] = keep.given ? "argument" : "app_setting";
}

// The app's keep_painting setting, as the call asked, for the object list's action that reads it, and
// back as it was afterwards.
class ScopedKeepPainting
{
public:
    explicit ScopedKeepPainting(bool keep) : m_before(wxGetApp().app_config->get("app", "keep_painting"))
    {
        m_changed = wxGetApp().app_config->get_bool("keep_painting") != keep;
        if (m_changed)
            wxGetApp().app_config->set_bool("keep_painting", keep);
    }
    ~ScopedKeepPainting()
    {
        if (m_changed)
            wxGetApp().app_config->set("app", "keep_painting", m_before);
    }

private:
    std::string m_before;
    bool        m_changed = false;
};

// ---- Selecting as a click does ----

int index_of(const ModelObject& object, const ModelVolume* volume)
{
    const auto it = std::find(object.volumes.begin(), object.volumes.end(), volume);
    return it == object.volumes.end() ? -1 : int(it - object.volumes.begin());
}

// The canvas the object list selects in: the Assemble view's when it shows, else the 3D view's
// (ObjectList::update_selections_on_canvas).
GLCanvas3D* list_canvas(Plater& plater)
{
    GLCanvas3D* current = plater.get_current_canvas3D();
    return current != nullptr && current->get_canvas_type() == GLCanvas3D::ECanvasType::CanvasAssembleView ? current :
                                                                                                               plater.get_view3D_canvas3D();
}

// The object list's row of volume `volume_idx` of object `object_id`, as the list maps its rows to volumes
// (a cut object's connectors have no row, so a row's index is not always its volume's), or an empty item
// for a volume without a row of its own.
wxDataViewItem volume_row(ObjectDataViewModel& rows, int object_id, int volume_idx)
{
    const wxDataViewItem item = rows.GetItemByVolumeId(object_id, rows.get_real_volume_index_in_ui(object_id, volume_idx));
    return item.IsOk() && (rows.GetItemType(item) & itVolume) ? item : wxDataViewItem();
}

// The volume the object list's selected row stands for, mapped as the list's Change Type maps it
// (ObjectList::set_volume_type), or -1 when no volume row is selected.
int selected_volume(ObjectList& list, int object_id)
{
    const wxDataViewItem  item = list.GetSelection();
    ObjectDataViewModel& rows = *list.GetModel();
    if (!item.IsOk() || !(rows.GetItemType(item) & itVolume))
        return -1;
    return rows.get_real_volume_index_in_3d(object_id, rows.GetVolumeIdByItem(item));
}

// Selects object `object_id` -- or its volume `volume` -- in the object list, and so in the 3D view, as a
// click on its row does, for the list's action that reads the selection. A volume without a row of its
// own (the one volume of a one-volume object) is selected by its object's row, as in the list. Selecting
// is not an edit: no undo step. The 3D view is brought up to date first: one that postponed its reload
// has no volumes to select. Why the selection could not be made, or nothing.
std::optional<std::string> select_in_object_list(Plater& plater, int object_id, ModelVolume* volume = nullptr)
{
    Plater::SuppressSnapshots not_an_edit(&plater);
    bool                      scene_current = false;
    OrcaMCPPlateUtils::SceneCanvas(scene_current);
    ModelObject* object     = plater.model().objects[std::size_t(object_id)];
    ObjectList*  list       = wxGetApp().obj_list();
    const int    volume_idx = volume != nullptr ? index_of(*object, volume) : -1;
    bool         volume_selected = true;
    if (volume_idx >= 0 && volume_listed_in_object_list(*object, volume_idx)) {
        const wxDataViewItem row = volume_row(*list->GetModel(), object_id, volume_idx);
        wxDataViewItemArray  rows;
        if (row.IsOk())
            rows.Add(row);
        list->select_items(rows);
        list->selection_changed();
        volume_selected = selected_volume(*list, object_id) == volume_idx;
    } else {
        list->select_item(ObjectVolumeID{object, nullptr});
    }
    const bool object_selected = list->get_selected_obj_idx() == object_id;
    const bool scene_selected  = list_canvas(plater)->get_selection().get_object_idx() == object_id;
    if (object_selected && volume_selected && scene_selected)
        return std::nullopt;
    return "the app could not select object " + std::to_string(object_id) + (volume_idx >= 0 ? " volume " + std::to_string(volume_idx) : "") +
           " in its object list and 3D view (" +
           (scene_current ? "the list did not take the selection" : "the 3D view could not be brought up to date") +
           "), so nothing was changed";
}

// Selects the objects in the object list, as a click on each row with the modifier key held does.
std::optional<std::string> select_objects_in_object_list(Plater& plater, const std::vector<int>& object_ids)
{
    Plater::SuppressSnapshots not_an_edit(&plater);
    bool                      scene_current = false;
    OrcaMCPPlateUtils::SceneCanvas(scene_current);
    std::vector<ObjectVolumeID> rows;
    for (int id : object_ids)
        rows.push_back({plater.model().objects[std::size_t(id)], nullptr});
    ObjectList* list = wxGetApp().obj_list();
    list->select_items(rows);
    wxDataViewItemArray selected;
    list->GetSelections(selected);
    std::set<int> selected_ids;
    for (const wxDataViewItem& item : selected)
        if (list->GetModel()->GetItemType(item) & itObject)
            selected_ids.insert(list->GetModel()->GetIdByItem(item));
    if (selected_ids == std::set<int>(object_ids.begin(), object_ids.end()) && selected.size() == object_ids.size())
        return std::nullopt;
    return std::string("the app could not select those objects in its object list, so nothing was changed");
}

// ---- Before a change ----

// An open toolbar tool (gizmo) closed right before a change that is about to be made, as the user
// closes it (close_open_toolbar_tool). An error answer when it did not close, or when closing it changed
// the object's volumes (it never should).
std::optional<nlohmann::json> close_toolbar_tool_before_change(Plater& plater, int object_id, std::size_t volumes_before,
                                                               std::string& closed_tool)
{
    closed_tool = close_open_toolbar_tool(plater);
    if (closed_tool.empty())
        return std::nullopt;
    if (toolbar_tool_open(plater))
        return with_closed_tool(error_response("The toolbar tool " + closed_tool + " is open in the app and did not close, so nothing was changed"),
                                closed_tool);
    const Model& model = plater.model();
    if (object_id >= 0 && (std::size_t(object_id) >= model.objects.size() || model.objects[std::size_t(object_id)]->volumes.size() != volumes_before))
        return with_closed_tool(error_response("Closing the toolbar tool " + closed_tool + " changed object " + std::to_string(object_id) +
                                               ", so nothing else was changed: call this again"),
                                closed_tool);
    return std::nullopt;
}

// What the object list does for a volume's change it makes itself (GLCanvas3D::do_move and friends):
// the object's mesh backup, its plates told, its list rows brought up to date. The placement fields go
// into `answer`.
void after_volume_change(Plater& plater, int object_id, nlohmann::json& answer)
{
    ModelObject& object = *plater.model().objects[std::size_t(object_id)];
    Slic3r::save_object_mesh(object);
    object.invalidate_bounding_box();
    rehome_and_report_placement(answer, object_id, /*moved=*/true);
    ObjectList* list = wxGetApp().obj_list();
    list->update_info_items(std::size_t(object_id));
    list->update_plate_values_for_items();
    plater.update();
}

// ---- move, rotate, scale, mirror of one volume ----

struct VolumeTransformRequest
{
    Transform3d transform = Transform3d::Identity();
    bool        changes   = false;
    bool        move      = false;
    std::string snapshot;
};

// The call's transform, in plate axes, or an error answer.
std::optional<nlohmann::json> read_volume_transform(const nlohmann::json& params, VolumeTransformKind kind, const BoundingBoxf3& box,
                                                    VolumeTransformRequest& out)
{
    if (kind == VolumeTransformKind::mirror) {
        std::string       error;
        const std::string axis = string_arg(params, "axis", error);
        const int         index = axis == "x" || axis == "X" ? 0 : axis == "y" || axis == "Y" ? 1 : axis == "z" || axis == "Z" ? 2 : -1;
        if (index < 0)
            return error_response("axis must be x, y or z");
        Vec3d factors  = Vec3d::Ones();
        factors[index] = -1.;
        out.transform  = Geometry::scale_transform(factors);
        out.changes    = true;
        out.snapshot   = _u8L("Mirror");
        return std::nullopt;
    }
    PlateAxes axes;
    if (const auto error = read_plate_axes(params, "", axes))
        return error_response(*error);
    std::string error;
    if (kind == VolumeTransformKind::move) {
        const bool relative = read_flag(params, "relative", error).value_or(true);
        if (!error.empty())
            return error_response(error);
        const Vec3d centre = box.center();
        const Vec3d delta  = relative ? axes.value_or(Vec3d::Zero()) : Vec3d(axes.value_or(centre) - centre);
        out.transform      = Geometry::translation_transform(delta);
        out.changes        = !delta.isZero();
        out.move           = true;
        out.snapshot       = _u8L("Set Position");
        return std::nullopt;
    }
    if (kind == VolumeTransformKind::rotate) {
        const bool relative = read_flag(params, "relative", error).value_or(true);
        if (!error.empty())
            return error_response(error);
        if (!relative)
            return error_response("absolute rotation is not supported: pass the change in degrees");
        out.transform = Geometry::rotation_transform(axes.value_or(Vec3d::Zero()) * (M_PI / 180.));
        out.changes   = !out.transform.isApprox(Transform3d::Identity());
        out.snapshot  = _u8L("Set orientation");
        return std::nullopt;
    }
    const bool uniform = read_flag(params, "uniform", error).value_or(false);
    if (!error.empty())
        return error_response(error);
    if (uniform && !axes.axis[0] && (axes.axis[1] || axes.axis[2]))
        return error_response("uniform scales every axis by x: give x");
    const Vec3d given   = axes.value_or(Vec3d::Ones());
    const Vec3d factors = uniform ? Vec3d(given.x(), given.x(), given.x()) : given;
    if (!valid_scale_factors(factors))
        return error_response("Scale factors must be positive; use mirror_object to flip an axis");
    out.transform = Geometry::scale_transform(factors);
    out.changes   = !factors.isApprox(Vec3d::Ones());
    out.snapshot  = _u8L("Set scale");
    return std::nullopt;
}

nlohmann::json vec_json(const Vec3d& v) { return {{"x", v.x()}, {"y", v.y()}, {"z", v.z()}}; }

nlohmann::json transform_volume_on_main_thread(const nlohmann::json& params, VolumeTransformKind kind)
{
    Plater*      plater    = wxGetApp().plater();
    int          object_id = -1;
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), object_id, error);
    if (object == nullptr)
        return error_response(error);
    const std::optional<int> volume_id = read_volume_id(params, error);
    if (!error.empty() || !volume_id)
        return error_response(error.empty() ? "volume_id is required here: the volume to transform" : error);
    if (const auto refusal = volume_transform_refusal(object_id, *object, *volume_id, cut_siblings(plater->model(), object_id)))
        return error_response(*refusal);
    const std::size_t   volume = std::size_t(*volume_id);
    const BoundingBoxf3 before = volume_world_box(*object, volume);
    VolumeTransformRequest request;
    if (const auto refusal = read_volume_transform(params, kind, before, request))
        return *refusal;
    if (const auto refusal = ui_job_refusal(*plater, kind == VolumeTransformKind::move     ? "move_object" :
                                                     kind == VolumeTransformKind::rotate   ? "rotate_object" :
                                                     kind == VolumeTransformKind::scale    ? "scale_object" :
                                                                                             "mirror_object"))
        return error_response(*refusal);

    nlohmann::json answer = {{"status", "success"}, {"object_id", object_id}, {"volume_id", *volume_id}};
    if (!request.changes) {
        answer.update(volume_row_json(*object, volume));
        answer["previous_position"] = vec_json(before.center());
        report_placement(answer, object_id);
        answer["changed"] = false;
        return answer;
    }

    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool_before_change(*plater, object_id, object->volumes.size(), closed_tool))
        return *refusal;
    const auto part_has_skew = [object, volume] {
        return Geometry::Transformation(object->instances[0]->get_matrix() * object->volumes[volume]->get_matrix()).has_skew();
    };
    const bool                had_skew      = part_has_skew();
    const std::vector<double> lowest_before = instances_lowest_z(*object);
    plater->take_snapshot(request.snapshot);
    if (request.move)
        translate_volume_in_plate_frame(*object, volume, Vec3d(request.transform.translation()));
    else
        transform_volume_in_plate_frame(*object, volume, request.transform);
    const std::vector<double> dropped = drop_after_volume_change(*object, lowest_before, request.move ? VolumeChange::move : VolumeChange::reshape);

    answer.update(volume_row_json(*object, volume));
    answer["previous_position"] = vec_json(before.center());
    answer["dropped_to_bed_mm"] = dropped.empty() ? 0. : dropped.front();
    if (kind == VolumeTransformKind::scale && !had_skew && part_has_skew())
        answer["skew_warning"] = "A non-uniform scale along plate axes on a part turned by other than a multiple of 90 degrees "
                                 "sheared it. Use uniform=true, or turn the part back first.";
    after_volume_change(*plater, object_id, answer);
    answer["active_warnings"] = get_active_warnings_json(plater);
    return with_closed_tool(std::move(answer), closed_tool);
}

// ---- delete ----

nlohmann::json delete_volume_on_main_thread(Plater& plater, int object_id, ModelObject& object, int volume_id)
{
    if (const auto error = volume_id_error(object_id, object, volume_id))
        return error_response(*error);
    if (const auto refusal = volume_delete_refusal(object_id, object, volume_id, cut_siblings(plater.model(), object_id)))
        return error_response(*refusal);
    if (const auto refusal = ui_job_refusal(plater, "delete_object"))
        return error_response(*refusal);
    const nlohmann::json           deleted = volume_row_json(object, std::size_t(volume_id));
    const std::vector<std::string> moving  = settings_moving_to_object(object, volume_id);
    const std::size_t              before  = object.volumes.size();

    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool_before_change(plater, object_id, before, closed_tool))
        return *refusal;
    McpDialogSuppressionGuard guard;
    {
        // The Delete key's undo step (Plater::remove_selected); the list's own inside it is suppressed.
        Plater::TakeSnapshot snapshot(&plater, "Delete Selected Objects");
        wxGetApp().obj_list()->delete_from_model_and_list(std::vector<ItemForDelete>{ItemForDelete(itVolume, object_id, volume_id)});
    }
    if (object.volumes.size() == before)
        return with_closed_tool(guard.fail_on_errors(error_response("The object list did not delete volume " + std::to_string(volume_id) +
                                                                    " of object " + std::to_string(object_id))),
                                closed_tool);
    if (object.is_cut()) // the list deleted its row by its own index, which hides connectors: list the rest again
        wxGetApp().obj_list()->add_volumes_to_object_in_list(std::size_t(object_id));

    nlohmann::json answer = {{"status", "success"},
                             {"object_id", object_id},
                             {"object_name", object.name},
                             {"deleted_volume", deleted},
                             {"volumes", volume_rows_json(object)}};
    if (!moving.empty())
        answer["settings_moved_to_object"] = moving;
    rehome_and_report_placement(answer, object_id, /*moved=*/true);
    answer["active_warnings"] = get_active_warnings_json(&plater);
    return with_closed_tool(guard.report(std::move(answer)), closed_tool);
}

nlohmann::json delete_instance_on_main_thread(Plater& plater, int object_id, ModelObject& object, int instance_id)
{
    if (const auto refusal = ui_job_refusal(plater, "delete_object"))
        return error_response(*refusal);
    const std::size_t before = object.instances.size();

    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool_before_change(plater, object_id, object.volumes.size(), closed_tool))
        return *refusal;
    McpDialogSuppressionGuard guard;
    {
        // The Delete key's undo step (Plater::remove_selected) on a selected instance; the list's own inside
        // it is suppressed. The list tells the plates, which file every later instance under its new index.
        Plater::TakeSnapshot snapshot(&plater, "Delete Selected Objects");
        wxGetApp().obj_list()->delete_from_model_and_list(itInstance, object_id, instance_id);
    }
    if (object.instances.size() == before)
        return with_closed_tool(guard.fail_on_errors(error_response("The object list did not delete instance " + std::to_string(instance_id) +
                                                                    " of object " + std::to_string(object_id))),
                                closed_tool);

    nlohmann::json answer = {{"status", "success"},
                             {"object_id", object_id},
                             {"object_name", object.name},
                             {"deleted_instance_id", instance_id},
                             {"instance_count", object.instances.size()}};
    if (std::size_t(instance_id) < object.instances.size())
        answer["instance_ids_shifted"] = "instances after " + std::to_string(instance_id) + " moved down by one";
    report_placement(answer, object_id);
    answer["active_warnings"] = get_active_warnings_json(&plater);
    return with_closed_tool(guard.report(std::move(answer)), closed_tool);
}

nlohmann::json delete_object_on_main_thread(Plater& plater, int object_id, ModelObject& object, bool include_preview)
{
    if (const auto refusal = ui_job_refusal(plater, "delete_object"))
        return error_response(*refusal);
    const std::string      name     = object.name;
    const std::vector<int> siblings = cut_siblings(plater.model(), object_id);
    const std::size_t      before   = plater.model().objects.size();

    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool_before_change(plater, -1, 0, closed_tool))
        return *refusal;
    McpDialogSuppressionGuard guard;
    {
        // The Delete key's undo step (Plater::remove_selected): Plater::remove took none, so undo after a
        // delete also undid the call before it. The list asks before deleting a piece of a cut (Delete,
        // answered Yes) and ends the cut's correspondence for its other pieces.
        Plater::TakeSnapshot snapshot(&plater, "Delete Selected Objects");
        wxGetApp().obj_list()->delete_from_model_and_list(itObject, object_id, 0);
    }
    if (plater.model().objects.size() == before)
        return with_closed_tool(guard.fail_on_errors(error_response("The object list did not delete object " + std::to_string(object_id))),
                                closed_tool);

    nlohmann::json answer = {{"status", "success"}, {"deleted_object_id", object_id}, {"deleted_object_name", name}};
    std::vector<int> invalidated;
    for (int sibling : siblings)
        if (sibling != object_id)
            invalidated.push_back(sibling > object_id ? sibling - 1 : sibling);
    if (!invalidated.empty())
        answer["cut_info_invalidated_for"] = invalidated;
    answer["active_warnings"] = get_active_warnings_json(&plater);
    add_turntable_preview_if_requested(answer, include_preview);
    return with_closed_tool(guard.report(std::move(answer)), closed_tool);
}

// ---- rename ----

nlohmann::json rename_on_main_thread(const nlohmann::json& params)
{
    Plater*      plater    = wxGetApp().plater();
    int          object_id = -1;
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), object_id, error);
    if (object == nullptr)
        return error_response(error);
    const std::optional<int> volume_id = read_volume_id(params, error);
    if (!error.empty())
        return error_response(error);
    if (!params.contains("new_name") || !params.at("new_name").is_string())
        return error_response("new_name must be a string");
    const std::string new_name = params.at("new_name").get<std::string>();
    if (const auto refusal = name_refusal(new_name))
        return error_response(*refusal);
    if (volume_id)
        if (const auto volume_error = volume_id_error(object_id, *object, *volume_id))
            return error_response(*volume_error);

    ObjectList*           list  = wxGetApp().obj_list();
    ObjectDataViewModel*  rows  = list->GetModel();
    nlohmann::json        answer = {{"status", "success"}, {"object_id", object_id}};
    if (volume_id) {
        ModelVolume&      volume   = *object->volumes[std::size_t(*volume_id)];
        const std::string old_name = volume.name;
        answer.update({{"volume_id", *volume_id}, {"old_name", old_name}, {"new_name", new_name}, {"changed", old_name != new_name}});
        if (old_name == new_name)
            return answer;
        // ObjectList::update_name_in_model's part branch, with the part found by itself: the list's row index
        // skips a cut object's connectors, and update_name_in_model reads it as the volume's. The volume's own
        // row is renamed when it has one; a one-volume object's part has none, and the row the list gives for
        // it is the object's, which keeps the object's name.
        plater->take_snapshot(_u8L("Rename Part"));
        volume.name = new_name;
        if (volume_listed_in_object_list(*object, *volume_id))
            if (const wxDataViewItem item = volume_row(*rows, object_id, *volume_id); item.IsOk())
                rows->SetName(from_u8(new_name), item);
        return answer;
    }
    const std::string old_name = object->name;
    answer.update({{"old_name", old_name}, {"new_name", new_name}, {"changed", old_name != new_name}});
    if (old_name == new_name)
        return answer;
    // The object list's rename (ObjectList::rename_item, after its dialog): the row renamed, then the
    // model -- which renames a one-part object's part too -- under its "Rename Object" undo step.
    const wxDataViewItem item = rows->GetItemById(object_id);
    if (!item.IsOk() || !rows->SetName(from_u8(new_name), item))
        return error_response("the object list has no row for object " + std::to_string(object_id) + ", so nothing was renamed");
    list->update_name_in_model(item);
    // The name is in the G-code (its object labels): the plates holding it no longer have its result.
    mark_object_plates_unsliced(plater->get_partplate_list(), object_id);
    return answer;
}

} // namespace

namespace Slic3r { namespace GUI { namespace OrcaMCP {

nlohmann::json transform_volume(const nlohmann::json& params, VolumeTransformKind kind)
{
    const bool include_preview    = params.value("include_preview", false);
    const int  preview_views      = params.value("preview_views", 4);
    const int  preview_resolution = params.value("preview_resolution", 256);
    return run_on_main_thread([&]() -> nlohmann::json {
        nlohmann::json answer = transform_volume_on_main_thread(params, kind);
        if (answer.value("status", "") == "success")
            add_turntable_preview_if_requested(answer, include_preview, preview_views, preview_resolution);
        return answer;
    });
}

nlohmann::json delete_in_object_list(const nlohmann::json& params)
{
    const bool include_preview = params.value("include_preview", false);
    return run_on_main_thread([&]() -> nlohmann::json {
        Plater*      plater    = wxGetApp().plater();
        int          object_id = -1;
        std::string  error;
        ModelObject* object = resolve_object_id(params, plater->model(), object_id, error);
        if (object == nullptr)
            return error_response(error);
        const std::optional<int> volume_id   = read_volume_id(params, error);
        const std::optional<int> instance_id = read_instance_id(params, error);
        if (!error.empty())
            return error_response(error);
        if (instance_id) {
            if (const auto refusal = instance_delete_refusal(object_id, *object, *instance_id, volume_id.has_value()))
                return error_response(*refusal);
            nlohmann::json answer = delete_instance_on_main_thread(*plater, object_id, *object, *instance_id);
            if (answer.value("status", "") == "success")
                add_turntable_preview_if_requested(answer, include_preview);
            return answer;
        }
        if (volume_id)
            return delete_volume_on_main_thread(*plater, object_id, *object, *volume_id);
        return delete_object_on_main_thread(*plater, object_id, *object, include_preview);
    });
}

nlohmann::json rename_in_object_list(const nlohmann::json& params)
{
    return run_on_main_thread([&]() -> nlohmann::json { return rename_on_main_thread(params); });
}

std::optional<std::string> select_as_a_click_does(Plater& plater, int object_id, ModelVolume* volume)
{
    return select_in_object_list(plater, object_id, volume);
}

std::optional<nlohmann::json> close_toolbar_tool_for_change(Plater& plater, int object_id, std::size_t volumes_before, std::string& closed_tool)
{
    return close_toolbar_tool_before_change(plater, object_id, volumes_before, closed_tool);
}

}}} // namespace Slic3r::GUI::OrcaMCP

namespace {

// ---- split_object ----

std::vector<int> volumes_not_in(const ModelObject& object, const std::set<const ModelVolume*>& before)
{
    std::vector<int> fresh;
    for (std::size_t i = 0; i < object.volumes.size(); ++i)
        if (before.count(object.volumes[i]) == 0)
            fresh.push_back(int(i));
    return fresh;
}

std::set<const ModelVolume*> volume_set(const ModelObject& object)
{
    return std::set<const ModelVolume*>(object.volumes.begin(), object.volumes.end());
}

nlohmann::json rows_of(const ModelObject& object, const std::vector<int>& volume_ids)
{
    nlohmann::json rows = nlohmann::json::array();
    for (int id : volume_ids)
        rows.push_back(volume_row_json(object, std::size_t(id)));
    return rows;
}

// Every object from `first` on, as get_scene_info describes one, with its placement.
nlohmann::json objects_from(const Model& model, std::size_t first)
{
    nlohmann::json objects = nlohmann::json::array();
    for (std::size_t i = first; i < model.objects.size(); ++i) {
        nlohmann::json summary = model_object_summary_json(*model.objects[i], int(i));
        report_placement(summary, int(i));
        objects.push_back(std::move(summary));
    }
    return objects;
}

std::vector<int> ids_from(std::size_t first, std::size_t end)
{
    std::vector<int> ids;
    for (std::size_t i = first; i < end; ++i)
        ids.push_back(int(i));
    return ids;
}

nlohmann::json split_to_objects(Plater& plater, int object_id, ModelObject& object, bool keep_height, const McpDialogSuppressionGuard& guard)
{
    Model&            model     = plater.model();
    const std::string name      = object.name;
    const nlohmann::json dropped = rows_of(object, volumes_dropped_by_split_to_objects(object));
    const std::size_t before    = model.objects.size();
    plater.split_object(object_id);
    if (model.objects.size() < before)
        return guard.fail_on_errors(error_response("Object " + std::to_string(object_id) + " was removed but no piece was added"));
    if (model.objects.size() == before && model.objects[std::size_t(object_id)] == &object)
        return guard.fail_on_errors(error_response("The object list did not split object " + std::to_string(object_id)));

    // The pieces are the last objects; the original's index is free, so every later object moved down by one.
    const std::size_t      first = before - 1;
    const std::vector<int> ids   = ids_from(first, model.objects.size());
    nlohmann::json         answer = {{"status", "success"},
                                     {"to", "objects"},
                                     {"object_id", object_id},
                                     {"object_name", name},
                                     {"new_object_ids", ids},
                                     {"new_objects", objects_from(model, first)},
                                     {"dropped_volumes", dropped}};
    if (object_id < int(first))
        answer["object_ids_shifted"] = "object " + std::to_string(object_id) + " is gone and every object after it moved down by one; the "
                                       "pieces are the last " + std::to_string(ids.size()) + ", objects " + listed_ids(ids);
    // The app asks only when auto-drop is on and a piece would float.
    answer["floating_pieces"] = guard.prompt_asked(MCP_PROMPT_SPLIT_FLOATING) ? nlohmann::json(keep_height ? "kept_height" : "dropped_to_bed")
                                                                               : nlohmann::json(nullptr);
    add_next_steps(answer, mesh_next_steps(model, ids, model_mesh_health(model)));
    return answer;
}

nlohmann::json split_to_parts(Plater& plater, int object_id, ModelObject& object, int volume_id, const McpDialogSuppressionGuard& guard)
{
    ModelVolume* target = object.volumes[std::size_t(volume_id)];
    if (const auto refusal = select_in_object_list(plater, object_id, object.volumes.size() > 1 ? target : nullptr))
        return error_response(*refusal);
    const std::set<const ModelVolume*> before = volume_set(object);
    plater.split_volume();
    std::vector<int> pieces = volumes_not_in(object, before);
    if (pieces.empty())
        return guard.fail_on_errors(error_response("The object list did not split volume " + std::to_string(volume_id) + " of object " +
                                                   std::to_string(object_id)));
    // The split volume becomes the first piece (ModelVolume::split).
    if (const int first = index_of(object, target); first >= 0)
        pieces.push_back(first);
    std::sort(pieces.begin(), pieces.end());
    nlohmann::json answer = {{"status", "success"},
                             {"to", "parts"},
                             {"object_id", object_id},
                             {"object_name", object.name},
                             {"volume_id", volume_id},
                             {"pieces", pieces},
                             {"volumes", volume_rows_json(object)}};
    report_placement(answer, object_id);
    add_next_steps(answer, split_parts_next_steps(object_id, volume_id, pieces));
    return answer;
}

nlohmann::json split_object_on_main_thread(const nlohmann::json& params)
{
    Plater*      plater    = wxGetApp().plater();
    int          object_id = -1;
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), object_id, error);
    if (object == nullptr)
        return error_response(error);
    const std::string to = string_arg(params, "to", error);
    if (to != "objects" && to != "parts")
        return error_response("to must be \"objects\" (one object per solid part, or per shell of a one-part object) or \"parts\" (one "
                              "part per shell of a volume)");
    const SplitTarget          target      = to == "objects" ? SplitTarget::objects : SplitTarget::parts;
    const std::optional<int>   volume_id   = read_volume_id(params, error);
    const std::optional<bool>  keep_height = error.empty() ? read_flag(params, "keep_height", error) : std::nullopt;
    const KeepPainting         keep        = error.empty() ? read_keep_painting(params, error) : KeepPainting{};
    if (!error.empty())
        return error_response(error);
    if (const auto refusal = split_refusal(object_id, *object, target, volume_id, keep_height.has_value(), cut_siblings(plater->model(), object_id)))
        return error_response(*refusal);
    if (const auto refusal = ui_job_refusal(*plater, "split_object"))
        return error_response(*refusal);

    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool_before_change(*plater, object_id, object->volumes.size(), closed_tool))
        return *refusal;
    McpDialogSuppressionGuard guard;
    ScopedKeepPainting        painting(keep.value);
    nlohmann::json            answer;
    if (target == SplitTarget::objects) {
        const bool kept = keep_height.value_or(true);
        guard.answer_prompt(MCP_PROMPT_SPLIT_FLOATING, kept ? wxID_YES : wxID_NO,
                            kept ? "pass keep_height: false to drop the pieces onto the bed" : "pass keep_height: true to keep each piece at its height");
        answer = split_to_objects(*plater, object_id, *object, kept, guard);
    } else {
        answer = split_to_parts(*plater, object_id, *object, *split_volume_of(*object, volume_id), guard);
    }
    if (answer.value("status", "") == "success") {
        add_keep_painting(answer, keep);
        answer["active_warnings"] = get_active_warnings_json(plater);
    }
    return with_closed_tool(guard.report(std::move(answer)), closed_tool);
}

// ---- add_volume ----

// The STEP tessellation the object list's part loader uses (ObjectList::load_modifier): the app's
// settings, with its fallbacks.
StepDeflection step_deflection()
{
    StepDeflection step;
    const double linear = string_to_double_decimal_point(wxGetApp().app_config->get("linear_deflection"));
    const double angle  = string_to_double_decimal_point(wxGetApp().app_config->get("angle_deflection"));
    step.linear         = linear > 0 ? linear : 0.003;
    step.angle          = angle > 0 ? angle : 0.5;
    step.split_compound = wxGetApp().app_config->get_bool("is_split_compound");
    return step;
}

const std::vector<std::pair<std::string, std::string>>& primitive_shapes()
{
    // The object list's Add Part / Modifier / ... submenu (MenuFactory::append_submenu_add_generic), by the
    // names ObjectList::load_generic_subobject takes.
    static const std::vector<std::pair<std::string, std::string>> shapes = {
        {"cube", "Cube"}, {"cylinder", "Cylinder"}, {"sphere", "Sphere"}, {"cone", "Cone"}, {"disc", "Disc"}, {"torus", "Torus"}};
    return shapes;
}

std::vector<std::string> primitive_shape_names()
{
    std::vector<std::string> names;
    for (const auto& [name, menu_name] : primitive_shapes())
        names.push_back(name);
    return names;
}

// Whether the instance's rotation, scale or mirror is the identity: what add_volume's primitive moves
// into the volumes (ObjectList::apply_object_instance_transfrom_to_all_volumes).
bool has_turn_or_scale(const Geometry::Transformation& transformation)
{
    return !transformation.get_matrix_no_offset().isApprox(Transform3d::Identity());
}

// The object list's tip after adding a modifier tells a user to switch the sidebar to object mode; under
// MCP it is answered like any unhandled dialog, and says nothing an agent can use.
void drop_modifier_tip(nlohmann::json& answer)
{
    if (!answer.contains("info_messages"))
        return;
    const std::string tip  = mcp_unhandled_modal(true, into_u8(_L("Add Modifier"))).message;
    nlohmann::json&   said = answer["info_messages"];
    said.erase(std::remove(said.begin(), said.end(), nlohmann::json(tip)), said.end());
    if (said.empty())
        answer.erase("info_messages");
}

nlohmann::json add_volume_on_main_thread(const nlohmann::json& params)
{
    Plater*      plater    = wxGetApp().plater();
    int          object_id = -1;
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), object_id, error);
    if (object == nullptr)
        return error_response(error);
    const std::string                    type_name = string_arg(params, "type", error);
    const std::optional<ModelVolumeType> type      = volume_type_from_name(type_name);
    if (!type)
        return error_response("type must be one of: part, negative_volume, modifier, support_blocker, support_enforcer");
    const std::optional<std::string> shape = error.empty() ? read_text(params, "shape", error) : std::nullopt;
    const std::optional<std::string> file  = error.empty() ? read_text(params, "file_path", error) : std::nullopt;
    if (!error.empty())
        return error_response(error);
    const bool has_shape = shape.has_value();
    const bool has_file  = file.has_value();
    if (has_shape == has_file)
        return error_response("give exactly one of shape (a primitive: cube, cylinder, sphere, cone, disc, torus) or file_path (a model file)");
    std::string menu_shape, file_path;
    if (has_shape) {
        for (const auto& [name, menu_name] : primitive_shapes())
            if (name == *shape)
                menu_shape = menu_name;
        if (menu_shape.empty())
            return error_response("shape must be one of: cube, cylinder, sphere, cone, disc, torus");
    } else {
        file_path = *file;
        boost::system::error_code ec;
        if (file_path.empty() || !boost::filesystem::is_regular_file(file_path, ec))
            return error_response("file_path " + file_path + " is not a file that can be read");
        // The object list's Load... records its undo step before it reads the file: read it first, so a file
        // the app cannot read changes nothing.
        if (const auto unreadable = model_file_load_error(file_path, step_deflection()))
            return error_response(*unreadable);
    }
    if (const auto refusal = ui_job_refusal(*plater, "add_volume"))
        return error_response(*refusal);

    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool_before_change(*plater, object_id, object->volumes.size(), closed_tool))
        return *refusal;
    if (const auto refusal = select_in_object_list(*plater, object_id))
        return with_closed_tool(error_response(*refusal), closed_tool);
    McpDialogSuppressionGuard guard;
    if (has_file)
        guard.answer_file(file_path); // the object list's Load... asks for the file
    const std::set<const ModelVolume*> before  = volume_set(*object);
    const bool                         turned  = has_turn_or_scale(object->instances[0]->get_transformation());
    ObjectList*                        list    = wxGetApp().obj_list();
    if (has_shape)
        list->load_generic_subobject(menu_shape, *type);
    else
        list->load_subobject(*type);
    const std::vector<int> added = volumes_not_in(*object, before);
    if (added.empty())
        return with_closed_tool(guard.fail_on_errors(error_response("Nothing was added to object " + std::to_string(object_id))), closed_tool);

    const int      volume_id = added.front();
    nlohmann::json answer    = {{"status", "success"},
                                {"object_id", object_id},
                                {"volume", volume_row_json(*object, std::size_t(volume_id))},
                                {"volumes", volume_rows_json(*object)}};
    if (has_shape && turned && !has_turn_or_scale(object->instances[0]->get_transformation()))
        answer["instance_transform_moved_to_volumes"] =
            "the object's rotation and scale moved from its instance into its volumes, as the object list does when it adds a "
            "primitive: rotation_degrees and scale now read 0 and 1, and the object looks the same";
    report_placement(answer, object_id);
    answer["active_warnings"] = get_active_warnings_json(plater);
    add_next_steps(answer, new_volume_next_steps(object_id, volume_id, type_name, has_shape));
    answer = guard.report(std::move(answer));
    drop_modifier_tip(answer);
    return with_closed_tool(std::move(answer), closed_tool);
}

// ---- set_volume_type ----

nlohmann::json set_volume_type_on_main_thread(const nlohmann::json& params)
{
    Plater*      plater    = wxGetApp().plater();
    int          object_id = -1;
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), object_id, error);
    if (object == nullptr)
        return error_response(error);
    const std::optional<int> volume_id = read_volume_id(params, error);
    if (!error.empty() || !volume_id)
        return error_response(error.empty() ? "volume_id is required: the volume whose type changes" : error);
    if (const auto volume_error = volume_id_error(object_id, *object, *volume_id))
        return error_response(*volume_error);
    const std::string                    type_name = string_arg(params, "type", error);
    const std::optional<ModelVolumeType> to        = volume_type_from_name(type_name);
    if (!to)
        return error_response("type must be one of: part, negative_volume, modifier, support_blocker, support_enforcer");
    ModelVolume*      target        = object->volumes[std::size_t(*volume_id)];
    const std::string previous_type = volume_type_name(target->type());
    nlohmann::json    answer        = {{"status", "success"}, {"object_id", object_id}, {"previous_volume_id", *volume_id},
                                       {"previous_type", previous_type}};
    if (target->type() == *to) {
        answer.update({{"volume_id", *volume_id}, {"type", type_name}, {"changed", false}, {"volumes", volume_rows_json(*object)}});
        return answer;
    }
    if (const auto refusal = volume_type_change_refusal(object_id, *object, *volume_id, *to, cut_siblings(plater->model(), object_id)))
        return error_response(*refusal);
    if (const auto refusal = ui_job_refusal(*plater, "set_volume_type"))
        return error_response(*refusal);

    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool_before_change(*plater, object_id, object->volumes.size(), closed_tool))
        return *refusal;
    if (const auto refusal = select_in_object_list(*plater, object_id, target))
        return with_closed_tool(error_response(*refusal), closed_tool);
    McpDialogSuppressionGuard guard;
    wxGetApp().obj_list()->set_volume_type(*to);
    if (target->type() != *to)
        return with_closed_tool(guard.fail_on_errors(error_response("The object list did not change the type of volume " +
                                                                    std::to_string(*volume_id) + " of object " + std::to_string(object_id))),
                                closed_tool);
    // What prints changed: the plates holding the object no longer have its result.
    mark_object_plates_unsliced(plater->get_partplate_list(), object_id);
    // The list sorts volumes by type (parts first), so the volume's index can change.
    const int now = index_of(*object, target);
    answer.update({{"volume_id", now}, {"type", type_name}, {"changed", true}, {"volumes", volume_rows_json(*object)}});
    report_placement(answer, object_id);
    answer["active_warnings"] = get_active_warnings_json(plater);
    add_next_steps(answer, new_volume_next_steps(object_id, now, type_name, false));
    return with_closed_tool(guard.report(std::move(answer)), closed_tool);
}

// ---- assemble_objects ----

nlohmann::json assemble_objects_on_main_thread(const nlohmann::json& params)
{
    Plater* plater = wxGetApp().plater();
    Model&  model  = plater->model();
    if (!params.contains("object_ids") || !params.at("object_ids").is_array())
        return error_response("object_ids must be a list of the objects to assemble, by object_id");
    std::vector<int> object_ids;
    for (const nlohmann::json& given : params.at("object_ids")) {
        int id = -1;
        if (!parse_integer_param(given, id))
            return error_response("object_ids must be whole numbers, as get_scene_info gives object_id; got " + given.dump());
        object_ids.push_back(id);
    }
    if (const auto refusal = assemble_refusal(model, object_ids))
        return error_response(*refusal);
    if (const auto refusal = ui_job_refusal(*plater, "assemble_objects"))
        return error_response(*refusal);

    nlohmann::json from = nlohmann::json::array();
    for (int id : object_ids) {
        const ModelObject& object = *model.objects[std::size_t(id)];
        from.push_back({{"object_id", id},
                        {"name", object.name},
                        {"volumes", int(object.volumes.size())},
                        // The object list's Assemble takes each object's first instance.
                        {"instances_left_out", int(object.instances.size()) - 1}});
    }
    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool_before_change(*plater, -1, 0, closed_tool))
        return *refusal;
    if (const auto refusal = select_objects_in_object_list(*plater, object_ids))
        return with_closed_tool(error_response(*refusal), closed_tool);
    ObjectList* list = wxGetApp().obj_list();
    if (!list->can_merge_to_multipart_object())
        return with_closed_tool(error_response("the object list does not assemble this selection, so nothing was changed"), closed_tool);
    McpDialogSuppressionGuard guard;
    const std::size_t         before = model.objects.size();
    list->merge(true);
    if (model.objects.size() != before - object_ids.size() + 1)
        return with_closed_tool(guard.fail_on_errors(error_response("The object list did not assemble the objects")), closed_tool);

    const int          assembled = int(model.objects.size()) - 1;
    const ModelObject& object    = *model.objects[std::size_t(assembled)];
    nlohmann::json     answer    = {{"status", "success"},
                                    {"object_id", assembled},
                                    {"object_name", object.name},
                                    {"assembled_from", from},
                                    {"volumes", volume_rows_json(object)},
                                    {"object_ids_shifted", "the assembled objects are gone and the objects after each moved down; the "
                                                           "assembly is the last object, " + std::to_string(assembled)}};
    report_placement(answer, assembled);
    answer["active_warnings"] = get_active_warnings_json(plater);
    add_next_steps(answer, assembled_next_steps(assembled));
    return with_closed_tool(guard.report(std::move(answer)), closed_tool);
}

// ---- merge_parts ----

nlohmann::json merge_parts_on_main_thread(const nlohmann::json& params)
{
    Plater*      plater    = wxGetApp().plater();
    Model&       model     = plater->model();
    int          object_id = -1;
    std::string  error;
    ModelObject* object = resolve_object_id(params, model, object_id, error);
    if (object == nullptr)
        return error_response(error);
    const KeepPainting keep = read_keep_painting(params, error);
    if (!error.empty())
        return error_response(error);
    if (const auto refusal = merge_parts_refusal(object_id, *object))
        return error_response(*refusal);
    if (const auto refusal = ui_job_refusal(*plater, "merge_parts"))
        return error_response(*refusal);

    // What the object list's Mesh boolean leaves out of the new object (ObjectList::boolean).
    std::vector<int>      merged, subtracted, left_out;
    std::set<std::string> part_settings; // the volumes' own settings, their filament (extruder) included
    for (std::size_t i = 0; i < object->volumes.size(); ++i) {
        const ModelVolume& volume = *object->volumes[i];
        (volume.is_model_part() ? merged : volume.is_negative_volume() ? subtracted : left_out).push_back(int(i));
        for (const std::string& key : volume.config.keys())
            part_settings.insert(key);
    }
    nlohmann::json not_carried = {{"volumes", rows_of(*object, left_out)},
                                  {"part_settings", std::vector<std::string>(part_settings.begin(), part_settings.end())},
                                  {"layer_ranges", int(object->layer_config_ranges.size())},
                                  {"brim_ears", int(object->brim_points.size())}};
    const int              instances = int(object->instances.size());
    const std::string      name      = object->name;
    const std::vector<int> siblings  = cut_siblings(model, object_id);

    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool_before_change(*plater, object_id, object->volumes.size(), closed_tool))
        return *refusal;
    if (const auto refusal = select_in_object_list(*plater, object_id))
        return with_closed_tool(error_response(*refusal), closed_tool);
    ObjectList* list = wxGetApp().obj_list();
    if (!list->can_mesh_boolean())
        return with_closed_tool(error_response("the object list does not merge this object's parts, so nothing was changed"), closed_tool);
    McpDialogSuppressionGuard guard;
    ScopedKeepPainting        painting(keep.value);
    const std::size_t         before = model.objects.size();
    list->boolean();
    if (model.objects.size() != before || model.objects.back() == object)
        return with_closed_tool(guard.fail_on_errors(error_response("The object list did not merge the parts of object " + std::to_string(object_id))),
                                closed_tool);

    const int          merged_id = int(model.objects.size()) - 1;
    const ModelObject& result    = *model.objects[std::size_t(merged_id)];
    nlohmann::json     answer    = {{"status", "success"},
                                    {"object_id", merged_id},
                                    {"previous_object_id", object_id},
                                    {"object_name", result.name},
                                    {"merged_volumes", merged},
                                    {"subtracted_volumes", subtracted},
                                    {"not_carried", not_carried},
                                    {"volumes", volume_rows_json(result)}};
    if (instances > 1)
        answer["instances_merged"] = "the object's " + std::to_string(instances) +
                                     " instances are one mesh now, in one instance, as the object list's Mesh boolean makes them";
    if (merged_id != object_id)
        answer["object_ids_shifted"] = "object " + std::to_string(object_id) + " is gone and every object after it moved down by one; the "
                                       "merged object is the last, " + std::to_string(merged_id);
    std::vector<int> invalidated;
    for (int sibling : siblings)
        if (sibling != object_id)
            invalidated.push_back(sibling > object_id ? sibling - 1 : sibling);
    if (!invalidated.empty())
        answer["cut_info_invalidated_for"] = invalidated;
    // A failed boolean leaves a notification and the parts side by side in one mesh (Plater::combine_mesh_fff).
    const nlohmann::json warnings = get_active_warnings_json(plater);
    const std::string    failed   = _u8L("Unable to perform boolean operation on model meshes.");
    answer["boolean"]             = "union";
    for (const nlohmann::json& warning : warnings["warnings"])
        if (warning.value("message", "").find(failed) != std::string::npos)
            answer["boolean"] = "failed: the parts are joined into one mesh without a union (" + warning.value("message", "") + ")";
    add_keep_painting(answer, keep);
    report_placement(answer, merged_id);
    answer["active_warnings"] = warnings;
    add_next_steps(answer, mesh_next_steps(model, {merged_id}, model_mesh_health(model)));
    return with_closed_tool(guard.report(std::move(answer)), closed_tool);
}

// ---- invalidate_cut_info ----

nlohmann::json invalidate_cut_info_on_main_thread(const nlohmann::json& params)
{
    Plater*      plater    = wxGetApp().plater();
    int          object_id = -1;
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), object_id, error);
    if (object == nullptr)
        return error_response(error);
    const std::vector<int> siblings = cut_siblings(plater->model(), object_id);
    if (siblings.empty())
        return {{"status", "success"},
                {"object_id", object_id},
                {"changed", false},
                {"message", "Object " + std::to_string(object_id) + " is not part of a cut: there is nothing to invalidate"}};
    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool_before_change(*plater, object_id, object->volumes.size(), closed_tool))
        return *refusal;
    McpDialogSuppressionGuard guard;
    wxGetApp().obj_list()->invalidate_cut_info_for_object(object_id);
    nlohmann::json answer = {{"status", "success"}, {"object_id", object_id}, {"objects", siblings}, {"changed", true}};
    answer["active_warnings"] = get_active_warnings_json(plater);
    return with_closed_tool(guard.report(std::move(answer)), closed_tool);
}

nlohmann::json keep_painting_schema()
{
    return {{"type", "boolean"},
            {"description", "Remap the painting (colour, supports, seams, fuzzy skin) onto the new meshes instead of clearing it; the "
                            "app calls this experimental. Left out: the app's \"Keep painted feature after mesh change\" setting, "
                            "which the answer's keep_painting_from names"}};
}

} // namespace

void Slic3r::GUI::OrcaMCPServer::register_part_tools()
{
    register_tool({
        "split_object",
        ToolCategory::Models,
        "Split an object into objects or parts",
        "Split an object as the object list's Split does. to: \"objects\" makes one object per solid part of a multi-part "
        "object, or per shell of a one-part object; its modifiers, negative volumes and support volumes are not carried "
        "(dropped_volumes lists them), and each piece keeps its place. When a piece would float and the object drops to the "
        "bed, keep_height (default true) keeps each piece at its height, false drops them onto the bed. The pieces are the "
        "last objects afterwards and every object after the split one moves down by one. to: \"parts\" splits one volume "
        "into one part per shell (name it with volume_id in a multi-part object); the pieces are named <name>_1, _2, ..., "
        "keep its filament, and stay in place. Painting is cleared unless keep_painting keeps it. One undo step. An open "
        "toolbar tool is closed first (closed_toolbar_tool). Refused while an arrange, orient or bed fill runs.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
                {"to", {{"type", "string"}, {"enum", {"objects", "parts"}},
                        {"description", "objects: one object per solid part, or per shell of a one-part object; parts: one part per "
                                        "shell of a volume"}}},
                {"volume_id", {{"type", "integer"}, {"minimum", 0},
                               {"description", "to parts: the volume to split, as get_object_info lists them; needed when the object "
                                               "has more than one volume"}}},
                {"keep_height", {{"type", "boolean"},
                                 {"description", "to objects: keep a floating piece at its height (default true) or drop it onto the "
                                                 "bed (false), the app's own question about floating pieces"}}},
                {"keep_painting", keep_painting_schema()}
            }},
            {"required", {"object_id", "to"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([&params]() -> nlohmann::json { return split_object_on_main_thread(params); });
        }
    });

    register_tool({
        "add_volume",
        ToolCategory::Models,
        "Add a part, modifier or support volume",
        "Add a volume to an object, as the object list's Add Part / Negative Part / Modifier / Support Blocker / Support "
        "Enforcer does: a primitive (shape) or a model file (file_path). A primitive is sized a tenth of the bed's longest "
        "side and stands beside the object at its right-front corner, on the bed: move_object and scale_object with "
        "volume_id place and size it. As the object list does, adding a primitive moves the object's rotation and scale "
        "from its instance into its volumes (instance_transform_moved_to_volumes says when). A file's volume keeps the "
        "position its file gives it. A modifier changes only the settings set_object_config with volume_id gives it. The "
        "answer: the new volume (volume_id, box and position in plate mm) and every volume. One undo step. An open toolbar "
        "tool is closed first (closed_toolbar_tool). Refused while an arrange, orient or bed fill runs.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
                {"type", {{"type", "string"}, {"enum", volume_type_names()}, {"description", "What the volume is"}}},
                {"shape", {{"type", "string"}, {"enum", primitive_shape_names()},
                           {"description", "A primitive, as the object list's submenu offers them; or give file_path"}}},
                {"file_path", {{"type", "string"},
                               {"description", "A model file (STL, 3MF, STEP, OBJ, ...) whose meshes become the volume; or give shape"}}}
            }},
            {"required", {"object_id", "type"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([&params]() -> nlohmann::json { return add_volume_on_main_thread(params); });
        }
    });

    register_tool({
        "set_volume_type",
        ToolCategory::Models,
        "Change a volume's type: part, modifier",
        "Change a volume's type, as the object list's Change Type does: part, negative_volume, modifier, support_blocker or "
        "support_enforcer. The object's last solid part stays a part, and a text or SVG volume never becomes a support "
        "volume. The list keeps volumes sorted by type, so the volume's index can change: volume_id in the answer is the new "
        "one, previous_volume_id the old. The same type changes nothing. One undo step. An open toolbar tool is closed first "
        "(closed_toolbar_tool). Refused while an arrange, orient or bed fill runs.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
                {"volume_id", {{"type", "integer"}, {"minimum", 0}, {"description", "The volume, as get_object_info lists them"}}},
                {"type", {{"type", "string"}, {"enum", volume_type_names()}, {"description", "Its new type"}}}
            }},
            {"required", {"object_id", "volume_id", "type"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([&params]() -> nlohmann::json { return set_volume_type_on_main_thread(params); });
        }
    });

    register_tool({
        "assemble_objects",
        ToolCategory::Models,
        "Assemble objects into one multi-part",
        "Assemble two or more objects into one object with their volumes as its parts, as the object list's Assemble does: "
        "each keeps its place, settings and filament; the new object, named Assembly, takes each object's first instance "
        "(assembled_from says how many others were left out). The assembly is the last object afterwards, and the objects "
        "after each assembled one move down. A piece of a cut is refused until invalidate_cut_info. One undo step. An open "
        "toolbar tool is closed first (closed_toolbar_tool). Refused while an arrange, orient or bed fill runs.",
        {
            {"type", "object"},
            {"properties", {
                {"object_ids", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}}},
                                {"description", "Two or more objects, by object_id"}}}
            }},
            {"required", {"object_ids"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([&params]() -> nlohmann::json { return assemble_objects_on_main_thread(params); });
        }
    });

    register_tool({
        "merge_parts",
        ToolCategory::Models,
        "Merge an object's parts into one part",
        "Merge an object's parts into one part, as the object list's Mesh boolean does: its solid parts are joined by a "
        "union and its negative volumes subtracted; its modifiers and support volumes, its parts' own settings and "
        "filaments, its layer ranges and brim ears are not carried (not_carried lists them); its name and object settings "
        "are. Every instance of the object becomes part of one mesh in one instance (instances_merged says so). When the "
        "union fails, the parts are joined without it (boolean says so). The merged object is the last object afterwards. "
        "Painting is cleared unless keep_painting keeps it. One undo step. An open toolbar tool is closed first "
        "(closed_toolbar_tool). Refused while an arrange, orient or bed fill runs.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}},
                {"keep_painting", keep_painting_schema()}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([&params]() -> nlohmann::json { return merge_parts_on_main_thread(params); });
        }
    });

    register_tool({
        "invalidate_cut_info",
        ToolCategory::Models,
        "End a cut's link between its pieces",
        "End the link the app keeps between the pieces of a cut, as the object list's Invalidate cut info does: until then "
        "a piece's solid parts and connectors cannot be moved or deleted on their own, and its pieces cannot be assembled. "
        "It ends it for every piece of that cut (objects lists them). An object that is not part of a cut changes nothing. "
        "One undo step.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based): any piece of the cut"}}}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return run_on_main_thread([&params]() -> nlohmann::json { return invalidate_cut_info_on_main_thread(params); });
        }
    });
}
