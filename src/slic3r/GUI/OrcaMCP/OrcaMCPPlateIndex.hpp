// src/slic3r/GUI/OrcaMCP/OrcaMCPPlateIndex.hpp
#pragma once

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Where Slice All stands after a plate is deleted. It keeps the index of the plate it is slicing
// (Plater::priv::m_cur_slice_plate) and, when that slice completes, moves on to the next index. The
// plates after a deleted one move down by one, so without this a deletion made it skip a plate, or end
// early while reporting success. A plate deleted before the one being sliced moves that one down; the
// plate being sliced itself, deleted, leaves the next plate at its index, so the walk steps back one
// to reach it with its next step. A plate after it changes nothing.
inline int slice_all_position_after_delete(int position, int deleted_index)
{
    return deleted_index <= position ? position - 1 : position;
}

}}} // namespace Slic3r::GUI::OrcaMCP
