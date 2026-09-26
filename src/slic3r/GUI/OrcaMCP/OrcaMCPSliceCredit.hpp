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
    int  credit_print_index = -1;    // the current plate's when its result really is valid, else -1: none
    bool end_slice_all      = false; // the UI worker is busy (an arrange, an orient): end the run, not sliced
};

// "Already sliced" is credited only to a plate whose result is valid. When the start failed because the
// UI worker was busy the plate was never sliced: crediting it marked it sliced with no G-code of its
// own, and export or send would have used a stale file. Any other refusal (nothing to slice, an invalid
// plate) credits nothing and lets the run go on to the next plate.
inline PlateNotStarted plate_not_started(bool plate_result_valid, bool worker_busy, int current_plate_print_index)
{
    if (plate_result_valid)
        return {current_plate_print_index, false};
    return {-1, worker_busy};
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
