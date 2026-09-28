// src/slic3r/GUI/OrcaMCP/OrcaMCPArrangeTools.cpp
#include "OrcaMCPArrangeTools.hpp"

#include "OrcaMCPArrangeOptions.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPInstanceBox.hpp"
#include "OrcaMCPInstanceEdits.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPPlateSettings.hpp"
#include "OrcaMCPPlateUtils.hpp"
#include "OrcaMCPServer.hpp"
#include "OrcaMCPUiJob.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/format.hpp>

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

// ---- Reading a call ----

std::string locked_plate_refusal(int plate_index)
{
    return "Plate " + std::to_string(plate_index) + " is locked, and an arrange leaves a locked plate's objects where they are: unlock it "
           "with set_plate_settings {\"plate_index\": " + std::to_string(plate_index) + ", \"locked\": false}, then call arrange_objects again";
}

// ---- The arrange menu's options, where the app keeps them ----

// The option set the menu edits (GLCanvas3D::get_arrange_settings).
ArrangeMode arrange_mode(Plater& plater)
{
    if (plater.printer_technology() == ptSLA)
        return ArrangeMode::sla;
    return wxGetApp().global_print_sequence() == PrintSequence::ByObject ? ArrangeMode::by_object : ArrangeMode::by_layer;
}

// The menu shows the calibration-area option only on a Bambu Lab printer that scans its first layer.
bool calibration_region_offered()
{
    PresetBundle&             bundle = *wxGetApp().preset_bundle;
    const ConfigOption* const scan   = bundle.printers.get_edited_preset().config.option("scan_first_layer");
    return bundle.is_bbl_vendor() && scan != nullptr && scan->getBool();
}

// The options the menu's Reset gives: align to Y as the printer's structure has it.
ArrangeOptions reset_options()
{
    ArrangeOptions defaults;
    const auto* structure = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<PrinterStructure>>(
        "printer_structure");
    defaults.align_to_y_axis = structure != nullptr && structure->value == PrinterStructure::psI3;
    return defaults;
}

// The menu's options: the 3D view's, where the menu is.
GLCanvas3D::ArrangeSettings& menu_settings(Plater& plater) { return plater.get_view3D_canvas3D()->get_arrange_settings(); }

ArrangeOptions options_of(const GLCanvas3D::ArrangeSettings& settings)
{
    ArrangeOptions options;
    options.spacing_mm               = settings.distance;
    options.auto_rotate              = settings.enable_rotation;
    options.allow_multiple_materials = settings.allow_multi_materials_on_same_plate;
    options.avoid_calibration_region = settings.avoid_extrusion_cali_region;
    options.align_to_y_axis          = settings.align_to_y_axis;
    return options;
}

// The request against the app's options, before anything is saved.
struct OptionsChange
{
    ArrangeOptionsRequest request;
    ArrangeOptions        before;
    ArrangeOptions        after;
    ArrangeMode           mode    = ArrangeMode::by_layer;
    bool                  offered = false;
};

// The options as the request leaves them, or why it cannot.
std::optional<std::string> plan_options(Plater& plater, const ArrangeOptionsRequest& request, OptionsChange& change)
{
    change.request = request;
    change.mode    = arrange_mode(plater);
    change.offered = calibration_region_offered();
    change.before  = options_of(menu_settings(plater));
    change.after   = change.before;
    return apply_arrange_options(request, reset_options(), change.offered, change.after);
}

// Saved as the menu saves them (GLCanvas3D::_render_arrange_menu): its Reset erases the keys, and each
// option given is written under its key. Align to Y has none: the menu keeps it in memory.
void save_to_app_config(const OptionsChange& change)
{
    AppConfig&                   config  = *wxGetApp().app_config;
    const ArrangeOptionKeys      keys    = arrange_option_keys(change.mode);
    const ArrangeOptionsRequest& request = change.request;
    if (request.reset)
        for (const std::string* key : {&keys.spacing, &keys.auto_rotate, &keys.allow_multiple_materials, &keys.avoid_calibration_region})
            config.erase("arrange", *key);
    if (request.spacing_mm)
        config.set("arrange", keys.spacing, float_to_string_decimal_point(float(change.after.spacing_mm)));
    if (request.auto_rotate)
        config.set("arrange", keys.auto_rotate, change.after.auto_rotate);
    if (request.allow_multiple_materials)
        config.set("arrange", keys.allow_multiple_materials, change.after.allow_multiple_materials);
    if (request.avoid_calibration_region)
        config.set("arrange", keys.avoid_calibration_region, change.after.avoid_calibration_region);
}

