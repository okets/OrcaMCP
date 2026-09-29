// src/slic3r/GUI/OrcaMCP/OrcaMCPExports.hpp
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// What export_gcode's sliced-file form and export_stl decide before the app writes anything: which
// file a path asks for, and why a call is refused. No wx and no Plater: the tests drive it with plain
// values (tests/slic3rutils/test_mcp_exports.cpp). The files themselves are written by the app's own
// exports -- Plater::export_gcode_3mf (File > Export > Export plate sliced file) and Plater::export_stl
// (File > Export > Export all objects as one STL / as STLs, and the object menu's Export as one STL) --
// whose file dialogs the call's path answers (mcp_answer_path_dialog).

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// ---- export_gcode ------------------------------------------------------------------------------

enum class GcodeExportKind
{
    gcode,       // a plain .gcode of the selected plate, written by the background process
    sliced_file, // a .gcode.3mf: the plate's, or every plate's, G-code inside a 3MF (the plate sliced file)
};

// What `output_path` asks for: a path ending in .gcode.3mf (any case) is a sliced file.
GcodeExportKind gcode_export_kind(const std::string& output_path);

// Where the app writes a sliced file chosen at `chosen`: there when it ends in .3mf, in any case, else with .3mf
// added. Plater::export_gcode_3mf's file dialog, answered by the user or by MCP, and export_gcode's answer
// all read it, so the answer names the file written (upstream's test was case-sensitive: X.GCODE.3MF was
// written as X.GCODE.3MF.3mf).
std::string sliced_file_path(const std::string& chosen);

// Why export_gcode refuses `output_path` with `all_plates` before looking at the plates, or nullopt: no
// path; a .3mf that is not a .gcode.3mf (a project, which export_3mf writes); all_plates for a plain
// .gcode, which holds one plate.
std::optional<std::string> gcode_export_path_refusal(const std::string& output_path, bool all_plates);

// One plate as the app's Export plate sliced file reads it (PartPlate).
struct SlicedFilePlate
{
    int                        index           = -1;
    bool                       has_objects     = false; // !PartPlate::empty()
    bool                       all_unprintable = false; // PartPlate::is_all_instances_unprintable
    bool                       sliced          = false; // a valid slice result
    std::optional<std::string> check_refusal;           // what the check its slice ran on its G-code found
};

// Why the sliced file cannot be written, or nullopt, as the menu's own enabling decides it: the
// selected plate (MainFrame::can_export_gcode), or every plate with a printable object on it, of which
// at least one (MainFrame::can_export_all_gcode, PartPlateList::is_all_slice_results_ready_for_print).
// A plate without a slice result names slice_all; one whose G-code check failed, what it found.
std::optional<std::string> sliced_file_refusal(const std::vector<SlicedFilePlate>& plates, bool all_plates, int selected_index);

// The plates whose G-code the file holds: the selected one, or every plate with a slice result.
std::vector<int> sliced_file_plates(const std::vector<SlicedFilePlate>& plates, bool all_plates, int selected_index);

// ---- export_stl --------------------------------------------------------------------------------

// ---- export_gcode's wait for a plain .gcode ------------------------------------------------------

// How a plain G-code export MCP started ended. The app writes the file in the background, as its Export G-code does, and
// records here how that went when it takes the export's completion in (Plater::priv::on_process_completed), on the main
// thread; export_gcode reads it on the HTTP thread while it waits. Only the first end counts.
class GcodeExportOutcome
{
public:
    enum class State
    {
        pending,
        written,   // the file is written
        failed,    // the export ended in an error: error() has the app's words
        cancelled, // cancelled before it was written (cancel_slice, the app's Cancel, a new slice)
        dropped,   // taken off without a completion (a new project, a project opened, the plate list changed)
    };
    State       state() const { return m_state.load(std::memory_order_acquire); }
    std::string error() const;
    void        end(State state, const std::string& error = {});

