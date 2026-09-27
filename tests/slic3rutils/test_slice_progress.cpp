#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <optional>
#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPSliceProgress.hpp"

// What get_slicing_status says while a slice runs (each plate's percent, the stage) and once it is
// over (slice_run.outcome), which is what wait_for_slice ends its wait on. Reporting a run done that
// left a plate unsliced sends the agent on to export G-code that does not exist.

using namespace Slic3r::GUI::OrcaMCP;

namespace {
SliceRunPlate sliced_plate(int index) { return {/*exists=*/true, /*sliced=*/true, index}; }
SliceRunPlate unsliced_plate(int index) { return {/*exists=*/true, /*sliced=*/false, index}; }
SliceRunPlate gone_plate() { return {/*exists=*/false, /*sliced=*/false, -1}; }
} // namespace

TEST_CASE("a plate with no result and no slice running reports no percent", "[orcamcp][SliceProgress]")
{
    CHECK_FALSE(reported_slice_percent(-1.0f).has_value());
}

TEST_CASE("a plate's percent is reported as a whole number from 0 to 100", "[orcamcp][SliceProgress]")
{
    CHECK(reported_slice_percent(0.0f) == std::optional<int>(0));
    CHECK(reported_slice_percent(42.6f) == std::optional<int>(43));
    CHECK(reported_slice_percent(100.0f) == std::optional<int>(100));
    CHECK(reported_slice_percent(130.0f) == std::optional<int>(100));
}

TEST_CASE("the stage is the last status text that came with a percentage", "[orcamcp][SliceProgress]")
{
    begin_slicing_run();
    const unsigned run = slicing_run_generation();
    note_slicing_status(run, 15, "Generating walls");
    note_slicing_status(run, 50, "Generating support");
    CHECK(slicing_stage_text() == "Generating support");
    begin_slicing_run();
}

TEST_CASE("the stage is kept without the spaces the status text arrives with", "[orcamcp][SliceProgress]")
{
    begin_slicing_run();
    note_slicing_status(slicing_run_generation(), 35, " plate 1:Generating infill toolpath ");
    CHECK(slicing_stage_text() == "plate 1:Generating infill toolpath");
    begin_slicing_run();
}

TEST_CASE("a status update without a percentage or text leaves the stage alone", "[orcamcp][SliceProgress]")
{
    begin_slicing_run();
    const unsigned run = slicing_run_generation();
    note_slicing_status(run, 25, "Generating infill regions");
    note_slicing_status(run, -1, "a warning refresh");
    note_slicing_status(run, 30, "");
    note_slicing_status(run, 30, "   ");
    CHECK(slicing_stage_text() == "Generating infill regions");
    begin_slicing_run();
}

TEST_CASE("a slice that starts forgets the previous slice's stage", "[orcamcp][SliceProgress]")
{
    begin_slicing_run();
    note_slicing_status(slicing_run_generation(), 80, "Exporting G-code");
    begin_slicing_run();
    CHECK(slicing_stage_text().empty());
}

TEST_CASE("an update still queued from a cancelled slice does not bring its stage back", "[orcamcp][SliceProgress]")
{
    begin_slicing_run();
    const unsigned cancelled = slicing_run_generation();
    note_slicing_status(cancelled, 35, "Generating infill toolpath");
    begin_slicing_run(); // the new slice starts before the old one's queued updates are handled
    note_slicing_status(cancelled, 50, "Generating support");
    CHECK(slicing_stage_text().empty());
    note_slicing_status(slicing_run_generation(), 15, "Generating walls");
    CHECK(slicing_stage_text() == "Generating walls");
    begin_slicing_run();
}

TEST_CASE("each slice that starts has a generation of its own", "[orcamcp][SliceProgress]")
{
    begin_slicing_run();
    const unsigned first = slicing_run_generation();
    begin_slicing_run();
    CHECK(slicing_run_generation() != first);
}