// The options into the menu's settings, and into every canvas's: the job reads the settings of the
// canvas showing (init_arrange_params reads Plater::canvas3D()), and a tool arranges from any tab.
void apply_to_canvases(Plater& plater, const ArrangeOptions& options)
{
    GLCanvas3D::ArrangeSettings& settings        = menu_settings(plater);
    settings.distance                            = float(options.spacing_mm);
    settings.enable_rotation                     = options.auto_rotate;
    settings.allow_multi_materials_on_same_plate = options.allow_multiple_materials;
    settings.avoid_extrusion_cali_region         = options.avoid_calibration_region;
    settings.align_to_y_axis                     = options.align_to_y_axis;
    for (GLCanvas3D* canvas : {plater.get_preview_canvas3D(), plater.get_assmeble_canvas3D()})
        if (canvas != nullptr && canvas != plater.get_view3D_canvas3D())
            canvas->get_arrange_settings() = settings;
}

void save_options(Plater& plater, const OptionsChange& change)
{
    save_to_app_config(change);
    apply_to_canvases(plater, change.after);
}

void add_options_report(nlohmann::json& answer, const OptionsChange& change)
{
    answer["arrange_options"] = arrange_options_json(change.after, change.mode, change.offered);
    answer["options_changed"] = changed_arrange_options(change.before, change.after);
}

// ---- Plates ----

// The plates the A key's arrange leaves as they are (ArrangeJob::prepare_all): locked ones, and those
// whose own print sequence differs from the global one, which it locks while it runs.
nlohmann::json plates_not_arranged(GUI::PartPlateList& plates)
{
    nlohmann::json skipped = nlohmann::json::array();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        PartPlate* plate = plates.get_plate(i);
        bool       same  = true;
        plate->get_real_print_seq(&same);
        if (plate->is_locked())
            skipped.push_back({{"plate_index", i}, {"reason", "locked"}});
        else if (!same)
            skipped.push_back({{"plate_index", i}, {"reason", "print_sequence_differs"}});
    }
    return skipped;
}

// Why the plate's arrange spaces its objects automatically whatever the spacing (init_arrange_params: a
// plate whose own print sequence differs from the global one), or nothing.
std::optional<std::string> automatic_spacing_note(PartPlate& plate, const ArrangeOptions& options)
{
    bool same = true;
    plate.get_real_print_seq(&same);
    if (same || options.spacing_mm == 0.)
        return std::nullopt;
    return std::string("this plate's own print sequence differs from the global one, so its arrange spaces objects automatically, "
                       "as the plate's Arrange does; spacing_mm applies to plates that follow the global sequence");
}

nlohmann::json placements_after(const std::vector<ObjectTransforms>& scope)
{
    nlohmann::json objects = nlohmann::json::array();
    for (const ObjectTransforms& before : scope)
        objects.push_back(placement_after_job(before));
    return objects;
}

// ---- Selecting as a click does ----

// Selects instance `instance_id` of object `object_id` alone, as a click on it in the 3D view does, for
// the GUI's instance actions, which act on the selection. Not an undo step. Why it could not, or nothing.
std::optional<std::string> select_instance(Plater& plater, int object_id, int instance_id)
{
    Plater::SuppressSnapshots not_an_edit(&plater);
    bool                      scene_current = false;
    GLCanvas3D*               view          = OrcaMCPPlateUtils::SceneCanvas(scene_current);
    if (view == nullptr)
        return std::string("the 3D view is not available, so nothing can be selected");
    if (instance_id < 0)
        view->get_selection().add_object(unsigned(object_id), /*as_single_selection=*/true);
    else
        view->get_selection().add_instance(unsigned(object_id), unsigned(instance_id), /*as_single_selection=*/true);
    wxGetApp().obj_list()->update_selections();
    if (plater.get_selected_object_idx() == object_id)
        return std::nullopt;
    return "the app could not select object " + std::to_string(object_id) + " in its 3D view (" +
           (scene_current ? "the view did not take the selection" : "the 3D view could not be brought up to date") + "), so nothing was changed";
}

