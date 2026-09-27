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
SliceRunPlate sliced_plate(int index) { return {/*exists=*/true, /*printable=*/true, /*sliced=*/true, index}; }
SliceRunPlate unsliced_plate(int index) { return {/*exists=*/true, /*printable=*/true, /*sliced=*/false, index}; }
SliceRunPlate empty_plate(int index) { return {/*exists=*/true, /*printable=*/false, /*sliced=*/false, index}; }
SliceRunPlate gone_plate() { return {/*exists=*/false, /*printable=*/false, /*sliced=*/false, -1}; }
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

// A plate deleted after the run left nothing unsliced: the run is judged by the plates there now.
// Judging a deleted plate as a failure left get_slicing_status's state idle for good after a run
// that sliced everything, once any plate was deleted.
TEST_CASE("a run is judged by the plates still there, so deleting one after it does not undo it", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged =
        judge_slice_run(/*run_known=*/true, /*slicing=*/false, {sliced_plate(0), gone_plate(), gone_plate()}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::done);
    CHECK(judged.message.empty());
    CHECK(slice_state(false, judged.outcome, sliced_plate(0)) == SliceState::done);
}

// new_project and load_project replace every plate: nothing the run asked for is left to judge, and
// the state is the new project's selected plate's (wait_for_slice then goes by it too).
TEST_CASE("a run none of whose plates are left has nothing to judge", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged = judge_slice_run(/*run_known=*/true, /*slicing=*/false, {gone_plate(), gone_plate()}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::none);
    CHECK(slice_state(false, judged.outcome, sliced_plate(0)) == SliceState::done);
    CHECK(slice_state(false, judged.outcome, unsliced_plate(0)) == SliceState::idle);
}

TEST_CASE("a run cancelled by a plate-list change also names the plates it left unsliced", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged =
        judge_slice_run(/*run_known=*/true, /*slicing=*/false, {unsliced_plate(0), unsliced_plate(1), gone_plate()}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::incomplete);
    CHECK(judged.message.find("1 of the run's 3 plate(s)") != std::string::npos);
    CHECK(judged.message.find("plate_index 0, 1 has no slice result") != std::string::npos);
}

// An empty plate has nothing to slice, so Slice All skips it (PlateNotStarted::skipped). Judging it
// unsliced left every run with an empty plate "incomplete", and a wait for done never ended.
TEST_CASE("a run's empty plates are skipped, and the run is done once the others are sliced", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged =
        judge_slice_run(/*run_known=*/true, /*slicing=*/false, {sliced_plate(0), empty_plate(1), sliced_plate(2)}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::done);
    CHECK(judged.message.empty());
    CHECK(judged.skipped == std::vector<int>{1});
}

TEST_CASE("a run that left plates unsliced names those, not the empty ones it skipped", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged =
        judge_slice_run(/*run_known=*/true, /*slicing=*/false, {empty_plate(0), unsliced_plate(1), sliced_plate(2)}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::incomplete);
    CHECK(judged.message.find("plate_index 1 has no slice result") != std::string::npos);
    CHECK(judged.message.find("plate_index 0") == std::string::npos);
    CHECK(judged.skipped == std::vector<int>{0});
}

TEST_CASE("a run none of whose plates has anything to slice is incomplete, with nothing to slice", "[orcamcp][SliceProgress]")
{
    const SliceRunJudgement judged = judge_slice_run(/*run_known=*/true, /*slicing=*/false, {empty_plate(0), empty_plate(1)}, std::nullopt);
    CHECK(judged.outcome == SliceRunOutcome::incomplete);
    CHECK(judged.message.find("nothing to slice") == 0);
    CHECK(judged.message.find("call slice_all again") != std::string::npos);
    CHECK(judged.skipped == std::vector<int>{0, 1});
}

// ---- get_slicing_status's state -----------------------------------------------------------------
//
// state used to be the selected plate's alone: with an empty plate selected it stayed idle after a
// run that sliced every other plate, so a poll for done never ended.

TEST_CASE("a slice in progress is slicing whatever the plates say", "[orcamcp][SliceProgress]")
{
    CHECK(slice_state(/*slicing=*/true, SliceRunOutcome::none, sliced_plate(0)) == SliceState::slicing);
    CHECK(slice_state(/*slicing=*/true, SliceRunOutcome::done, empty_plate(0)) == SliceState::slicing);
}

