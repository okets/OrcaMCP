// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceCredit.hpp
#pragma once
#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

// Which plate a slice's completion is credited to, and what a change to the plate list does to a
// running slice. No wx: the tests drive it with plain values (tests/slic3rutils/test_slice_credit.cpp).
//
// A completion carries the print index of the Print it is about (PartPlateList's key, never reused).
// The slice's own completion carries the Print it started (BackgroundSlicingProcess::thread_proc), the
// edit that cancelled it the same, and Slice All's "could not start this plate" the current plate's.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// What the plate holding a completion's print index is like when the completion is handled.
struct PlateAtCompletion
{
    bool exists         = false; // a plate still holds that print index
    bool print_empty    = true;  // its Print has nothing to slice (no objects, or a plate from a .gcode.3mf)
    bool print_finished = false; // its current Print is finished, with its G-code
    bool validation_ok  = false; // it passed validation (PartPlate::is_apply_result_invalid is false), and
                                 // the start was not refused for a failed one
};
enum class CompletionCredit
{
    none,       // leave the plate's "sliced" flag as it is
    sliced,     // mark it sliced
    not_sliced, // mark it not sliced
};
// The credit is decided when the completion is handled, never when it was posted: in between an edit,
// an undo or an arrange may have invalidated the Print, or moved or deleted the plate, and its G-code is
// then stale. A plate is marked sliced only when it is still there, its current Print is finished and
// it passed validation, for a completion that succeeded; otherwise it is marked not sliced. A plate gone
// is left alone, and so is one whose Print is empty, as upstream leaves it.
inline CompletionCredit credit_completion(const PlateAtCompletion& plate, bool completion_succeeded)
{
    if (!plate.exists || plate.print_empty)
        return CompletionCredit::none;
    if (completion_succeeded && plate.print_finished && plate.validation_ok)
        return CompletionCredit::sliced;
    return CompletionCredit::not_sliced;
}

// A plate Slice All reached but could not start (restart_background_process said no): what the run
// does. The plate itself is credited by credit_completion when the completion posted for it is handled.
enum class PlateNotStarted
{
    already_sliced, // its Print is finished: the run goes on
    invalid,        // it failed validation: not sliced, and the run goes on
    run_ended,      // the UI worker is busy (an arrange, an orient): not sliced, and the run ends here
    skipped,        // nothing to start (an empty plate): the run goes on
};
// Upstream's "use the previous result", decided by the Print rather than by the plate's "sliced" flag:
// a Tab reset or re-selecting the same preset clears the flag and leaves the Print finished, with its
// G-code. A Print not finished was not sliced: on a busy worker the run ends there rather than marking
// the plate sliced with no G-code of its own.
inline PlateNotStarted plate_not_started(bool print_finished, bool worker_busy, bool validation_ok)
{
    if (!validation_ok)
        return PlateNotStarted::invalid;
    if (print_finished)
        return PlateNotStarted::already_sliced;
    return worker_busy ? PlateNotStarted::run_ended : PlateNotStarted::skipped;
}

// The log line for it, saying what happens to the plate (`plate_index`, 0-based) and the run.
inline std::string plate_not_started_log(PlateNotStarted not_started, int plate_index)
{
    const std::string plate = "Slice All: plate " + std::to_string(plate_index);
    switch (not_started) {
    case PlateNotStarted::already_sliced: return plate + " is already sliced (its Print is finished); the run goes on";
    case PlateNotStarted::invalid: return plate + " failed validation: not sliced, skipped";
    case PlateNotStarted::run_ended:
        return plate + " could not be started, the UI worker is busy: not sliced, and the run ends here";
    case PlateNotStarted::skipped: return plate + " has nothing to slice: skipped";
    }
    return plate;
}

// An undo or redo whose snapshot load threw part way (Plater::priv::recover_from_failed_jump). The
// project may be inconsistent, and nothing tries to mend it: from then on MCP refuses every tool that
// reads or changes the scene, so none walks plates the load left half built, until the app is
// restarted. What the message asks for stays open -- saving a copy -- and so does quitting.
inline constexpr const char* k_failed_jump_message = "Undo/redo failed partway; the project may be inconsistent. Save a copy "
                                                     "(save_project with a new output_path) and restart OrcaMCP.";
