// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceProgress.hpp
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// What get_slicing_status says about a slice while it runs and once it is over. No wx: the tests
// drive it with plain values (tests/slic3rutils/test_slice_progress.cpp).

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// The stage of the slice in progress: the last SlicingStatus::text that Plater::priv::on_slicing_update
// received with a percentage ("Generating walls", "Generating support", ...), in the app's language.
// The GUI shows it only in its slicing notification, which has no getter. GUI thread only: written by
// on_slicing_update, read by get_slicing_status inside run_on_main_thread.
inline std::string& slicing_stage_text()
{
    static std::string text;
    return text;
}

// Which slice a status update belongs to. Every SlicingStatusEvent reads it when it is made, on the
// slicing thread (BackgroundSlicingProcess.hpp), and begin_slicing_run moves it on when a slice
// starts. Updates of a cancelled slice can still be queued when the next one starts, and without it
// they would bring the old stage back.
inline std::atomic<unsigned>& slicing_run_counter()
{
    static std::atomic<unsigned> counter{0};
    return counter;
}
inline unsigned slicing_run_generation() { return slicing_run_counter().load(); }

// A slice starts: BackgroundSlicingProcess::start, which the GUI's Slice button, background processing
// and each plate of a Slice All run all go through, calls this before the slicing thread runs. The
// last slice's stage is forgotten, and its updates still queued are ignored from now on.
inline void begin_slicing_run()
{
    ++slicing_run_counter();
    slicing_stage_text().clear();
}

// Called first thing in on_slicing_update, before upstream prefixes the text and before it drops an
// update that arrives while a UI job runs. An update of an earlier slice (`generation`), or without a
// percentage -- a warning refresh, not a stage -- is ignored. The text arrives as
// " plate 1:Generating walls"; the space is dropped.
inline void note_slicing_status(unsigned generation, int percent, const std::string& text)
{
    const size_t first = text.find_first_not_of(' ');
    if (generation == slicing_run_generation() && percent >= 0 && first != std::string::npos)
        slicing_stage_text() = text.substr(first, text.find_last_not_of(' ') - first + 1);
}

// A plate's PartPlate::get_slicing_percent as get_slicing_status reports it: a whole percentage,
// 0-100, or nullopt for the -1 a plate holds while it has no slice result and nothing is slicing it
// (PartPlate::update_slice_result_valid_state).
inline std::optional<int> reported_slice_percent(float percent)
{
    if (percent < 0.0f)
        return std::nullopt;
    return std::clamp(static_cast<int>(std::lround(percent)), 0, 100);
}

// One plate the last slice_all asked for, as it stands now. slice_all records plates by print index,
// which PartPlateList never reuses, so a plate deleted since, or rebuilt by an undo, a redo or a 3MF
// load, no longer exists under it.
struct SliceRunPlate
{
    bool exists    = false;
    bool printable = false; // it has a printable object on it (PartPlate::has_printable_instances); an
                            // empty plate has nothing to slice, and Slice All skips it
    bool sliced    = false; // it has a valid slice result
    int  index     = -1;    // its 0-based plate index now, when it exists
};

enum class SliceRunOutcome
{
    none,        // no slice_all since the app started, or none of its plates is left, and nothing is slicing
    running,     // a slice is in progress
    done,        // every plate the run asked for has a slice result
    ended_early, // Slice All stopped before its last plate (Plater::slice_all_ended_early)
    incomplete,  // the run is over and some of its plates still there have no slice result
};

inline const char* slice_run_outcome_name(SliceRunOutcome outcome)
{
    switch (outcome) {
    case SliceRunOutcome::none: return "none";
    case SliceRunOutcome::running: return "running";
    case SliceRunOutcome::done: return "done";
    case SliceRunOutcome::ended_early: return "ended_early";
    case SliceRunOutcome::incomplete: return "incomplete";
    }
    return "none";
}

struct SliceRunJudgement
{
    SliceRunOutcome  outcome = SliceRunOutcome::none;
    std::string      message; // why, for ended_early and incomplete; empty otherwise
    std::vector<int> skipped; // the run's plates that are there with nothing to slice, by plate index
};

// "1, 3" for plate indexes {1, 3}.
inline std::string plate_index_list(const std::vector<int>& indexes)
{
    std::string list;
    for (int index : indexes)
        list += (list.empty() ? "" : ", ") + std::to_string(index);
    return list;
}