TEST_CASE("with an empty plate selected, the state is the run's", "[orcamcp][SliceProgress]")
{
    CHECK(slice_state(false, SliceRunOutcome::done, empty_plate(1)) == SliceState::done);
    CHECK(slice_state(false, SliceRunOutcome::incomplete, empty_plate(1)) == SliceState::idle);
    CHECK(slice_state(false, SliceRunOutcome::ended_early, empty_plate(1)) == SliceState::idle);
}

TEST_CASE("a run that is not done is not done, even on a plate that has its result", "[orcamcp][SliceProgress]")
{
    CHECK(slice_state(false, SliceRunOutcome::done, sliced_plate(0)) == SliceState::done);
    CHECK(slice_state(false, SliceRunOutcome::incomplete, sliced_plate(0)) == SliceState::idle);
}

TEST_CASE("a selected plate with objects and no result is idle, whatever the run did", "[orcamcp][SliceProgress]")
{
    // A run over plate 0 alone, then plate 1 selected: the run is done, plate 1 is not sliced.
    CHECK(slice_state(false, SliceRunOutcome::done, unsliced_plate(1)) == SliceState::idle);
}

TEST_CASE("before any slice_all the state is the selected plate's, as before", "[orcamcp][SliceProgress]")
{
    CHECK(slice_state(false, SliceRunOutcome::none, sliced_plate(0)) == SliceState::done);
    CHECK(slice_state(false, SliceRunOutcome::none, unsliced_plate(0)) == SliceState::idle);
    CHECK(slice_state(false, SliceRunOutcome::none, empty_plate(0)) == SliceState::idle);
}

TEST_CASE("every state has the name get_slicing_status reports", "[orcamcp][SliceProgress]")
{
    CHECK(std::string(slice_state_name(SliceState::idle)) == "idle");
    CHECK(std::string(slice_state_name(SliceState::slicing)) == "slicing");
    CHECK(std::string(slice_state_name(SliceState::done)) == "done");
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
SliceStartSignals signals_with(std::vector<SliceRunPlate> plates)
{
    SliceStartSignals signals;
    signals.plates = std::move(plates);
    return signals;
}
SliceRunPlate unsliced_printable() { return unsliced_plate(0); }
SliceRunPlate selected(SliceRunPlate plate)
{
    plate.selected = true;
    return plate;
}

PipelineState pipeline(bool is_slicing, bool working, bool done, bool exporting = false, bool uploading = false, int slice_all_plate = -1)
{
    return {is_slicing, working, done, exporting, uploading, slice_all_plate, /*plate_count=*/5};
}
} // namespace

TEST_CASE("slice_all refuses exactly when the pipeline is busy, in every state", "[orcamcp][SliceProgress]")
{
    // The same predicate is get_slicing_status's busy and what wait_for_slice waits on, so a refusal
    // that says "call wait_for_slice" always finds something to wait for.
    for (int bits = 0; bits < 32; ++bits)
        for (int slice_all_plate : {-1, 2}) {
            const PipelineState state = pipeline(bits & 1, bits & 2, bits & 4, bits & 8, bits & 16, slice_all_plate);
            DYNAMIC_SECTION("state " << bits << " on plate " << slice_all_plate)
            {
                const bool busy = pipeline_busy(state) != PipelineBusy::idle;
                CHECK(refuse_while_busy(state).has_value() == busy);
                if (busy) {
                    CHECK(refuse_while_busy(state)->reason == "busy_slicing");
                    CHECK(refuse_while_busy(state)->message.find(pipeline_busy_text(state)) == 0);
                }
            }
        }
}

TEST_CASE("the pipeline is idle only when nothing slices and the process has nothing left to hand over", "[orcamcp][SliceProgress]")
{
    CHECK(pipeline_busy(pipeline(false, false, false)) == PipelineBusy::idle);
    CHECK(pipeline_busy(pipeline(false, false, false, /*exporting=*/true)) == PipelineBusy::idle);
}