inline std::atomic<bool>& jump_failed_partway()
{
    static std::atomic<bool> failed{false};
    return failed;
}
inline std::optional<std::string> refusal_after_failed_jump(bool jump_failed, const std::string& tool)
{
    if (!jump_failed || tool == "save_project" || tool == "export_3mf" || tool == "quit_app" || tool == "get_server_info")
        return std::nullopt;
    return std::string(k_failed_jump_message);
}

// A slice the safety net cancelled (frees_what_the_slice_uses), reported once by the next response.
inline std::string slice_cancelled_by_free_text(const std::string& caller)
{
    return "a slice was cancelled because " + caller + " freed the plate or Print it was running on; call slice_all again";
}

// A Slice All run that ended before its last plate, for get_slicing_status and active_warnings.
struct SliceAllEndedEarly
{
    int         plate_index = -1; // 0-based: the plate it stopped at, not sliced
    std::string reason;
};
// The run_ended outcome's: the plate could not be started while the UI worker was busy.
inline SliceAllEndedEarly slice_all_ended_by_busy_worker(int plate_index)
{
    return {plate_index, "another job (an arrange, an orient or a bed fill) was running"};
}
inline std::string slice_all_ended_early_text(const SliceAllEndedEarly& ended)
{
    return "Slice All stopped at plate " + std::to_string(ended.plate_index) + ": " + ended.reason + "; call slice_all again";
}

// A plate deleted or moved while a slice runs, or the plate list rebuilt (undo, redo, a 3MF load). The
// slice is stopped: its Print or its plate may be the one freed, and the process may be pointed at
// another plate. A Slice All run is cancelled, as any cancel ends it: it walks the plates by index.
struct PlateListChangeDuringSlice
{
    bool slice_cancelled     = false; // a slice still in progress was cancelled
    bool slice_all_cancelled = false;

    PlateListChangeDuringSlice& operator|=(const PlateListChangeDuringSlice& other)
    {
        slice_cancelled     = slice_cancelled || other.slice_cancelled;
        slice_all_cancelled = slice_all_cancelled || other.slice_all_cancelled;
        return *this;
    }
};
// `stop_cancelled_a_slice`: the stop found a slice still in progress and cancelled it. A slice that had
// already finished, its completion still queued, was not cancelled: that completion credits its plate by
// print index when it arrives, if the plate is still there. A Slice All run is cancelled either way.
inline PlateListChangeDuringSlice on_plate_list_change(bool stop_cancelled_a_slice, bool slicing_all_plates)
{
    return {stop_cancelled_a_slice, slicing_all_plates};
}

// What delete_plate, undo and redo tell the agent about the slice they stopped; nullopt when none ran.
inline std::optional<std::string> plate_list_change_note(const PlateListChangeDuringSlice& change)
{
    if (change.slice_all_cancelled)
        return std::string("the plate list changed during Slice All; the run was cancelled, call slice_all again");
    if (change.slice_cancelled)
        return std::string("the plate list changed during a slice; it was cancelled, call slice_all again");
    return std::nullopt;
}

// The safety net where PartPlateList frees plates or Prints (PartPlateList::set_before_free): true when
// the slicing process is running on one of them, and must be stopped before they go. The callers stop
// it themselves first; this catches one that does not, which crashed the app (SIGSEGV, 2026-09-26).
template<class Plate, class Print>
bool frees_what_the_slice_uses(bool                             slice_running,
                               const Plate*                     slice_plate,
                               const Print*                     slice_print,
                               const std::vector<const Plate*>& plates,
                               const std::vector<const Print*>& prints)
{
    if (!slice_running)
        return false;
    const auto holds = [](const auto& freed, const auto* used) {
        return used != nullptr && std::find(freed.begin(), freed.end(), used) != freed.end();
    };
    return holds(plates, slice_plate) || holds(prints, slice_print);
}

}}} // namespace Slic3r::GUI::OrcaMCP