// Closes an open toolbar tool right before a change, as the user closes it (close_open_toolbar_tool). An
// error answer when it did not close.
std::optional<nlohmann::json> close_toolbar_tool(Plater& plater, std::string& closed_tool)
{
    closed_tool = close_open_toolbar_tool(plater);
    if (closed_tool.empty() || !toolbar_tool_open(plater))
        return std::nullopt;
    return with_closed_tool(error_response("The toolbar tool " + closed_tool + " is open in the app and did not close, so nothing was changed"),
                            closed_tool);
}

// ---- set_instance_count ----

// The instances from `first_added` on that overlap another instance on their plate, those partly off
// their plate, those on no plate, and the plate to arrange for the first two.
struct AddedInstances
{
    std::vector<int> crowded;
    std::vector<int> partly_off;
    std::vector<int> on_no_plate;
    int              plate_index = -1;
};

std::vector<BoundingBoxf3> instance_boxes_on(const Model& model, PartPlate& plate, int object_id, int instance_id, int& position)
{
    std::vector<BoundingBoxf3> boxes;
    for (std::size_t o = 0; o < model.objects.size(); ++o)
        for (std::size_t i = 0; i < model.objects[o]->instances.size(); ++i)
            if (plate.contain_instance(int(o), int(i))) {
                if (int(o) == object_id && int(i) == instance_id)
                    position = int(boxes.size());
                boxes.push_back(instance_box(*model.objects[o], i));
            }
    return boxes;
}

AddedInstances added_instances(Plater& plater, int object_id, std::size_t first_added)
{
    AddedInstances                       added;
    const Model&                         model      = plater.model();
    GUI::PartPlateList&                  plates     = plater.get_partplate_list();
    const ModelObject&                   object     = *model.objects[std::size_t(object_id)];
    const std::vector<InstancePlacement> placements = instance_placements(object, object_id, plates);
    for (std::size_t i = first_added; i < placements.size(); ++i) {
        const int plate_index = placements[i].plate_index;
        if (plate_index < 0) {
            added.on_no_plate.push_back(int(i));
            continue;
        }
        if (added.plate_index < 0)
            added.plate_index = plate_index;
        int position = -1;
        if (!placements[i].on_bed)
            added.partly_off.push_back(int(i));
        else if (!overlapping_boxes(instance_boxes_on(model, *plates.get_plate(plate_index), object_id, int(i), position), {position}).empty())
            added.crowded.push_back(int(i));
    }
    return added;
}

// The instances of the object that stand on no plate, as its placement reports them.
std::vector<int> instances_on_no_plate(const nlohmann::json& placement)
{
    std::vector<int> ids;
    for (const nlohmann::json& instance : placement.value("instance_placement", nlohmann::json::array()))
        if (instance.at("plate_index").is_null())
            ids.push_back(instance.at("instance_id").get<int>());
    return ids;
}

std::vector<int> ids_between(std::size_t first, std::size_t end)
{
    std::vector<int> ids;
    for (std::size_t i = first; i < end; ++i)
        ids.push_back(int(i));
    return ids;
}

nlohmann::json set_instance_count_on_main_thread(const nlohmann::json& params, int count)
{
    Plater*      plater    = wxGetApp().plater();
    int          object_id = -1;
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), object_id, error);
    if (object == nullptr)
        return error_response(error);
    if (!plater->get_ui_job_worker().is_idle())
        return error_response(ui_job_busy_message("set_instance_count"));
    if (const auto refusal = instance_edit_refusal(object_id, *object, cut_siblings(plater->model(), object_id), "set_instance_count"))
        return error_response(*refusal);

    const std::size_t before = object->instances.size();
    nlohmann::json    answer = {{"status", "success"},
                                {"object_id", object_id},
                                {"object_name", object->name},
                                {"instance_count_before", before},
                                {"instance_count", before},
                                {"changed", false}};
    if (int(before) == count) {
        report_placement(answer, object_id);
        answer["active_warnings"] = get_active_warnings_json(plater);
        return answer;
    }

    std::string closed_tool;
    if (const auto refusal = close_toolbar_tool(*plater, closed_tool))
        return *refusal;
    if (const auto refusal = select_instance(*plater, object_id, -1))
        return with_closed_tool(error_response(*refusal), closed_tool);
    McpDialogSuppressionGuard guard;
    {
        // "Set number of instances" without its number dialog (Plater::set_number_of_copies): one undo step,
        // the Add or Remove instance inside it taking none.
        Plater::TakeSnapshot snapshot(plater, (boost::format("Set numbers of copies to %1%") % count).str());
        if (count > int(before))
            plater->increase_instances(std::size_t(count) - before);
        else
            plater->decrease_instances(before - std::size_t(count));
    }
    const std::size_t after = object->instances.size();
    if (after != std::size_t(count))
        return with_closed_tool(guard.fail_on_errors(error_response("The app set object " + std::to_string(object_id) + " to " +
                                                                    std::to_string(after) + " instances, not " + std::to_string(count))),
                                closed_tool);

    answer["instance_count"] = after;
    answer["changed"]        = true;
    if (after > before)
        answer["added_instance_ids"] = ids_between(before, after);
    else
        answer["removed_instance_ids"] = ids_between(after, before);
    report_placement(answer, object_id);
    if (after > before) {
        const AddedInstances added = added_instances(*plater, object_id, before);
        add_next_steps(answer, added_instances_next_steps(object_id, added.plate_index, added.crowded, added.partly_off, added.on_no_plate));
    }
    answer["active_warnings"] = get_active_warnings_json(plater);
    return with_closed_tool(guard.report(std::move(answer)), closed_tool);
}

