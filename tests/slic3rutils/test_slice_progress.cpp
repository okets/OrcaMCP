#include <catch2/catch_test_macros.hpp>

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
    forget_slicing_stage();
    note_slicing_status(15, "Generating walls");
    note_slicing_status(50, "Generating support");
    CHECK(slicing_stage_text() == "Generating support");
    forget_slicing_stage();
}

TEST_CASE("the stage is kept without the spaces the status text arrives with", "[orcamcp][SliceProgress]")
{
    forget_slicing_stage();
    note_slicing_status(35, " plate 1:Generating infill toolpath ");
    CHECK(slicing_stage_text() == "plate 1:Generating infill toolpath");
    forget_slicing_stage();
}

TEST_CASE("a status update without a percentage or text leaves the stage alone", "[orcamcp][SliceProgress]")
{
    forget_slicing_stage();
    note_slicing_status(25, "Generating infill regions");
    note_slicing_status(-1, "a warning refresh");
    note_slicing_status(30, "");
    note_slicing_status(30, "   ");
    CHECK(slicing_stage_text() == "Generating infill regions");
    forget_slicing_stage();
}

TEST_CASE("slice_all forgets the previous run's stage", "[orcamcp][SliceProgress]")
{
    note_slicing_status(80, "Exporting G-code");
    forget_slicing_stage();
    CHECK(slicing_stage_text().empty());
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