TEST_CASE("a slice or a Slice All run in progress is slicing", "[orcamcp][SliceProgress]")
{
    CHECK(pipeline_busy(pipeline(true, true, false)) == PipelineBusy::slicing);
    CHECK(pipeline_busy(pipeline(true, false, false, false, false, /*slice_all_plate=*/2)) == PipelineBusy::slicing);
    CHECK(pipeline_busy_text(pipeline(true, true, false, false, false, 2)).find("plate_index 2 of 5") != std::string::npos);
}

TEST_CASE("an export or an upload is named as what it is", "[orcamcp][SliceProgress]")
{
    const PipelineState exporting = pipeline(false, true, false, /*exporting=*/true);
    CHECK(pipeline_busy(exporting) == PipelineBusy::exporting);
    CHECK(refuse_while_busy(exporting)->message.find("a G-code export is running") == 0);
    const PipelineState uploading = pipeline(false, true, false, true, /*uploading=*/true);
    CHECK(pipeline_busy(uploading) == PipelineBusy::uploading);
    CHECK(refuse_while_busy(uploading)->message.find("an upload to the printer is running") == 0);
}

TEST_CASE("a slice whose completion is not taken in yet is stopping", "[orcamcp][SliceProgress]")
{
    CHECK(pipeline_busy(pipeline(false, false, /*done=*/true)) == PipelineBusy::stopping);
    CHECK(pipeline_busy(pipeline(/*is_slicing=*/true, false, false)) == PipelineBusy::stopping);
    CHECK(pipeline_busy_text(pipeline(false, false, true)) == "the previous slice is finishing or stopping");
}

TEST_CASE("every busy state has the name get_slicing_status reports", "[orcamcp][SliceProgress]")
{
    CHECK(std::string(pipeline_busy_name(PipelineBusy::idle)) == "idle");
    CHECK(std::string(pipeline_busy_name(PipelineBusy::slicing)) == "slicing");
    CHECK(std::string(pipeline_busy_name(PipelineBusy::exporting)) == "exporting");
    CHECK(std::string(pipeline_busy_name(PipelineBusy::uploading)) == "uploading");
    CHECK(std::string(pipeline_busy_name(PipelineBusy::stopping)) == "stopping");
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
    const SliceStartReport report = judge_slice_start(signals_with({sliced_plate(0), sliced_plate(1)}));
    CHECK(report.status == SliceStart::not_started);
    CHECK(report.reason == "already_sliced");
}

TEST_CASE("an empty plate does not keep the others' results from counting as already sliced", "[orcamcp][SliceProgress]")
{
    const SliceStartReport report = judge_slice_start(signals_with({sliced_plate(0), empty_plate(1)}));
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
    const SliceStartReport report = judge_slice_start(signals_with({empty_plate(0), empty_plate(1)}));
    CHECK(report.status == SliceStart::not_started);
    CHECK(report.reason == "nothing_to_slice");
    // An object partly off its plate is not printable there either, which an agent that sees the object
    // on the plate needs told.
    CHECK(report.message.find("partly outside its plate") != std::string::npos);
}

TEST_CASE("a slice the app's validation stopped is invalid, with the app's message", "[orcamcp][SliceProgress]")
{
    SliceStartSignals signals = signals_with({unsliced_printable()});
    signals.validation_error  = "Prime Tower is partially outside the printable area";
    const SliceStartReport report = judge_slice_start(signals);
    CHECK(report.status == SliceStart::not_started);
    CHECK(report.reason == "invalid");
    CHECK(report.message.find("Prime Tower is partially outside the printable area") != std::string::npos);
}

// reslice() refuses at once, without a word, while the app's own pre-slice checks turn the plate down
// (process_completed_with_error, the checks behind the GUI's greyed Slice button). slice_all called it
// unknown; each refusal is invalid now, and says which check refused and what changes it.
TEST_CASE("a plate the app's own checks turn down is invalid, and says so", "[orcamcp][SliceProgress]")
{
    SliceRunPlate unready = unsliced_plate(2);
    unready.ready         = false;
    const SliceStartReport report = judge_slice_start(signals_with({sliced_plate(0), unready}));
    CHECK(report.status == SliceStart::not_started);
    CHECK(report.reason == "invalid");
    CHECK(report.message.find("plate_index 2 is not ready to slice") != std::string::npos);
    CHECK(report.message.find("partly outside the plate") != std::string::npos);
}