nlohmann::json set_instance_count(const nlohmann::json& params)
{
    std::string error;
    const bool  include_preview = read_flag(params, "include_preview", error).value_or(false);
    const auto  count           = read_instance_count(params, error);
    if (!count || !error.empty())
        return error_response(error);
    return run_on_main_thread([&]() -> nlohmann::json {
        nlohmann::json answer = set_instance_count_on_main_thread(params, *count);
        if (answer.value("status", "") == "success")
            add_turntable_preview_if_requested(answer, include_preview);
        return answer;
    });
}

// ---- fill_bed_with_instances ----

struct FillBedStart
{
    std::shared_ptr<UiJobOutcome> outcome;
    std::vector<ObjectTransforms> scope;
    std::vector<ObjectID>         order; // the objects' order before the job, which the arrange re-sorts
    ObjectTransforms              filled;
    int                           plate_index = -1;
    OptionsChange                 options;
    std::string                   closed_tool;
};

nlohmann::json start_fill_bed(const nlohmann::json& params, const ArrangeOptionsRequest& request, std::optional<int> instance_arg,
                              FillBedStart& start)
{
    Plater*      plater    = wxGetApp().plater();
    int          object_id = -1;
    std::string  error;
    ModelObject* object = resolve_object_id(params, plater->model(), object_id, error);
    if (object == nullptr)
        return error_response(error);
    const int instance_id = instance_arg.value_or(0);
    if (const auto refusal = instance_id_error(object_id, *object, instance_id))
        return error_response(*refusal);
    if (!plater->get_ui_job_worker().is_idle())
        return error_response(ui_job_busy_message("fill_bed_with_instances"));
    if (const auto refusal = instance_edit_refusal(object_id, *object, cut_siblings(plater->model(), object_id), "fill_bed_with_instances"))
        return error_response(*refusal);
    GUI::PartPlateList& plates = plater->get_partplate_list();
    start.plate_index     = plates.find_instance(object_id, instance_id);
    if (start.plate_index < 0)
        return error_response("Instance " + std::to_string(instance_id) + " of object " + std::to_string(object_id) +
                              " stands on no plate, and a fill fills the plate the instance stands on: move it onto one with move_object "
                              "first, or name another instance");
    if (const auto refusal = plan_options(*plater, request, start.options))
        return error_response(*refusal);
    if (const auto refusal = close_toolbar_tool(*plater, start.closed_tool))
        return *refusal;

    save_options(*plater, start.options);
    make_plate_current(*plater, start.plate_index);
    if (const auto refusal = select_instance(*plater, object_id, instance_id))
        return with_closed_tool(error_response(*refusal), start.closed_tool);
    start.scope   = current_plate_objects(*plater);
    start.order   = object_order(plater->model());
    start.filled  = transforms_of(*object);
    start.outcome = start_ui_job(*plater, UiJobKind::fill_bed, Job::PREPARE_STATE_MENU);
    return nullptr;
}

