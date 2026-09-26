// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceCredit.hpp
#pragma once
#include <optional>
#include <string>

// Which plate a slice's completion is credited to, and what a change to the plate list does to a
// running slice. No wx: the tests drive it with plain numbers (tests/slic3rutils/test_slice_credit.cpp).

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Where a completion event comes from.
enum class CompletionKind
{
    sliced,          // the slicing thread ran a slice: finished, cancelled or failed
    apply_cancelled, // an edit cancelled the running slice (Plater::priv::update_background_process)
    already_sliced,  // "nothing to slice on this plate, go on": Slice All's skip, start_next_slice
};

// The print index a completion carries: the key of the Print it is about, which PartPlateList never
// reuses. The completion is credited to the plate that still holds it
// (PartPlateList::find_plate_by_print_index), wherever that plate now stands, or to none once it has
// been deleted. A slice, and the edit that cancelled it, are about the Print whose slice was started; a
// skip is about the current plate, which was not started at all -- crediting it to the last started
// Print marked that plate sliced with a result that was no longer its own.
inline int completion_print_index(CompletionKind kind, int started_print_index, int current_plate_print_index)
{
    return kind == CompletionKind::already_sliced ? current_plate_print_index : started_print_index;
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
