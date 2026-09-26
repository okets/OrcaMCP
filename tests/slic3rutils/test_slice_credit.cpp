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
using Outcome = PlateNotStarted::Outcome;

TEST_CASE("a plate Slice All cannot start is sliced when its Print is finished, whatever its flag says", "[SliceCredit]")
{
    // Re-selecting the same preset, or a Tab reset, clears the plate's "sliced" flag and leaves its
    // Print finished, with its G-code: the run credits it as sliced, as upstream's "use the previous
    // result" does, and goes on without slicing it again.
    const auto finished = plate_not_started(/*print_finished=*/true, /*worker_busy=*/false, /*print_index=*/9);
    CHECK(finished.outcome == Outcome::already_sliced);
    CHECK(finished.credit_print_index == 9);

    const auto finished_while_busy = plate_not_started(true, /*worker_busy=*/true, 9); // nothing to start anyway
    CHECK(finished_while_busy.outcome == Outcome::already_sliced);
    CHECK(finished_while_busy.credit_print_index == 9);
}

TEST_CASE("a plate Slice All could not start while the UI worker is busy is not sliced, and the run ends", "[SliceCredit]")
{
    // An arrange or an orient held the worker, so the plate's Print was never finished: the run ends
    // there, the plate left unsliced, rather than marking it sliced with no G-code of its own.
    const auto busy = plate_not_started(/*print_finished=*/false, /*worker_busy=*/true, 9);
    CHECK(busy.outcome == Outcome::run_ended);
    CHECK(busy.credit_print_index == -1);

    const auto ended = slice_all_ended_by_busy_worker(/*plate_index=*/2);
    CHECK(ended.plate_index == 2);
    const std::string text = slice_all_ended_early_text(ended);
    CHECK(text.find("Slice All stopped at plate 2") != std::string::npos);
    CHECK(text.find("call slice_all again") != std::string::npos);
}

TEST_CASE("a plate Slice All could not start for any other reason is skipped, not credited", "[SliceCredit]")
{
    const auto skipped = plate_not_started(/*print_finished=*/false, /*worker_busy=*/false, 9);
    CHECK(skipped.outcome == Outcome::skipped);
    CHECK(skipped.credit_print_index == -1);
}

TEST_CASE("the log says what happens to a plate Slice All could not start", "[SliceCredit]")
{
    // Upstream logs "already sliced, skip to next" for every refusal, a busy worker's included.
    CHECK(plate_not_started_log(plate_not_started(true, false, 9), 1).find("plate 1 is already sliced") != std::string::npos);
    CHECK(plate_not_started_log(plate_not_started(false, false, 9), 1).find("not sliced, skipped") != std::string::npos);
    CHECK(plate_not_started_log(plate_not_started(false, true, 9), 1).find("the run ends here") != std::string::npos);
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
