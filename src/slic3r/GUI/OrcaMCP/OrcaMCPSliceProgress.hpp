// src/slic3r/GUI/OrcaMCP/OrcaMCPSliceProgress.hpp
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
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
    bool ready     = true;  // the app's own checks let it slice (PartPlate::can_slice): no object partly
                            // off the plate or over its height, no filament that cannot print where it is
    bool valid     = true;  // its validation did not fail (PartPlate::is_apply_result_invalid)
    bool selected  = false; // it is the selected plate, the one reslice() works on
    std::optional<std::string> validation_message; // the app's words for a failed validation, when it has them
                                                   // (the selected plate's: its Print is the one validated)
    bool slice_failed = false; // its last slice ended in an error, which stays in active_warnings until the next
                               // (the selected plate's: Plater::last_error_blocks_reslice)
};

enum class SliceRunOutcome
{
    none,        // no slice_all since the app started, or none of its plates is left, and nothing is slicing
    running,     // a slice is in progress
    done,        // every plate the run asked for has a slice result
    ended_early, // Slice All stopped before its last plate (Plater::slice_all_ended_early)
    incomplete,  // the run is over and some of its plates still there have no slice result
    cancelled,   // someone cancelled it: cancel_slice, or the app's Cancel (Plater::slice_cancelled)
};

