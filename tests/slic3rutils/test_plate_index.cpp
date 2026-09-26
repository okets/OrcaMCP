#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPlateIndex.hpp"

// Slice All walks the plates by index and steps to the next one when a slice completes. Deleting a
// plate mid-walk moves every later plate down by one; these pin where the walk must then stand so that
// its next step reaches the first plate it has not sliced yet.

using namespace Slic3r::GUI::OrcaMCP;

TEST_CASE("deleting a plate after the one being sliced leaves Slice All where it is", "[PlateIndex]")
{
    CHECK(slice_all_position_after_delete(/*position=*/1, /*deleted_index=*/2) == 1);
}

TEST_CASE("deleting a plate before the one being sliced moves Slice All down with it", "[PlateIndex]")
{
    // Slicing plate 2 of 0..3; plate 0 goes: the plate being sliced is now 1, and the next step reaches
    // the old plate 3, now 2.
    CHECK(slice_all_position_after_delete(2, 0) == 1);
}

TEST_CASE("deleting the plate being sliced lets Slice All's next step reach the plate after it", "[PlateIndex]")
{
    // Slicing plate 1; it goes, and the old plate 2 is now at 1: stand at 0, so the next step is 1.
    CHECK(slice_all_position_after_delete(1, 1) == 0);
    // The first plate: stand before it.
    CHECK(slice_all_position_after_delete(0, 0) == -1);
}