// How the last slice_all run stands. `run_known` is false before the first slice_all; `plates` are
// the plates it asked for; `ended_early_text` is Plater's report of a Slice All run that stopped early
// (slice_all_ended_early_text). A run in progress is running whatever else holds, and a run that
// ended early says so rather than listing the plates that stop left unsliced. An empty plate has
// nothing to slice: it is skipped, not unsliced, so a run is done once every plate with something on
// it is sliced -- and a run with nothing to slice on any plate is not done at all.
inline SliceRunJudgement judge_slice_run(bool                              run_known,
                                         bool                              slicing,
                                         const std::vector<SliceRunPlate>& plates,
                                         const std::optional<std::string>& ended_early_text)
{
    if (slicing)
        return {SliceRunOutcome::running, {}, {}};
    if (ended_early_text)
        return {SliceRunOutcome::ended_early, *ended_early_text, {}};
    if (!run_known)
        return {SliceRunOutcome::none, {}, {}};

    // Judged by the plates still there: a plate deleted since leaves nothing unsliced, and a run none
    // of whose plates are left (new_project, load_project) has nothing to judge.
    const auto       gone = std::count_if(plates.begin(), plates.end(), [](const SliceRunPlate& p) { return !p.exists; });
    if (size_t(gone) == plates.size())
        return {SliceRunOutcome::none, {}, {}};
    std::vector<int> unsliced, skipped;
    bool             any_sliced = false;
    for (const SliceRunPlate& plate : plates) {
        if (!plate.exists)
            continue;
        if (!plate.printable)
            skipped.push_back(plate.index);
        else if (plate.sliced)
            any_sliced = true;
        else
            unsliced.push_back(plate.index);
    }

    if (unsliced.empty()) {
        if (any_sliced)
            return {SliceRunOutcome::done, {}, skipped};
        return {SliceRunOutcome::incomplete,
                "nothing to slice: no plate the run asked for has a printable object on it (plate_index " +
                    plate_index_list(skipped) + " empty); put one on a plate, then call slice_all again",
                skipped};
    }

    // A plate that went during the run is the likely reason the others were left: the plate-list
    // change cancelled Slice All.
    std::string message;
    if (gone > 0)
        message = std::to_string(gone) + " of the run's " + std::to_string(plates.size()) +
                  " plate(s) no longer exist: the plate list changed (a plate was deleted, or an undo, redo or project "
                  "load rebuilt the list), which cancels Slice All; ";
    message += std::string("plate_index ") + plate_index_list(unsliced) + " has no slice result" +
               (gone > 0 ? "" : ": its slice failed (active_warnings says why), was cancelled, or an edit since invalidated it");
    return {SliceRunOutcome::incomplete, message + "; call slice_all again", skipped};
}

// get_slicing_status's state.
enum class SliceState
{
    idle,    // nothing is slicing, and there is no finished run to read
    slicing, // a slice, or a Slice All run, is in progress
    done,    // the run is done, and the selected plate is sliced or has nothing to slice
};

inline const char* slice_state_name(SliceState state)
{
    switch (state) {
    case SliceState::idle: return "idle";
    case SliceState::slicing: return "slicing";
    case SliceState::done: return "done";
    }
    return "idle";
}

// The state from how the last slice_all run stands (`run`, judge_slice_run's outcome) and the selected
// plate, rather than from the selected plate alone: with an empty plate selected that stayed idle
// after a run that sliced every other plate. A selected plate with objects and no result is idle
// whatever the run did (a run over another plate says nothing about it); before any slice_all, done
// means the selected plate is sliced, as it always did.
inline SliceState slice_state(bool slicing, SliceRunOutcome run, const SliceRunPlate& selected)
{
    if (slicing)
        return SliceState::slicing;
    if (selected.printable && !selected.sliced)
        return SliceState::idle;
    if (run == SliceRunOutcome::none)
        return selected.sliced ? SliceState::done : SliceState::idle;
    return run == SliceRunOutcome::done ? SliceState::done : SliceState::idle;
}

// ---- What slice_all reports ---------------------------------------------------------------------

enum class SliceStart
{
    started,     // a slice is running (or a Slice All run is under way)
    not_started, // nothing new is slicing: see the reason
};

inline const char* slice_start_status_name(SliceStart status)
{
    return status == SliceStart::started ? "slicing_started" : "not_started";
}

struct SliceStartReport
{
    SliceStart  status = SliceStart::not_started;
    std::string reason;  // for not_started: a code an agent can branch on
    std::string message; // what happened, and what to do about it
};

// Whether the slicing pipeline is busy, and with what. The one answer slice_all's refusal,
// get_slicing_status's busy / busy_reason and wait_for_slice's wait all go by, so an agent told to
// wait for the pipeline is never told by the next call that there is nothing to wait for.
enum class PipelineBusy
{
    idle,
    slicing,   // a slice, or a Slice All run, is in progress
    exporting, // the background process writes G-code (an export, with nothing left to slice)
    uploading, // the background process sends G-code to a printer
    stopping,  // a slice finished or was cancelled, and the app has not taken in its completion yet
};

inline const char* pipeline_busy_name(PipelineBusy busy)
{
    switch (busy) {
    case PipelineBusy::idle: return "idle";
    case PipelineBusy::slicing: return "slicing";
    case PipelineBusy::exporting: return "exporting";
    case PipelineBusy::uploading: return "uploading";
    case PipelineBusy::stopping: return "stopping";
    }
    return "idle";
}