inline const char* slice_run_outcome_name(SliceRunOutcome outcome)
{
    switch (outcome) {
    case SliceRunOutcome::none: return "none";
    case SliceRunOutcome::running: return "running";
    case SliceRunOutcome::done: return "done";
    case SliceRunOutcome::ended_early: return "ended_early";
    case SliceRunOutcome::incomplete: return "incomplete";
    case SliceRunOutcome::cancelled: return "cancelled";
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

// The run's plates still there that have something on them and no slice result, and those with
// nothing on them, by plate index.
inline void split_unsliced(const std::vector<SliceRunPlate>& plates, std::vector<int>& unsliced, std::vector<int>& skipped, bool& any_sliced)
{
    any_sliced = false;
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
}

// Why a plate the run asked for has no slice result, as far as its state tells now, and where the app's
// own words are when it has some. A validation failure's notification closes once the plate validates,
// and an edit's leaves none: only a slice that failed leaves one in active_warnings.
inline std::string unsliced_reason(const SliceRunPlate& plate)
{
    if (!plate.valid)
        return plate.validation_message ? "the app's validation refuses it: " + *plate.validation_message :
                                          std::string("the app's validation refuses it (slice_all answers with the app's words)");
    if (!plate.ready)
        return "the app will not slice it as it stands: an object partly off the plate or too tall for it, or a filament that "
               "cannot print where it is (get_scene_info's placement says which)";
    if (plate.slice_failed)
        return "its last slice ended in an error (active_warnings has it)";
    return "its slice was stopped or failed (a failure stays in active_warnings until the next slice), or an edit since "
           "invalidated it";
}

// "plate_index 1, 3 has no slice result: <why>" for `unsliced`, the plates with one reason together, in order.
inline std::string unsliced_plates_text(const std::vector<SliceRunPlate>& plates, const std::vector<int>& unsliced)
{
    std::vector<std::pair<std::string, std::vector<int>>> by_reason;
    for (const SliceRunPlate& plate : plates) {
        if (!plate.exists || std::find(unsliced.begin(), unsliced.end(), plate.index) == unsliced.end())
            continue;
        const std::string reason = unsliced_reason(plate);
        auto              group  = std::find_if(by_reason.begin(), by_reason.end(), [&reason](const auto& g) { return g.first == reason; });
        if (group == by_reason.end())
            by_reason.push_back({reason, {plate.index}});
        else
            group->second.push_back(plate.index);
    }
    std::string text;
    for (const auto& [reason, indexes] : by_reason)
        text += (text.empty() ? "" : "; ") + ("plate_index " + plate_index_list(indexes) + " has no slice result: " + reason);
    return text;
}

// A cancelled run's message: who cancelled it where, and the plates it left without a result; the
// plates it sliced before keep theirs. nullopt when it left none: a cancel between plates that were all
// sliced already cost nothing, and the run is judged as any other.
inline std::optional<SliceRunJudgement> cancelled_run(const std::string& cancelled_text, const std::vector<SliceRunPlate>& plates)
{
    std::vector<int> unsliced, skipped;
    bool             any_sliced = false;
    split_unsliced(plates, unsliced, skipped, any_sliced);
    if (unsliced.empty() && any_sliced)
        return std::nullopt;
    std::string message = cancelled_text;
    if (!unsliced.empty())
        message += "; plate_index " + plate_index_list(unsliced) + " has no slice result: call slice_all to slice it";
    return SliceRunJudgement{SliceRunOutcome::cancelled, message, skipped};
}

// How the last slice_all run stands. `run_known` is false before the first slice_all; `plates` are
// the plates it asked for; `ended_early_text` is Plater's report of a Slice All run that stopped early
// (slice_all_ended_early_text), `cancelled_text` of one someone cancelled (slice_cancelled_text). A run
// in progress is running whatever else holds, and a run that was cancelled or ended early says so
// rather than only listing the plates that left unsliced. An empty plate has nothing to slice: it is
// skipped, not unsliced, so a run is done once every plate with something on it is sliced -- and a run
// with nothing to slice on any plate is not done at all.
inline SliceRunJudgement judge_slice_run(bool                              run_known,
                                         bool                              slicing,
                                         const std::vector<SliceRunPlate>& plates,
                                         const std::optional<std::string>& ended_early_text,
                                         const std::optional<std::string>& cancelled_text = std::nullopt)
{
    if (slicing)
        return {SliceRunOutcome::running, {}, {}};
    if (cancelled_text)
        if (std::optional<SliceRunJudgement> cancelled = cancelled_run(*cancelled_text, plates))
            return *cancelled;
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
    split_unsliced(plates, unsliced, skipped, any_sliced);

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
    message += gone > 0 ? "plate_index " + plate_index_list(unsliced) + " has no slice result" : unsliced_plates_text(plates, unsliced);
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

// ---- What cancel_slice does ---------------------------------------------------------------------

// What cancelling does in each pipeline state, as the Cancel on the app's slicing notification does it
// (Plater::cancel_slicing), decided before anything is touched.
enum class SliceCancelAction
{
    cancel,  // a slice is in progress: stop it, which also ends a Slice All run
    end_run, // a Slice All run between two plates, the next one's start already queued: end the run there
    refuse,  // the process exports or uploads, which is no slice: nothing is cancelled
    nothing, // nothing slices, or the last slice already ended and its completion is being taken in
};

inline SliceCancelAction slice_cancel_action(const PipelineState& state)
{
    switch (pipeline_busy(state)) {
    case PipelineBusy::slicing: return state.process_working ? SliceCancelAction::cancel : SliceCancelAction::end_run;
    case PipelineBusy::exporting:
    case PipelineBusy::uploading: return SliceCancelAction::refuse;
    case PipelineBusy::stopping:
    case PipelineBusy::idle: break;
    }
    return SliceCancelAction::nothing;
}

// cancel_slice's words for the actions that cancel nothing.
inline std::string slice_cancel_refusal(const PipelineState& state)
{
    return pipeline_busy_text(state) + ", not a slice, so nothing was cancelled: cancel_slice cancels slicing only; wait_for_slice "
                                       "waits for it to end";
}
inline std::string nothing_to_cancel_text(const PipelineState& state)
{
    if (pipeline_busy(state) == PipelineBusy::stopping)
        return "The last slice already finished or stopped, and the app is taking in how it ended, so nothing was cancelled: "
               "get_slicing_status says how it ended.";
    return "Nothing is slicing, so nothing was cancelled.";
}

// What the app shows right after slice_all dispatched its slice.
struct SliceStartSignals
{
    bool                       slicing        = false; // Plater::is_background_process_slicing()
    bool                       ui_job_running = false; // an arrange or an orient holds the UI worker
    std::optional<std::string> validation_error;       // the selected plate's validation failed, in the app's words;
                                                       // a verdict without words is its SliceRunPlate::valid
    bool                       plugins_missing       = false; // slicing needs plugins not installed or active (Plater::plugins_block_slicing)
    bool                       broken_mixed_filament = false; // a mixed filament of the selected plate lost a component
    bool                       last_slice_failed     = false; // the selected plate's last slice, or the check before it,
                                                               // ended in an error, which reslice() keeps refusing on
                                                               // until the plate changes (Plater::last_error_blocks_reslice)
    std::vector<SliceRunPlate> plates;                 // the plates slice_all asked for, right after it asked
};

namespace detail {
inline SliceStartReport invalid(std::string message) { return {SliceStart::not_started, "invalid", std::move(message)}; }
inline SliceStartReport validation_refusal(const std::string& words) { return invalid("The slice failed validation: " + words); }
inline SliceStartReport plugins_refusal()
{
    return invalid("Slicing needs plugins that are missing, inactive or broken, and the app slices nothing until they are "
                   "resolved (active_warnings names them).");
}
inline SliceStartReport not_ready_refusal(int plate_index)
{
    return invalid("plate_index " + std::to_string(plate_index) +
                   " is not ready to slice, as the app's own checks decide (its Slice button is off too): an object on it is "
                   "partly outside the plate or over its height limit, or a filament cannot print where it is placed. "
                   "active_warnings says which; move the object fully onto or off the plate, or fix the filament, then "
                   "slice_all again.");
}
inline SliceStartReport nothing_to_slice()
{
    return {SliceStart::not_started, "nothing_to_slice",
            "No plate it was asked for has a printable object fully on it: an object marked unprintable, partly "
            "outside its plate or taller than the printable height does not count (get_object_info's on_bed and "
            "placement_warning, and active_warnings, say which)."};
}
} // namespace detail

// Why the app turned the plates down as they stand, or nullopt: the check reslice() stopped on, which it
// does without a word (the GUI greys its Slice button). It works on the selected plate and refuses, in
// this order, on the error that plate's last update or slice left (process_completed_with_error), named
// by what left it; a broken mixed filament; missing plugins; then the validation and readiness its update
// checks. Only then are the run's other plates looked at: Slice All never reaches them. Plates with
// nothing printable on them, which reslice() does not refuse on but slices nothing of, come after every
// refusal the app gives words for, and before a plate's bare validation verdict. An object over the
// printable height is outside its plate (PartPlate::check_outside) and fails Print::validate's height
// check too; when the Print still holds it (printable_height 0, which the build volume takes for no
// limit) the validation is the app's stop and its words are there, so they come first. An object the
// build volume finds too tall is left out of the Print (ModelInstance::is_printable), so its plate keeps a
// verdict without words from the validation before that, and nothing printable is the true answer.
inline std::optional<SliceStartReport> pre_slice_refusal(const SliceStartSignals& signals)
{
    const auto selected = std::find_if(signals.plates.begin(), signals.plates.end(),
                                       [](const SliceRunPlate& p) { return p.exists && p.selected; });
    const bool has_selected      = selected != signals.plates.end();
    const bool selected_unready  = has_selected && selected->printable && !selected->ready;
    const bool nothing_printable = std::none_of(signals.plates.begin(), signals.plates.end(),
                                                [](const SliceRunPlate& p) { return p.exists && p.printable; });
    if (signals.last_slice_failed) {
        if (signals.validation_error)
            return detail::validation_refusal(*signals.validation_error);
        if (signals.plugins_missing)
            return detail::plugins_refusal();
        if (selected_unready)
            return detail::not_ready_refusal(selected->index);
        // The error an object partly off the plate left: nothing on it is printable.
        if (nothing_printable)
            return detail::nothing_to_slice();
        return detail::invalid("The app does not slice the selected plate again until something on it changes: its last "
                               "slice ended in an error (active_warnings has it).");
    }
    if (signals.broken_mixed_filament)
        return detail::invalid("A mixed filament the selected plate uses has lost a component (deleted, or no longer the "
                               "same type), and the app will not slice it: fix it with set_mixed_filament or "
                               "delete_mixed_filament.");
    if (signals.plugins_missing)
        return detail::plugins_refusal();
    if (signals.validation_error)
        return detail::validation_refusal(*signals.validation_error);
    if (selected_unready)
        return detail::not_ready_refusal(selected->index);
    if (nothing_printable)
        return detail::nothing_to_slice();
    for (const SliceRunPlate& plate : signals.plates)
        if (plate.exists && !plate.valid)
            return detail::validation_refusal("plate_index " + std::to_string(plate.index) + " failed validation");
    for (const SliceRunPlate& plate : signals.plates)
        if (plate.exists && plate.printable && !plate.ready)
            return detail::not_ready_refusal(plate.index);
    return std::nullopt;
}

// slice_all's answer once it dispatched a slice. A plate already sliced is not sliced again, which is
// not a failure: nothing needed doing (wait_for_slice then reports done). Otherwise the first cause the
// signals show, a plate with nothing printable on it among them (pre_slice_refusal).
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
                "Another job (an arrange, an orient or a bed fill) is running, so the slice could not start: poll "
                "get_slicing_status until ui_job is null, then call slice_all again."};
    if (const std::optional<SliceStartReport> refusal = pre_slice_refusal(signals))
        return *refusal;
    return {SliceStart::not_started, "unknown", "The app did not start a slice; active_warnings may say why."};
}

// ---- What export_gcode reports ------------------------------------------------------------------

// One export_gcode call as it went (Plater::export_gcode_to_file): what refused it before the export
// was asked for, and, once asked, whether the app scheduled it. The app's export returns without a
// word when the plate fails its own forced validation, so being asked is not having started.
struct ExportStart
{
    bool                       has_objects       = true;
    bool                       already_exporting = false; // BackgroundSlicingProcess::is_export_scheduled() before asking
    std::optional<std::string> validation_error;          // the selected plate's validation failure, in the app's words
    std::optional<std::string> failure;                   // what the update before the export threw
    bool                       checked = true;            // the plate has a slice result, whose G-code the slice checked
    std::optional<std::string> gcode_check_refusal;       // what that check found (OrcaMCPGcodeCheck.hpp)
    std::optional<bool>        scheduled;                 // after asking; nullopt: not asked (yet)
};

// Why the export did not start, or nullopt when it started or nothing has refused it yet.
inline std::optional<std::string> export_not_started(const ExportStart& attempt)
{
    if (!attempt.has_objects)
        return std::string("The scene has no objects, so there is no G-code to export.");
    if (attempt.already_exporting)
        return std::string("Another export job is running.");
    if (attempt.failure)
        return "The export did not start: " + *attempt.failure;
    if (attempt.scheduled.value_or(false))
        return std::nullopt;
    if (attempt.validation_error)
        return "The export did not start: the plate failed validation: " + *attempt.validation_error;
    // The app writes a plate's G-code only once it passed the check its slice ran, as its Export button
    // does; an unsliced plate would be sliced and written unchecked.
    if (!attempt.checked)
        return std::string("The export did not start: the plate has no slice result, so its G-code has not been checked: "
                           "slice_all, then wait_for_slice, first.");
    if (attempt.gcode_check_refusal)
        return "The export did not start: " + *attempt.gcode_check_refusal;
    if (attempt.scheduled.has_value())
        return std::string("The app did not start the export; active_warnings may say why.");
    return std::nullopt;
}

}}} // namespace Slic3r::GUI::OrcaMCP
