// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceCredit.hpp
#pragma once
#include <optional>
#include <string>

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
    bool stop_slice       = false;
    bool cancel_slice_all = false;

    PlateListChangeDuringSlice& operator|=(const PlateListChangeDuringSlice& other)
    {
        stop_slice       = stop_slice || other.stop_slice;
        cancel_slice_all = cancel_slice_all || other.cancel_slice_all;
        return *this;
    }
};
inline PlateListChangeDuringSlice on_plate_list_change(bool slice_running, bool slicing_all_plates)
{
    return {slice_running || slicing_all_plates, slicing_all_plates};
}

// What delete_plate, undo and redo tell the agent about the slice they stopped; nullopt when none ran.
inline std::optional<std::string> plate_list_change_note(const PlateListChangeDuringSlice& change)
{
    if (change.cancel_slice_all)
        return std::string("the plate list changed during Slice All; the run was cancelled, call slice_all again");
    if (change.stop_slice)
        return std::string("the plate list changed during a slice; it was cancelled, call slice_all again");
    return std::nullopt;
}

}}} // namespace Slic3r::GUI::OrcaMCP
