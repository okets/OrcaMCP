// src/slic3r/GUI/OrcaMCP/OrcaMCPExports.cpp
#include "OrcaMCPExports.hpp"
#include "OrcaMCPNextSteps.hpp"

#include <algorithm>
#include <set>
#include <thread>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

bool ends_with_nocase(const std::string& text, const std::string& suffix) { return boost::algorithm::iends_with(text, suffix); }

// A plate whose G-code goes in the file, and one that keeps the file from being written.
bool counts_for_sliced_file(const SlicedFilePlate& plate) { return plate.has_objects && !plate.all_unprintable; }
bool ready_for_print(const SlicedFilePlate& plate) { return plate.sliced && !plate.check_refusal; }

std::optional<std::string> plate_not_ready(const SlicedFilePlate& plate)
{
    if (!plate.sliced)
        return "plate_index " + std::to_string(plate.index) + " has no slice result: slice_all, then wait_for_slice, first";
    return plate.check_refusal;
}

const SlicedFilePlate* plate_at(const std::vector<SlicedFilePlate>& plates, int index)
{
    const auto it = std::find_if(plates.begin(), plates.end(), [index](const SlicedFilePlate& p) { return p.index == index; });
    return it != plates.end() ? &*it : nullptr;
}

} // namespace

std::string GcodeExportOutcome::error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

void GcodeExportOutcome::end(State state, const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state.load(std::memory_order_acquire) != State::pending)
        return;
    m_error = error;
    m_state.store(state, std::memory_order_release);
}

bool GcodeExportOutcome::hand_to_waiting_call()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handed = m_call_waiting;
    return m_handed;
}

bool GcodeExportOutcome::stop_waiting()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_call_waiting = false;
    return m_handed;
}

GcodeExportWait wait_for_handed_end(const GcodeExportOutcome& outcome, bool handed, GcodeExportWait waited,
                                    const std::function<bool()>& quitting, std::chrono::milliseconds poll)
{
    if (!handed || waited != GcodeExportWait::timed_out)
        return waited;
    // The end always follows the hand-off, in the same on_process_completed, once the export's error is routed and before
    // the scene update (an exception there ends the app).
    while (outcome.state() == GcodeExportOutcome::State::pending) {
        if (quitting())
            return GcodeExportWait::quitting;
        std::this_thread::sleep_for(poll);
    }
    return GcodeExportWait::ended;
}

CompletionErrorRoute completion_error_route(bool handed_to_waiting_call, bool tracking_popup_menu)
{
    if (handed_to_waiting_call)
        return CompletionErrorRoute::captured_for_call;
    return tracking_popup_menu ? CompletionErrorRoute::after_popup_menu : CompletionErrorRoute::dialog;
}

nlohmann::json gcode_export_state_json(const GcodeExportOutcome& outcome, const std::string& output_path)
{
    const auto state_name = [](GcodeExportOutcome::State state) {
        switch (state) {
        case GcodeExportOutcome::State::pending: return "writing";
        case GcodeExportOutcome::State::written: return "written";
        case GcodeExportOutcome::State::failed: return "failed";
        case GcodeExportOutcome::State::cancelled: return "cancelled";
        case GcodeExportOutcome::State::dropped: return "dropped";
        }
        return "writing";
    };
    nlohmann::json json = {{"output_path", output_path}, {"state", state_name(outcome.state())}};
    if (outcome.state() == GcodeExportOutcome::State::failed)
        json["error"] = outcome.error();
    return json;
}

nlohmann::json gcode_export_answer(const GcodeExportOutcome& outcome, GcodeExportWait wait, const std::string& output_path,
                                   std::optional<std::uintmax_t> bytes, double waited_s)
{
    if (wait == GcodeExportWait::timed_out) {
        nlohmann::json answer = {{"status", "export_started"},
                                 {"output_path", output_path},
                                 {"finished", false},
                                 {"waited_s", waited_s},
                                 {"note", "The G-code is still being written, past how long this call waits: it is complete once "
                                          "wait_for_slice returns."}};
        add_next_steps(answer, export_next_steps(true));
        return answer;
    }
    if (wait == GcodeExportWait::quitting)
        return {{"status", "export_started"},
                {"output_path", output_path},
                {"finished", false},
                {"message", "OrcaMCP began quitting while the G-code was being written; the file may not be complete"}};

    const std::string not_written = "The G-code export ended and nothing was written to " + output_path + ": ";
    switch (outcome.state()) {
    case GcodeExportOutcome::State::written: {
        nlohmann::json answer = {{"status", "success"}, {"output_path", output_path}, {"format", "gcode"}, {"waited_s", waited_s}};
        if (bytes)
            answer["bytes"] = *bytes;
        return answer;
    }
    case GcodeExportOutcome::State::failed: return {{"status", "error"}, {"message", "The G-code export failed: " + outcome.error()}};
    case GcodeExportOutcome::State::cancelled:
        return {{"status", "error"},
                {"message", not_written + "it was cancelled (cancel_slice, the app's Cancel, or a change that restarted the slice)"}};
    case GcodeExportOutcome::State::dropped:
    case GcodeExportOutcome::State::pending:
        return {{"status", "error"},
                {"message", not_written + "the app took it off (a new project, a project opened, or the plate list changed)"}};
    }
    return {{"status", "error"}, {"message", not_written + "unknown"}};
}