TEST_CASE("a slice in progress is running whatever its plates say", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged = judge_slice_run(/*run_known=*/true, /*slicing=*/true, {unsliced_plate(0), gone_plate()},
                                                     std::string("Slice All stopped at plate 1"));
    CHECK(judged.outcome == SliceRunOutcome::running);
}

TEST_CASE("before any slice_all there is no run to judge", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged = judge_slice_run(/*run_known=*/false, /*slicing=*/false, {}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::none);
    CHECK(judged.message.empty());
}

TEST_CASE("a run whose plates all have a result is done", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged =
        judge_slice_run(/*run_known=*/true, /*slicing=*/false, {sliced_plate(0), sliced_plate(1), sliced_plate(2)}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::done);
    CHECK(judged.message.empty());
}

TEST_CASE("a Slice All run that stopped early reports that, with the app's reason", "[orcamcp][SliceProgress]")
{
    const std::string       reason = "Slice All stopped at plate 1: another job (an arrange or an orient) was running";
    const SliceRunJudgement judged = judge_slice_run(/*run_known=*/true, /*slicing=*/false, {sliced_plate(0), unsliced_plate(1)}, reason);
    CHECK(judged.outcome == SliceRunOutcome::ended_early);
    CHECK(judged.message == reason);
}

TEST_CASE("a run that left plates unsliced is incomplete and names them", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged =
        judge_slice_run(/*run_known=*/true, /*slicing=*/false, {sliced_plate(0), unsliced_plate(1), unsliced_plate(3)}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::incomplete);
    CHECK(judged.message.find("plate_index 1, 3 ") != std::string::npos);
}

TEST_CASE("a run whose plates were deleted or rebuilt since is incomplete, and says the plate list changed", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged =
        judge_slice_run(/*run_known=*/true, /*slicing=*/false, {sliced_plate(0), gone_plate(), gone_plate()}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::incomplete);
    CHECK(judged.message.find("2 of the run's 3 plate(s)") != std::string::npos);
    CHECK(judged.message.find("plate list changed") != std::string::npos);
}

TEST_CASE("a run cancelled by a plate-list change also names the plates it left unsliced", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged =
        judge_slice_run(/*run_known=*/true, /*slicing=*/false, {unsliced_plate(0), unsliced_plate(1), gone_plate()}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::incomplete);
    CHECK(judged.message.find("1 of the run's 3 plate(s)") != std::string::npos);
    CHECK(judged.message.find("plate_index 0, 1 has no slice result") != std::string::npos);
}

TEST_CASE("every outcome has the name get_slicing_status reports", "[orcamcp][SliceProgress]")
{
    CHECK(std::string(slice_run_outcome_name(SliceRunOutcome::none)) == "none");
    CHECK(std::string(slice_run_outcome_name(SliceRunOutcome::running)) == "running");
    CHECK(std::string(slice_run_outcome_name(SliceRunOutcome::done)) == "done");
    CHECK(std::string(slice_run_outcome_name(SliceRunOutcome::ended_early)) == "ended_early");
    CHECK(std::string(slice_run_outcome_name(SliceRunOutcome::incomplete)) == "incomplete");
}

// ---- what slice_all reports ---------------------------------------------------------------------
//
// slice_all used to say slicing_started whatever happened: with the previous slice still in progress
// the new one was started and then stopped by the old one's completion, and nothing sliced.

namespace {
SliceStartSignals signals_with(std::vector<PlateToSlice> plates)
{
    SliceStartSignals signals;
    signals.plates = std::move(plates);
    return signals;
}
PlateToSlice unsliced_printable() { return {/*sliced=*/false, /*printable=*/true}; }

SlicingActivity activity(bool is_slicing, bool process_running, int slice_all_plate = -1)
{
    return {is_slicing, process_running, slice_all_plate, /*plate_count=*/5};
}
} // namespace