TEST_CASE("an empty plate is never the one reported as not ready", "[orcamcp][SliceProgress]")
{
    SliceRunPlate empty = empty_plate(1);
    empty.ready         = false;
    const SliceStartReport report = judge_slice_start(signals_with({unsliced_plate(0), empty}));
    CHECK(report.reason == "unknown");
}

TEST_CASE("a plate whose last slice failed is invalid until something on it changes", "[orcamcp][SliceProgress]")
{
    SliceStartSignals signals = signals_with({unsliced_printable()});
    signals.last_slice_failed = true;
    const SliceStartReport report = judge_slice_start(signals);
    CHECK(report.reason == "invalid");
    CHECK(report.message.find("last slice") != std::string::npos);
    CHECK(report.message.find("until something on it changes") != std::string::npos);
}

TEST_CASE("missing plugins and a broken mixed filament each name themselves", "[orcamcp][SliceProgress]")
{
    SliceStartSignals plugins = signals_with({unsliced_printable()});
    plugins.plugins_missing   = true;
    const SliceStartReport by_plugins = judge_slice_start(plugins);
    CHECK(by_plugins.reason == "invalid");
    CHECK(by_plugins.message.find("plugins") != std::string::npos);

    SliceStartSignals mixed     = signals_with({unsliced_printable()});
    mixed.broken_mixed_filament = true;
    const SliceStartReport by_mixed = judge_slice_start(mixed);
    CHECK(by_mixed.reason == "invalid");
    CHECK(by_mixed.message.find("mixed filament") != std::string::npos);
}

// Several checks can refuse at once. The report names the one reslice() stopped on, in its order: it
// works on the selected plate, and first refuses on the error that plate's last update or slice left
// (process_completed_with_error), naming what left it; then a broken mixed filament, missing plugins,
// and the validation and readiness its update checks, the selected plate's before the run's others.
TEST_CASE("the refusal reported is the one reslice stopped on, in its order", "[orcamcp][SliceProgress]")
{
    SliceRunPlate first = selected(unsliced_plate(0));
    first.ready         = false;
    SliceRunPlate other = unsliced_plate(1);
    other.ready         = false;
    other.valid         = false;
    SliceStartSignals all     = signals_with({first, other});
    all.validation_error      = "Prime Tower is partially outside the printable area";
    all.plugins_missing       = true;
    all.broken_mixed_filament = true;
    all.last_slice_failed     = true;

    // The last error, told by what left it: the validation, plugins, the plate not ready, else the slice.
    CHECK(judge_slice_start(all).message.find("Prime Tower") != std::string::npos);
    all.validation_error.reset();
    CHECK(judge_slice_start(all).message.find("plugins") != std::string::npos);
    all.plugins_missing = false;
    CHECK(judge_slice_start(all).message.find("plate_index 0 is not ready to slice") != std::string::npos);
    all.plates[0].ready = true;
    CHECK(judge_slice_start(all).message.find("last slice") != std::string::npos);

    // No last error: the mixed filament, plugins, then the selected plate's validation and readiness,
    // then the other plates'.
    all.last_slice_failed = false;
    all.plugins_missing   = true;
    CHECK(judge_slice_start(all).message.find("mixed filament") != std::string::npos);
    all.broken_mixed_filament = false;
    CHECK(judge_slice_start(all).message.find("plugins") != std::string::npos);
    all.plugins_missing  = false;
    all.validation_error = "Prime Tower is partially outside the printable area";
    all.plates[0].ready  = false;
    CHECK(judge_slice_start(all).message.find("Prime Tower") != std::string::npos);
    all.validation_error.reset();
    CHECK(judge_slice_start(all).message.find("plate_index 0 is not ready to slice") != std::string::npos);
    all.plates[0].ready = true;
    CHECK(judge_slice_start(all).message.find("plate_index 1 failed validation") != std::string::npos);
    all.plates[1].valid = true;
    CHECK(judge_slice_start(all).message.find("plate_index 1 is not ready to slice") != std::string::npos);
}