nlohmann::json fill_bed_with_instances(const nlohmann::json& params)
{
    std::string error;
    const bool  include_preview = read_flag(params, "include_preview", error).value_or(false);
    const auto  instance_id     = read_instance_id(params, error);
    const auto  request         = read_arrange_options(params, error);
    if (!error.empty())
        return error_response(error);
    FillBedStart         start;
    const nlohmann::json refusal = run_on_main_thread([&]() -> nlohmann::json { return start_fill_bed(params, *request, instance_id, start); });
    if (!refusal.is_null())
        return refusal;
    return answer_after_ui_job(
        *start.outcome,
        [start, include_preview]() -> nlohmann::json {
            const nlohmann::json filled = placement_after_job(start.filled);
            nlohmann::json       answer = {{"status", "success"},
                                           {"object_id", filled.value("object_id", nlohmann::json(nullptr))},
                                           {"plate_index", start.plate_index}};
            const std::size_t    now    = filled.contains("instance_placement") ? filled["instance_placement"].size() : 0;
            answer["instances_added"]   = int(now) - int(start.filled.instances.size());
            answer["instance_count"]    = now;
            // The fill's estimate can add more than its plate's arrange fits: those stand on no plate.
            const std::vector<int> unplaced = instances_on_no_plate(filled);
            answer["instances_on_no_plate"] = unplaced;
            if (filled.contains("object_id") && filled["object_id"].is_number())
                add_next_steps(answer, unplaced_instances_next_steps(filled["object_id"].get<int>(), unplaced, int(now)));
            answer["objects"]           = placements_after(start.scope);
            answer["object_id_changes"] = object_id_changes(start.order, wxGetApp().plater()->model());
            add_options_report(answer, start.options);
            answer["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
            add_turntable_preview_if_requested(answer, include_preview);
            return with_closed_tool(std::move(answer), start.closed_tool);
        },
        /*until_worker_idle=*/true);
}

} // namespace

namespace Slic3r { namespace GUI { namespace OrcaMCP {

nlohmann::json arrange_objects_properties()
{
    nlohmann::json properties = {
        {"include_preview", {{"type", "boolean"}, {"description", "Return turntable preview path, drawn once the arrange has been applied"}}},
        {"all_plates",
         {{"type", "boolean"},
          {"description", "Arrange every object on every plate that is not locked, as the A key and the arrange menu's Arrange do; it may add "
                          "plates, and moves unprintable objects to a plate after the last. Default false: one plate"}}},
        {"plate_index",
         {{"type", "integer"},
          {"minimum", 0},
          {"description", "The plate to arrange, made the current plate first as the plate's own Arrange icon does. Default: the current plate"}}}};
    properties.update(arrange_option_properties());
    return properties;
}

nlohmann::json arrange_objects(const nlohmann::json& params)
{
    std::string error;
    const bool  include_preview = read_flag(params, "include_preview", error).value_or(false);
    const bool  all_plates      = read_flag(params, "all_plates", error).value_or(false);
    const auto  plate_index     = read_plate_index(params, error);
    const auto  request         = read_arrange_options(params, error);
    if (!error.empty())
        return error_response(error);
    if (all_plates && plate_index)
        return error_response("give all_plates or plate_index, not both: all_plates arranges every plate");

    std::shared_ptr<UiJobOutcome> outcome;
    std::vector<ObjectTransforms> scope;
    std::vector<ObjectID>         order;
    OptionsChange                 options;
    nlohmann::json                facts;
    const nlohmann::json          refusal = run_on_main_thread([&]() -> nlohmann::json {
        Plater* plater = wxGetApp().plater();
        if (!plater->get_ui_job_worker().is_idle())
            return error_response(ui_job_busy_message("arrange_objects"));
        if (plater->model().objects.empty())
            return error_response("The scene has no objects to arrange");
        GUI::PartPlateList& plates = plater->get_partplate_list();
        const int      target = plate_index.value_or(plates.get_curr_plate_index());
        if (const auto refusal = plate_index_error(target, plates.get_plate_count()))
            return error_response(*refusal);
        if (!all_plates && plates.is_locked(target))
            return error_response(locked_plate_refusal(target));
        if (const auto refusal = plan_options(*plater, *request, options))
            return error_response(*refusal);

        save_options(*plater, options);
        if (all_plates) {
            facts = {{"scope", "all_plates"}, {"plate_count_before", plates.get_plate_count()}, {"plates_not_arranged", plates_not_arranged(plates)}};
            scope = scene_objects(*plater);
        } else {
            make_plate_current(*plater, target);
            facts = {{"scope", "plate"}, {"plate_index", target}};
            if (const auto note = automatic_spacing_note(*plates.get_plate(target), options.after))
                facts["spacing_note"] = *note;
            scope = current_plate_objects(*plater);
        }
        order   = object_order(plater->model());
        outcome = start_ui_job(*plater, UiJobKind::arrange, all_plates ? Job::PREPARE_STATE_DEFAULT : Job::PREPARE_STATE_MENU);
        return nullptr;
    });
    if (!refusal.is_null())
        return refusal;
    return answer_after_ui_job(*outcome, [scope, order, facts, options, all_plates, include_preview]() -> nlohmann::json {
        Plater*        plater = wxGetApp().plater();
        nlohmann::json answer = {{"status", "success"}};
        answer.update(facts);
        answer["objects"]           = placements_after(scope);
        answer["object_id_changes"] = object_id_changes(order, plater->model());
        if (all_plates)
            answer["plate_count"] = plater->get_partplate_list().get_plate_count();
        answer["current_plate_index"] = plater->get_partplate_list().get_curr_plate_index();
        add_options_report(answer, options);
        answer["active_warnings"] = get_active_warnings_json(plater);
        if (include_preview) {
            add_turntable_preview_if_requested(answer, true);
            answer["preview_hint"] = "Check the preview image to see the new arrangement of objects on the plate.";
        }
        return answer;
    });
}

}}} // namespace Slic3r::GUI::OrcaMCP