    // The call that started the export waits for it. While it does, the app's completion hands it the export's end
    // (and captures the app's error dialog, whose words the call answers); once the call has stopped waiting, the app
    // shows its dialog as for its own exports. Main thread, at the completion: whether the call still waits, and so
    // answers the end.
    bool hand_to_waiting_call();
    // HTTP thread, when the call stops waiting: whether the completion was handed to it (then it answers the export's
    // end, which the main thread records next); after this, none is.
    bool stop_waiting();

private:
    std::atomic<State> m_state{State::pending};
    mutable std::mutex m_mutex;
    std::string        m_error;
    bool               m_call_waiting = true;
    bool               m_handed       = false;
};

// get_slicing_status's last_export: {output_path, state: writing, written, failed, cancelled or dropped, error when failed}.
nlohmann::json gcode_export_state_json(const GcodeExportOutcome& outcome, const std::string& output_path);

// How export_gcode's wait for its export ended: the export ended, the call's wait cap passed first, or the app began quitting.
enum class GcodeExportWait { ended, timed_out, quitting };

// After export_gcode's wait for its export stopped with `waited`: when the app had handed the completion to the call
// (`handed`, what stop_waiting answered), the wait for the end the app records right after the hand-off, which only a
// quit (`quitting`, asked every `poll`) cuts short. on_process_completed ends the outcome once it has updated the scene,
// which a large preview can take seconds for on the -O0 build; the call answers that end, whose error dialog it
// captured. Returns how the wait ended: `waited` as it was when nothing was handed or the wait did not time out.
GcodeExportWait wait_for_handed_end(const GcodeExportOutcome& outcome, bool handed, GcodeExportWait waited,
                                    const std::function<bool()>& quitting,
                                    std::chrono::milliseconds poll = std::chrono::milliseconds(20));

// Where on_process_completed sends an export's critical error. Handed to the waiting call, it goes to the call's guard,
// which captures the error dialog (the call answers the error), even while a popup menu is open: queued for the menu,
// it showed its dialog once the menu closed (Plater::PopupMenu), after the call had answered it. Else, while a popup
// menu is open, it waits for the menu to close; else the error dialog shows.
enum class CompletionErrorRoute { captured_for_call, after_popup_menu, dialog };
CompletionErrorRoute completion_error_route(bool handed_to_waiting_call, bool tracking_popup_menu);

// export_gcode's answer for a plain .gcode once its wait is over: success with output_path and bytes (the file's size,
// when it is there) once written; error with why when it failed, was cancelled or was taken off; past the cap, the
// export_started it answered before it waited, with how long it waited and next_steps to wait_for_slice; the app
// quitting: export_started, finished false. Without active_warnings, which the caller adds.
nlohmann::json gcode_export_answer(const GcodeExportOutcome& outcome, GcodeExportWait wait, const std::string& output_path,
                                   std::optional<std::uintmax_t> bytes, double waited_s);

// export_stl's arguments, as the handler read them.
struct MeshExportRequest
{
    std::string                     output_path;
    std::optional<std::string>      format;             // "stl" or "drc"; nullopt: from output_path's extension
    std::optional<std::vector<int>> object_ids;         // nullopt: every object
    bool                            one_file_per_object = false;
};

// What the app is asked to write.
struct MeshExportPlan
{
    bool             drc                 = false; // Draco, else STL
    bool             one_file_per_object = false; // output_path is a folder, one file per object (or instance)
    bool             selection_only      = false; // the object menu's export of the objects named, else the File menu's of all
    std::vector<int> object_ids;                  // the objects selected for a selection_only export
};

// The plan for `request` over a scene of `object_count` objects, or the refusal: no objects; an
// object_id out of range or listed twice; an empty object_ids; a format other than stl or drc; a file
// whose extension is not the format's; a folder that does not exist (`is_folder`) for one file per object.
struct MeshExportDecision
{
    std::optional<MeshExportPlan> plan;
    std::string                   refusal; // when there is no plan
};
MeshExportDecision plan_mesh_export(const MeshExportRequest& request, int object_count, const std::function<bool(const std::string&)>& is_folder);

}}} // namespace Slic3r::GUI::OrcaMCP