GcodeExportKind gcode_export_kind(const std::string& output_path)
{
    return ends_with_nocase(output_path, ".gcode.3mf") ? GcodeExportKind::sliced_file : GcodeExportKind::gcode;
}

std::string sliced_file_path(const std::string& chosen)
{
    return ends_with_nocase(chosen, ".3mf") ? chosen : chosen + ".3mf";
}

std::optional<std::string> gcode_export_path_refusal(const std::string& output_path, bool all_plates)
{
    if (output_path.empty())
        return std::string("output_path is required: file dialogs cannot be opened from MCP.");
    const GcodeExportKind kind = gcode_export_kind(output_path);
    if (kind == GcodeExportKind::gcode && ends_with_nocase(output_path, ".3mf"))
        return "output_path \"" + output_path + "\" is a 3MF but not a .gcode.3mf: end it in .gcode.3mf for the sliced file, or save "
               "the project with export_3mf";
    if (kind == GcodeExportKind::gcode && all_plates)
        return std::string("A plain G-code file holds one plate: pass an output_path ending in .gcode.3mf for every plate's G-code in "
                           "one file, or select_plate and export_gcode each plate");
    return std::nullopt;
}

std::optional<std::string> sliced_file_refusal(const std::vector<SlicedFilePlate>& plates, bool all_plates, int selected_index)
{
    const std::string refused = "The sliced file was not written: ";
    if (!all_plates) {
        const SlicedFilePlate* selected = plate_at(plates, selected_index);
        if (selected == nullptr || !selected->has_objects)
            return refused + "plate_index " + std::to_string(selected_index) + " has nothing on it to export";
        if (const auto not_ready = plate_not_ready(*selected))
            return refused + *not_ready;
        return std::nullopt;
    }
    bool any_ready = false;
    for (const SlicedFilePlate& plate : plates) {
        if (!counts_for_sliced_file(plate))
            continue;
        if (const auto not_ready = plate_not_ready(plate))
            return refused + *not_ready;
        any_ready = true;
    }
    if (!any_ready)
        return refused + "no plate has a printable object on it";
    return std::nullopt;
}

std::vector<int> sliced_file_plates(const std::vector<SlicedFilePlate>& plates, bool all_plates, int selected_index)
{
    std::vector<int> indexes;
    for (const SlicedFilePlate& plate : plates)
        if ((all_plates || plate.index == selected_index) && ready_for_print(plate))
            indexes.push_back(plate.index);
    return indexes;
}

MeshExportDecision plan_mesh_export(const MeshExportRequest& request, int object_count, const std::function<bool(const std::string&)>& is_folder)
{
    const auto refuse = [](std::string why) { return MeshExportDecision{std::nullopt, std::move(why)}; };
    if (object_count <= 0)
        return refuse("The scene has no objects, so there is nothing to export.");
    if (request.output_path.empty())
        return refuse("output_path is required: file dialogs cannot be opened from MCP.");

    MeshExportPlan plan;
    plan.one_file_per_object = request.one_file_per_object;

    std::string format = request.format ? boost::algorithm::to_lower_copy(*request.format) : std::string();
    if (!format.empty() && format != "stl" && format != "drc")
        return refuse("format must be stl or drc, got \"" + *request.format + "\"");
    if (plan.one_file_per_object) {
        if (!is_folder(request.output_path))
            return refuse("With one_file_per_object, output_path is the folder the files go in, and \"" + request.output_path +
                          "\" is not an existing folder");
        if (format.empty())
            format = "stl";
    } else {
        const std::string extension = ends_with_nocase(request.output_path, ".stl") ? "stl" :
                                      ends_with_nocase(request.output_path, ".drc") ? "drc" :
                                                                                      std::string();
        if (extension.empty())
            return refuse("output_path must end in .stl or .drc (or pass one_file_per_object with a folder), got \"" +
                          request.output_path + "\"");
        if (!format.empty() && format != extension)
            return refuse("format is " + format + " but output_path ends in ." + extension + ": make them match");
        format = extension;
    }
    plan.drc = format == "drc";

    if (request.object_ids) {
        if (request.object_ids->empty())
            return refuse("object_ids is empty: leave it out to export every object");
        std::set<int> seen;
        for (int id : *request.object_ids) {
            if (id < 0 || id >= object_count)
                return refuse("Invalid object_id " + std::to_string(id) + ": the scene has " + std::to_string(object_count) + " objects");
            if (!seen.insert(id).second)
                return refuse("object_id " + std::to_string(id) + " is listed twice in object_ids");
        }
        plan.selection_only = true;
        plan.object_ids     = *request.object_ids;
    }
    return {plan, {}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