// What the app shows about its slicing pipeline.
struct PipelineState
{
    bool is_slicing       = false; // Plater::is_background_process_slicing(): a slice, or a Slice All run, is on
    bool process_working  = false; // the background process is started or running a task
    bool process_done     = false; // it finished or was cancelled, and its completion is not handled yet
    bool export_scheduled = false; // BackgroundSlicingProcess::is_export_scheduled()
    bool upload_scheduled = false; // BackgroundSlicingProcess::is_upload_scheduled()
    int  slice_all_plate  = -1;    // the plate a Slice All run is on (0-based), -1 when none runs
    int  plate_count      = 0;
};

inline PipelineBusy pipeline_busy(const PipelineState& state)
{
    if (state.process_working) {
        if (state.upload_scheduled)
            return PipelineBusy::uploading;
        if (state.export_scheduled && !state.is_slicing)
            return PipelineBusy::exporting;
        return PipelineBusy::slicing;
    }
    // A Slice All run between plates: the next one is about to start.
    if (state.is_slicing && state.slice_all_plate >= 0)
        return PipelineBusy::slicing;
    if (state.process_done || state.is_slicing)
        return PipelineBusy::stopping;
    return PipelineBusy::idle;
}

// The pipeline's state in words, for a busy one.
inline std::string pipeline_busy_text(const PipelineState& state)
{
    switch (pipeline_busy(state)) {
    case PipelineBusy::slicing:
        return state.slice_all_plate >= 0 ? "Slice All is slicing plate_index " + std::to_string(state.slice_all_plate) + " of " +
                                                std::to_string(state.plate_count) + " plate(s)"
                                          : std::string("a slice is in progress");
    case PipelineBusy::exporting: return "a G-code export is running";
    case PipelineBusy::uploading: return "an upload to the printer is running";
    case PipelineBusy::stopping: return "the previous slice is finishing or stopping";
    case PipelineBusy::idle: break;
    }
    return "nothing is slicing";
}

// A settings change reaches the slicer only when its background timer fires, 0.5 s after the change
// (Plater::priv::schedule_background_process). Until then the plate keeps its last result and its last
// validation failure, so slice_all was refused on a failure the change had just fixed. A tool that is
// about to slice, report a slice or export one applies the change first, as the timer would
// (Plater::apply_pending_background_update) -- but only while the pipeline is idle: a busy one is left
// alone, as slice_all leaves it (refuse_while_busy).
inline bool should_apply_pending_update(const PipelineState& state, bool update_scheduled)
{
    return update_scheduled && pipeline_busy(state) == PipelineBusy::idle;
}

// slice_all while the pipeline is busy starts nothing: a slice started then is stopped by the
// previous one's completion (Plater::priv::on_process_completed stops the process), so it would never
// slice. The answer says what is going on and what to do. nullopt: the pipeline is idle.
inline std::optional<SliceStartReport> refuse_while_busy(const PipelineState& state)
{
    if (pipeline_busy(state) == PipelineBusy::idle)
        return std::nullopt;
    return SliceStartReport{SliceStart::not_started, "busy_slicing",
                            pipeline_busy_text(state) + ", so nothing was started: call wait_for_slice, then slice_all again"};
}

// What the app shows right after slice_all dispatched its slice.
struct SliceStartSignals
{
    bool                       slicing        = false; // Plater::is_background_process_slicing()
    bool                       ui_job_running = false; // an arrange or an orient holds the UI worker
    std::optional<std::string> validation_error;       // the app's own validation of a plate asked for failed, saying this
    std::vector<SliceRunPlate> plates;                 // the plates slice_all asked for, right after it asked
};

// slice_all's answer once it dispatched a slice. A plate already sliced is not sliced again, which is
// not a failure: nothing needed doing (wait_for_slice then reports done). An empty plate needs no
// slice either. Otherwise the first cause the signals show.
inline SliceStartReport judge_slice_start(const SliceStartSignals& signals)
{
    if (signals.slicing)
        return {SliceStart::started, {}, {}};
    const auto printable  = [](const SliceRunPlate& p) { return p.exists && p.printable; };
    const bool all_sliced = std::any_of(signals.plates.begin(), signals.plates.end(), printable) &&
                            std::all_of(signals.plates.begin(), signals.plates.end(),
                                        [&printable](const SliceRunPlate& p) { return !printable(p) || p.sliced; });
    if (all_sliced)
        return {SliceStart::not_started, "already_sliced",
                "Every plate it was asked for already has a valid slice result, so there was nothing to slice: "
                "get_print_estimate reads it."};
    if (signals.ui_job_running)
        return {SliceStart::not_started, "busy_job",
                "Another job (an arrange or an orient) is running, so the slice could not start: call slice_all again "
                "once it has finished."};
    if (std::none_of(signals.plates.begin(), signals.plates.end(), printable))
        return {SliceStart::not_started, "nothing_to_slice", "No plate it was asked for has a printable object on it."};
    if (signals.validation_error)
        return {SliceStart::not_started, "invalid", "The slice failed validation: " + *signals.validation_error};
    return {SliceStart::not_started, "unknown", "The app did not start a slice; active_warnings may say why."};
}

}}} // namespace Slic3r::GUI::OrcaMCP
