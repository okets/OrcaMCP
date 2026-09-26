#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPSliceCredit.hpp"

// Which plate a slice's completion is credited to, and what a plate-list change does to a running
// slice. A plate wrongly marked sliced exports and sends G-code that is not its own.

using namespace Slic3r::GUI::OrcaMCP;

TEST_CASE("a skip is credited to the current plate, never to the last started print", "[SliceCredit]")
{
    // Slice All reaches plate 2, already sliced, and skips it. The last slice started was plate 1's
    // (print 7), since invalidated by an edit: crediting the skip to it marked plate 1 sliced with a
    // result that was no longer its own.
    CHECK(completion_print_index(CompletionKind::already_sliced, /*started=*/7, /*current=*/9) == 9);
}

TEST_CASE("a slice, or an edit that cancels it, is about the print that was being sliced", "[SliceCredit]")
{
    // Not the current plate's: a plate switch or deletion may have repointed the process since.
    CHECK(completion_print_index(CompletionKind::sliced, /*started=*/7, /*current=*/9) == 7);
    CHECK(completion_print_index(CompletionKind::apply_cancelled, 7, 9) == 7);
}

TEST_CASE("a plate-list change stops a running slice and cancels a Slice All run", "[SliceCredit]")
{
    const auto idle = on_plate_list_change(/*slice_running=*/false, /*slicing_all_plates=*/false);
    CHECK_FALSE(idle.stop_slice);
    CHECK_FALSE(idle.cancel_slice_all);
    CHECK_FALSE(plate_list_change_note(idle));

    const auto one = on_plate_list_change(true, false);
    CHECK(one.stop_slice);
    CHECK_FALSE(one.cancel_slice_all);
    REQUIRE(plate_list_change_note(one));
    CHECK(plate_list_change_note(one)->find("call slice_all again") != std::string::npos);

    // Between two plates of the run the process may be idle, a completion still queued: the run is
    // cancelled all the same, or it would go on by an index the change has shifted.
    const auto between = on_plate_list_change(false, true);
    CHECK(between.stop_slice);
    CHECK(between.cancel_slice_all);
    REQUIRE(plate_list_change_note(between));
    CHECK(plate_list_change_note(between)->find("Slice All; the run was cancelled") != std::string::npos);
}