void Slic3r::GUI::OrcaMCPServer::register_arrange_tools()
{
    register_tool({
        "set_instance_count",
        ToolCategory::Transforms,
        "Set how many instances an object has",
        "Set how many instances (linked copies: one mesh, each placed on its own) an object has, as the GUI's Set number of instances "
        "does, in one undo step: new ones are added a small step from the last one, as Add instance places them (they may overlap: "
        "next_steps then names arrange_objects), and a lower count removes the last ones. count is 1 to 1000; to remove the object, "
        "delete_object; to remove one chosen instance, delete_object with instance_id. Answers the instances added or removed and "
        "each instance's placement; the same count is changed: false, with no undo step. Refused for an object with an unprintable "
        "instance or a piece of a cut, as the GUI's menu is, and while an arrange, orient or bed fill runs. An open toolbar tool is "
        "closed first (closed_toolbar_tool).",
        {{"type", "object"},
         {"properties",
          {{"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
           {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", k_max_instance_count}, {"description", "How many instances, 1 to 1000"}}},
           {"include_preview", {{"type", "boolean"}, {"description", "Return turntable preview path"}}}}},
         {"required", {"object_id", "count"}}},
        [](const nlohmann::json& params) -> nlohmann::json { return set_instance_count(params); }});

    nlohmann::json fill_properties = {
        {"object_id", {{"type", "integer"}, {"description", "Object index (0-based)"}}},
        {"instance_id",
         {{"type", "integer"},
          {"minimum", 0},
          {"description", "The instance whose plate is filled, and whose placement the new ones copy (default 0), as get_object_info's "
                          "instance_placement lists them"}}},
        {"include_preview", {{"type", "boolean"}, {"description", "Return turntable preview path, drawn once the fill has been applied"}}}};
    fill_properties.update(arrange_option_properties());
    register_tool({
        "fill_bed_with_instances",
        ToolCategory::Transforms,
        "Fill a plate with an object's instances",
        "Fill the free space of a plate with instances (linked copies) of an object, as the GUI's Fill bed with instances does: the plate "
        "the instance stands on, made current first, gets as many as fit, then its objects are arranged; one undo step (the GUI's "
        "takes two). The arrange menu's options this call gives are saved and used, as the menu's are. Answered once the fill and its "
        "arrange have been applied, with instances_added (0 when nothing more fits), instances_on_no_plate (copies the fill's "
        "estimate added and the arrange could not fit; next_steps names how to place or remove them), every object on the plate with "
        "its placement, and object_id_changes (the arrange re-sorts every object in the scene). Past the bridge's cap: status fill_bed_started with "
        "finished false, and get_slicing_status's ui_job says when it has ended. Refused for an object with an unprintable instance or a "
        "piece of a cut, an instance on no plate, and while another job runs; an open toolbar tool is closed first (closed_toolbar_tool).",
        {{"type", "object"}, {"properties", fill_properties}, {"required", {"object_id"}}},
        [](const nlohmann::json& params) -> nlohmann::json { return fill_bed_with_instances(params); }});
}
