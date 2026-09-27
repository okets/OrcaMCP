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

}}} // namespace Slic3r::GUI::OrcaMCP
