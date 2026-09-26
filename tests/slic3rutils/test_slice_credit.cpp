#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPSliceCredit.hpp"

// Which plate a slice's completion is credited to, and what a plate-list change does to a running
// slice. A plate wrongly marked sliced exports and sends G-code that is not its own.

using namespace Slic3r::GUI::OrcaMCP;

TEST_CASE("a plate Slice All finds already sliced is credited to it, and the run goes on", "[SliceCredit]")
{
    // Never to the last started Print: that plate may have been edited since, and would be marked
    // sliced with a result that is no longer its own.
    const auto outcome = plate_not_started(/*plate_result_valid=*/true, /*worker_busy=*/false, /*current=*/9);
    CHECK(outcome.credit_print_index == 9);
    CHECK_FALSE(outcome.end_slice_all);
    CHECK(plate_not_started(true, /*worker_busy=*/true, 9).credit_print_index == 9); // nothing to start anyway
}

TEST_CASE("a plate Slice All could not start while the UI worker is busy is not marked sliced", "[SliceCredit]")
{
    // An arrange or orient job held the worker: the plate was never sliced. The run ends there, the
    // plate left unsliced, rather than marking it sliced with no G-code of its own.
    const auto outcome = plate_not_started(/*plate_result_valid=*/false, /*worker_busy=*/true, 9);
    CHECK(outcome.credit_print_index == -1);
    CHECK(outcome.end_slice_all);
}

TEST_CASE("a plate Slice All could not start for any other reason is skipped, not credited", "[SliceCredit]")
{
    const auto outcome = plate_not_started(/*plate_result_valid=*/false, /*worker_busy=*/false, 9);
    CHECK(outcome.credit_print_index == -1);
    CHECK_FALSE(outcome.end_slice_all);
}

TEST_CASE("what several plate-list changes did adds up", "[SliceCredit]")
{
    // delete_plate reports what every stop its deletion made did, not only the last (a no-op).
    PlateListChangeDuringSlice change;
    change |= on_plate_list_change(/*slice_running=*/true, /*slicing_all_plates=*/true);
    change |= on_plate_list_change(false, false);
    CHECK(change.stop_slice);
    CHECK(change.cancel_slice_all);
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
