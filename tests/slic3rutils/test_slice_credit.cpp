#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPSliceCredit.hpp"
#include "slic3r/GUI/PartPlate.hpp"

// Which plate a slice's completion is credited to, and what a plate-list change does to a running
// slice. A plate wrongly marked sliced exports and sends G-code that is not its own; a plate or Print
// freed under the slicing thread crashes the app.

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
    // A caller reports what every stop its change made did, not only the last (a no-op).
    PlateListChangeDuringSlice change;
    change |= on_plate_list_change(/*stop_cancelled_a_slice=*/true, /*slicing_all_plates=*/true);
    change |= on_plate_list_change(false, false);
    CHECK(change.slice_cancelled);
    CHECK(change.slice_all_cancelled);
}

TEST_CASE("a plate-list change reports a cancel only for a slice that was still in progress", "[SliceCredit]")
{
    const auto idle = on_plate_list_change(/*stop_cancelled_a_slice=*/false, /*slicing_all_plates=*/false);
    CHECK_FALSE(idle.slice_cancelled);
    CHECK_FALSE(idle.slice_all_cancelled);
    CHECK_FALSE(plate_list_change_note(idle));

    // A slice that had finished, its completion still queued, was not cancelled: the stop found it done
    // (BackgroundSlicingProcess::stop's `cancelled_a_slice`), and the completion credits its plate.
    const auto finished_before = on_plate_list_change(/*stop_cancelled_a_slice=*/false, false);
    CHECK_FALSE(plate_list_change_note(finished_before));

    const auto cancelled = on_plate_list_change(true, false);
    CHECK(cancelled.slice_cancelled);
    CHECK_FALSE(cancelled.slice_all_cancelled);
    REQUIRE(plate_list_change_note(cancelled));
    CHECK(plate_list_change_note(cancelled)->find("call slice_all again") != std::string::npos);

    // Between two plates of the run nothing may be in progress, a completion still queued: the run is
    // cancelled all the same, or it would go on by an index the change has shifted.
    const auto between = on_plate_list_change(false, true);
    CHECK_FALSE(between.slice_cancelled);
    CHECK(between.slice_all_cancelled);
    REQUIRE(plate_list_change_note(between));
    CHECK(plate_list_change_note(between)->find("Slice All; the run was cancelled") != std::string::npos);
}

namespace {
struct FakePlate {};
struct FakePrint {};
} // namespace

TEST_CASE("the safety net stops a slice only when what it runs on is about to be freed", "[SliceCredit]")
{
    const FakePlate sliced_plate, other_plate;
    const FakePrint sliced_print, other_print;
    const std::vector<const FakePlate*> no_plates;
    const std::vector<const FakePrint*> no_prints;

    CHECK(frees_what_the_slice_uses(true, &sliced_plate, &sliced_print, no_plates, {&other_print, &sliced_print}));
    CHECK(frees_what_the_slice_uses(true, &sliced_plate, &sliced_print, {&sliced_plate}, no_prints));
    CHECK_FALSE(frees_what_the_slice_uses(true, &sliced_plate, &sliced_print, {&other_plate}, {&other_print}));
    // Nothing runs: freeing is safe, the process is pointed elsewhere before it starts again.
    CHECK_FALSE(frees_what_the_slice_uses(false, &sliced_plate, &sliced_print, {&sliced_plate}, {&sliced_print}));
}

namespace {
// What PartPlateList told its before-free hook, and whether each thing was still there when it did.
struct FreedLog
{
    std::vector<std::string>                          callers;
    std::vector<const Slic3r::GUI::PartPlate*>        plates;
    std::vector<const Slic3r::PrintBase*>             prints;

    Slic3r::GUI::PartPlateList::BeforeFree hook()
    {
        return [this](const std::vector<const Slic3r::GUI::PartPlate*>& p, const std::vector<const Slic3r::PrintBase*>& r,
                      const char* caller) {
            callers.emplace_back(caller);
            plates.insert(plates.end(), p.begin(), p.end());
            prints.insert(prints.end(), r.begin(), r.end());
        };
    }
};

const Slic3r::PrintBase* print_of(Slic3r::GUI::PartPlateList& list, int plate)
{
    Slic3r::PrintBase* print = nullptr;
    list.get_plate(plate)->get_print(&print, nullptr, nullptr);
    return print;
}
} // namespace

TEST_CASE("the plate list tells its hook about every plate and Print before it frees them", "[SliceCredit]")
{
    // The hook is how the Plater stops a slice running on what is about to go, whichever caller frees it.
    Slic3r::Model              model;
    Slic3r::GUI::PartPlateList list(nullptr, &model, Slic3r::ptFFF);
    FreedLog                   log;
    list.set_before_free(log.hook());

    SECTION("destroy_print names the Print it frees")
    {
        int index = -1;
        list.get_plate(0)->get_print(nullptr, nullptr, &index);
        const Slic3r::PrintBase* print = print_of(list, 0);
        list.destroy_print(index);
        REQUIRE(log.callers.size() == 1);
        CHECK(log.callers[0] == "PartPlateList::destroy_print");
        CHECK(log.prints == std::vector<const Slic3r::PrintBase*>{print});
    }
    SECTION("an unknown print index frees nothing, and tells nothing")
    {
        list.destroy_print(12345);
        CHECK(log.callers.empty());
    }
    SECTION("reinit names every plate and Print, and the path it came by")
    {
        const Slic3r::GUI::PartPlate* plate = list.get_plate(0);
        const Slic3r::PrintBase*      print = print_of(list, 0);
        list.reinit();
        REQUIRE(log.callers.size() == 1);
        CHECK(log.callers[0] == "PartPlateList::reinit");
        CHECK(log.plates == std::vector<const Slic3r::GUI::PartPlate*>{plate});
        CHECK(log.prints == std::vector<const Slic3r::PrintBase*>{print});
    }
    SECTION("reset without init names the plates it deletes, and keeps the Prints")
    {
        const Slic3r::GUI::PartPlate* plate = list.get_plate(0);
        list.reset(false);
        REQUIRE(log.callers.size() == 1);
        CHECK(log.callers[0] == "PartPlateList::reset");
        CHECK(log.plates == std::vector<const Slic3r::GUI::PartPlate*>{plate});
        CHECK(log.prints.empty());
    }
}