TEST_CASE("slice_all starts nothing while a slice runs, or while one is still stopping", "[orcamcp][SliceProgress]")
{
    const auto busy = GENERATE(activity(true, true), activity(true, false), activity(false, true), activity(true, true, 2));
    const std::optional<SliceStartReport> refusal = refuse_while_slicing(busy);
    REQUIRE(refusal.has_value());
    CHECK(refusal->status == SliceStart::not_started);
    CHECK(refusal->reason == "busy_slicing");
    CHECK(refusal->message.find("call wait_for_slice, then slice_all again") != std::string::npos);
}

TEST_CASE("slice_all starts a slice when nothing is slicing or stopping", "[orcamcp][SliceProgress]")
{
    CHECK_FALSE(refuse_while_slicing(activity(false, false)).has_value());
}

TEST_CASE("the refusal says which Slice All plate is slicing", "[orcamcp][SliceProgress]")
{
    CHECK(refuse_while_slicing(activity(true, true, 2))->message.find("plate_index 2 of 5") != std::string::npos);
}

TEST_CASE("the refusal says when the previous slice is only still stopping", "[orcamcp][SliceProgress]")
{
    CHECK(refuse_while_slicing(activity(true, false))->message.find("still stopping") != std::string::npos);
    CHECK(refuse_while_slicing(activity(false, true))->message.find("still stopping") != std::string::npos);
}

TEST_CASE("a slice that is running after the dispatch has started", "[orcamcp][SliceProgress]")
{
    SliceStartSignals signals = signals_with({unsliced_printable()});
    signals.slicing           = true;
    const SliceStartReport report = judge_slice_start(signals);
    CHECK(report.status == SliceStart::started);
    CHECK(std::string(slice_start_status_name(report.status)) == "slicing_started");
}

TEST_CASE("plates that already have a result need no slice, which is not a failure to start", "[orcamcp][SliceProgress]")
{
    const SliceStartReport report = judge_slice_start(signals_with({{true, true}, {true, true}}));
    CHECK(report.status == SliceStart::not_started);
    CHECK(report.reason == "already_sliced");
}

TEST_CASE("a slice the UI worker blocks did not start, and names the job", "[orcamcp][SliceProgress]")
{
    SliceStartSignals signals = signals_with({unsliced_printable()});
    signals.ui_job_running    = true;
    const SliceStartReport report = judge_slice_start(signals);
    CHECK(report.status == SliceStart::not_started);
    CHECK(report.reason == "busy_job");
}

TEST_CASE("plates with nothing printable on them give nothing to slice", "[orcamcp][SliceProgress]")
{
    const SliceStartReport report = judge_slice_start(signals_with({{false, false}}));
    CHECK(report.status == SliceStart::not_started);
    CHECK(report.reason == "nothing_to_slice");
}

TEST_CASE("a slice stopped by a new validation error points at active_warnings", "[orcamcp][SliceProgress]")
{
    SliceStartSignals signals = signals_with({unsliced_printable()});
    signals.new_error         = true;
    const SliceStartReport report = judge_slice_start(signals);
    CHECK(report.status == SliceStart::not_started);
    CHECK(report.reason == "invalid");
    CHECK(report.message.find("active_warnings") != std::string::npos);
}

TEST_CASE("a slice that did not start for a reason no signal shows says it did not start", "[orcamcp][SliceProgress]")
{
    const SliceStartReport report = judge_slice_start(signals_with({unsliced_printable()}));
    CHECK(report.status == SliceStart::not_started);
    CHECK(report.reason == "unknown");
    CHECK(std::string(slice_start_status_name(report.status)) == "not_started");
}

TEST_CASE("only an error the attempt raised counts, not one already showing", "[orcamcp][SliceProgress]")
{
    const std::vector<std::string> old_error = {"SlicingError\nPrime Tower is partially outside the printable area"};
    CHECK_FALSE(has_new_error(old_error, old_error));
    CHECK(has_new_error(old_error, {old_error.front(), "SlicingError\nNo object can be printed"}));
    CHECK(has_new_error({}, old_error));
    CHECK_FALSE(has_new_error(old_error, {}));
}
