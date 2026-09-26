// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceCredit.hpp
#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// Which plate a slice's completion is credited to, and what a change to the plate list does to a
// running slice. No wx: the tests drive it with plain values (tests/slic3rutils/test_slice_credit.cpp).
//
// A completion carries the print index of the Print it is about (PartPlateList's key, never reused)
// and is credited to the plate that still holds it, or to none. The slice's own completion carries the
// Print it started (BackgroundSlicingProcess::thread_proc), and so does the edit that cancelled it.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A plate Slice All reached but could not start (restart_background_process said no).
struct PlateNotStarted
{
    enum class Outcome
    {
        already_sliced, // its Print is finished: credited as sliced, and the run goes on
        skipped,        // nothing to start (an empty plate): not sliced, and the run goes on
        run_ended,      // the UI worker is busy: not sliced, and the run ends here
    };
    Outcome outcome            = Outcome::skipped;
    int     credit_print_index = -1; // the plate's print index when it is already sliced, else -1: none
};

// Upstream's "use the previous result", decided by the Print rather than by the plate's "sliced" flag:
// a Tab reset or re-selecting the same preset clears the flag and leaves the Print finished, with its
// G-code, and such a plate is sliced. A Print not finished was not sliced. When the start was refused
// because the UI worker is busy (an arrange, an orient) the run ends with that plate not sliced, rather
// than marking it sliced with no G-code of its own; any other refusal skips the plate.
inline PlateNotStarted plate_not_started(bool print_finished, bool worker_busy, int print_index)
{
    if (print_finished)
        return {PlateNotStarted::Outcome::already_sliced, print_index};
    return {worker_busy ? PlateNotStarted::Outcome::run_ended : PlateNotStarted::Outcome::skipped, -1};
}

// The log line for it, saying what happens to the plate (`plate_index`, 0-based) and the run.
inline std::string plate_not_started_log(const PlateNotStarted& not_started, int plate_index)
{
    const std::string plate = "Slice All: plate " + std::to_string(plate_index);
    switch (not_started.outcome) {
    case PlateNotStarted::Outcome::already_sliced: return plate + " is already sliced (its Print is finished): credited as sliced";
    case PlateNotStarted::Outcome::skipped: return plate + " has nothing to slice: not sliced, skipped";
    case PlateNotStarted::Outcome::run_ended:
        return plate + " could not be started, the UI worker is busy: not sliced, and the run ends here";
    }
    return plate;
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
    return {plate_index, "another job (an arrange or an orient) was running"};
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