// Slice All selects plate 0 and reslice() stops at once on the error plate 0's last slice left. Naming
// plate 1, which had an object partly off it, sent the caller to fix plate 1 and meet plate 0's error on
// the next call.
TEST_CASE("the selected plate's last error is named before another plate's problem", "[orcamcp][SliceProgress]")
{
    SliceRunPlate off_plate = unsliced_plate(1);
    off_plate.ready         = false;
    SliceStartSignals signals = signals_with({selected(unsliced_plate(0)), off_plate});
    signals.last_slice_failed = true;
    const SliceStartReport report = judge_slice_start(signals);
    CHECK(report.reason == "invalid");
    CHECK(report.message.find("last slice") != std::string::npos);
    CHECK(report.message.find("plate_index 1") == std::string::npos);

    SliceRunPlate failed_validation = unsliced_plate(1);
    failed_validation.valid         = false;
    signals.plates                  = {selected(unsliced_plate(0)), failed_validation};
    CHECK(judge_slice_start(signals).message.find("plate_index 1") == std::string::npos);
}

TEST_CASE("a slice that did not start for a reason no signal shows says it did not start", "[orcamcp][SliceProgress]")
{
    const SliceStartReport report = judge_slice_start(signals_with({unsliced_printable()}));
    CHECK(report.status == SliceStart::not_started);
    CHECK(report.reason == "unknown");
    CHECK(std::string(slice_start_status_name(report.status)) == "not_started");
}

// ---- a settings change the background timer has not taken in yet --------------------------------
//
// The slicer takes in a settings change only when its background timer fires, 0.5 s later. Until then
// slice_all was refused on the last validation failure the change had fixed, and a read of the plate
// could report the result the change had made stale.

TEST_CASE("a pending settings change is applied first only while the pipeline is idle", "[orcamcp][SliceProgress]")
{
    for (int bits = 0; bits < 32; ++bits)
        for (int slice_all_plate : {-1, 2})
            for (bool scheduled : {false, true}) {
                const PipelineState state = pipeline(bits & 1, bits & 2, bits & 4, bits & 8, bits & 16, slice_all_plate);
                DYNAMIC_SECTION("state " << bits << " on plate " << slice_all_plate << (scheduled ? ", scheduled" : ""))
                {
                    const bool idle     = pipeline_busy(state) == PipelineBusy::idle;
                    const bool expected = scheduled ? idle : false;
                    CHECK(should_apply_pending_update(state, scheduled) == expected);
                }
            }
}

// ---- what export_gcode reports -------------------------------------------------------------------
//
// export_gcode said export_started whenever Plater::export_gcode_to_file got as far as asking for
// the export, but the app's export refuses silently on a plate that fails its own (forced)
// validation, and then nothing was ever written.

TEST_CASE("an export that was scheduled has started", "[orcamcp][SliceProgress]")
{
    ExportStart attempt;
    attempt.scheduled = true;
    CHECK_FALSE(export_not_started(attempt).has_value());
}

TEST_CASE("an export the app did not schedule did not start, and says the plate's validation failure", "[orcamcp][SliceProgress]")
{
    ExportStart attempt;
    attempt.scheduled        = false;
    attempt.validation_error = "Layer height cannot exceed nozzle diameter.";
    const std::optional<std::string> why = export_not_started(attempt);
    REQUIRE(why.has_value());
    CHECK(why->find("Layer height cannot exceed nozzle diameter.") != std::string::npos);

    attempt.validation_error.reset();
    REQUIRE(export_not_started(attempt).has_value());
    CHECK(export_not_started(attempt)->find("did not start") != std::string::npos);
}

TEST_CASE("an export is refused before it is asked for, for the first reason that holds", "[orcamcp][SliceProgress]")
{
    ExportStart empty_scene;
    empty_scene.has_objects       = false;
    empty_scene.already_exporting = true;
    CHECK(export_not_started(empty_scene)->find("no objects") != std::string::npos);

    ExportStart busy;
    busy.already_exporting = true;
    CHECK(*export_not_started(busy) == "Another export job is running.");

    ExportStart invalid;
    invalid.validation_error = "Prime Tower is partially outside the printable area";
    CHECK(export_not_started(invalid)->find("Prime Tower is partially outside the printable area") != std::string::npos);

    ExportStart failed;
    failed.failure = "PlaceholderParserError: unknown variable";
    CHECK(export_not_started(failed)->find("unknown variable") != std::string::npos);

    // Nothing refused and not asked for yet: no verdict.
    CHECK_FALSE(export_not_started(ExportStart{}).has_value());
}
