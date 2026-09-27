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
    bool exists = false;
    bool sliced = false; // it has a valid slice result
    int  index  = -1;    // its 0-based plate index now, when it exists
};

enum class SliceRunOutcome
{
    none,        // no slice_all since the app started, and nothing is slicing
    running,     // a slice is in progress
    done,        // every plate the run asked for has a slice result
    ended_early, // Slice All stopped before its last plate (Plater::slice_all_ended_early)
    incomplete,  // the run is over and some of its plates have no slice result, or are gone
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
    SliceRunOutcome outcome = SliceRunOutcome::none;
    std::string     message; // why, for ended_early and incomplete; empty otherwise
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
// ended early says so rather than listing the plates that stop left unsliced.
inline SliceRunJudgement judge_slice_run(bool                              run_known,
                                         bool                              slicing,
                                         const std::vector<SliceRunPlate>& plates,
                                         const std::optional<std::string>& ended_early_text)
{
    if (slicing)
        return {SliceRunOutcome::running, {}};
    if (ended_early_text)
        return {SliceRunOutcome::ended_early, *ended_early_text};
    if (!run_known)
        return {SliceRunOutcome::none, {}};

    const auto       gone = std::count_if(plates.begin(), plates.end(), [](const SliceRunPlate& p) { return !p.exists; });
    std::vector<int> unsliced;
    for (const SliceRunPlate& plate : plates)
        if (plate.exists && !plate.sliced)
            unsliced.push_back(plate.index);

    if (gone == 0 && unsliced.empty())
        return {SliceRunOutcome::done, {}};

    std::string message;
    if (gone > 0)
        message = std::to_string(gone) + " of the run's " + std::to_string(plates.size()) +
                  " plate(s) no longer exist: the plate list changed (a plate was deleted, or an undo, redo or project "
                  "load rebuilt the list), which cancels Slice All";
    if (!unsliced.empty())
        message += (message.empty() ? "" : "; ") + std::string("plate_index ") + plate_index_list(unsliced) + " has no slice result" +
                   (gone > 0 ? "" : ": its slice failed (active_warnings says why), was cancelled, or an edit since invalidated it");
    return {SliceRunOutcome::incomplete, message + "; call slice_all again"};
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

// What the app is doing when slice_all is called.
struct SlicingActivity
{
    bool is_slicing      = false; // Plater::is_background_process_slicing(): a slice, or a Slice All run, is on
    bool process_running = false; // the background process is not idle: slicing, or finished or cancelled and
                                  // its completion not yet handled
    int  slice_all_plate = -1;    // the plate a Slice All run is on (0-based), -1 when none runs
    int  plate_count     = 0;
};

// slice_all while anything is slicing, or still stopping, starts nothing: a slice started then is
// stopped by the previous one's completion (Plater::priv::on_process_completed stops the process), so
// the new one would never slice. The answer says what is going on and what to do. nullopt: idle.
inline std::optional<SliceStartReport> refuse_while_slicing(const SlicingActivity& activity)
{
    if (!activity.is_slicing && !activity.process_running)
        return std::nullopt;
    std::string state;
    if (activity.slice_all_plate >= 0)
        state = "Slice All is slicing plate_index " + std::to_string(activity.slice_all_plate) + " of " +
                std::to_string(activity.plate_count) + " plate(s)";
    else if (activity.is_slicing && activity.process_running)
        state = "a slice is in progress";
    else
        state = "the previous slice is still stopping";
    return SliceStartReport{SliceStart::not_started, "busy_slicing",
                            state + ", so nothing was started: call wait_for_slice, then slice_all again"};
}

// One plate slice_all asked for, right after it asked.
struct PlateToSlice
{
    bool sliced    = false; // it has a valid slice result
    bool printable = false; // it has a printable object on it
};

// What the app shows right after slice_all dispatched its slice.
struct SliceStartSignals
{
    bool                      slicing          = false; // Plater::is_background_process_slicing()
    bool                      ui_job_running   = false; // an arrange or an orient holds the UI worker
    bool                      new_error        = false; // an error-level warning that was not there before
    std::vector<PlateToSlice> plates;
};

// Whether `after` holds an error-level warning `before` did not: one that the attempt raised, rather
// than an old one still showing. Each entry names one error warning (its type and message).
inline bool has_new_error(const std::vector<std::string>& before, const std::vector<std::string>& after)
{
    return std::any_of(after.begin(), after.end(),
                       [&before](const std::string& error) { return std::find(before.begin(), before.end(), error) == before.end(); });
}

// slice_all's answer once it dispatched a slice. A plate already sliced is not sliced again, which is
// not a failure: nothing needed doing (wait_for_slice then reports done). Otherwise the first cause
// the signals show.
inline SliceStartReport judge_slice_start(const SliceStartSignals& signals)
{
    if (signals.slicing)
        return {SliceStart::started, {}, {}};
    const bool all_sliced = !signals.plates.empty() &&
                            std::all_of(signals.plates.begin(), signals.plates.end(), [](const PlateToSlice& p) { return p.sliced; });
    if (all_sliced)
        return {SliceStart::not_started, "already_sliced",
                "Every plate it was asked for already has a valid slice result, so there was nothing to slice: "
                "get_print_estimate reads it."};
    if (signals.ui_job_running)
        return {SliceStart::not_started, "busy_job",
                "Another job (an arrange or an orient) is running, so the slice could not start: call slice_all again "
                "once it has finished."};
    if (std::none_of(signals.plates.begin(), signals.plates.end(), [](const PlateToSlice& p) { return p.printable; }))
        return {SliceStart::not_started, "nothing_to_slice", "No plate it was asked for has a printable object on it."};
    if (signals.new_error)
        return {SliceStart::not_started, "invalid", "The slice failed validation: active_warnings says why."};
    return {SliceStart::not_started, "unknown", "The app did not start a slice; active_warnings may say why."};
}

}}} // namespace Slic3r::GUI::OrcaMCP
