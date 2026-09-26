// src/slic3r/GUI/OrcaMCP/OrcaMCPQuit.hpp
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// Quitting while a modal dialog is open, and telling an agent that one is.
//
// A modal dialog runs a nested event loop, and every MCP tool call's work runs inside it until the
// user answers. A main frame closed from there is deleted at that loop's idle time, and deletes the
// dialog with it: a dialog that lives on its caller's stack. On 2026-09-26 quit_app, sent while the
// startup "restore unsaved items?" prompt was open, aborted the app that way ("pointer being freed was
// not allocated": ~Plater -> DestroyChildren -> ~MessageDialog). So a close that cannot be vetoed is
// held back until nothing modal is left: it ends the innermost dialog unanswered, one per turn of the
// event loop, and closes once the main loop runs again.
//
// "Unanswered" is each dialog's own way of saying no: its Cancel button if it has one; else its No
// button, if that still says No (or Cancel); else Cancel, which is what its close box returns. Never
// an answer its caller takes for yes: wxID_ABORT, which the app used at a system logout, made "Sync
// printer information?" sync and the unsaved-presets dialog go on. A No relabelled as a choice ("Right:
// 0.6 mm", "Ignore") is not a no. A caller that must tell "the app quit" from "the user said no" asks
// closing_dialogs_to_quit().
//
// No wx here: the dialogs and the loop are read through hooks, so the tests drive it with fakes
// (tests/slic3rutils/test_mcp_quit.cpp). The wx side is in OrcaMCPQuitApp.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// What is modal right now.
struct ModalState
{
    struct Dialog
    {
        std::uint64_t id = 0; // this showing of the dialog: a dialog shown again, or at a dead one's address, is new
        std::string   title;
        bool          system = false; // a native alert, file chooser, colour or font panel: only the user closes it
    };
    std::vector<Dialog> dialogs;               // every modal dialog open, the app's and the system's, innermost first
    bool                in_modal_loop = false; // an event loop other than the main one runs

    bool system_dialog_open() const;
    // A modal loop runs that no dialog accounts for: nothing the app can end.
    bool untracked_modal_loop() const { return in_modal_loop && dialogs.empty(); }
    bool anything_open() const { return in_modal_loop || !dialogs.empty(); }
    // The app's own dialogs, innermost first.
    std::vector<std::string> titles() const;
};

// The modal dialogs in the order they opened. Fed by a wxModalDialogHook, which wx calls for every
// ShowModal: the app's DPIDialogs, its plain wxDialog subclasses (which never enter dialogStack) and
// the native ones. Each showing gets an id of its own.
class ModalStack
{
public:
    void entered(std::uintptr_t key, std::string title, bool system);
    void exited(std::uintptr_t key);
    std::vector<ModalState::Dialog> innermost_first() const;
    // The dialog to end, while it is the innermost one; nothing otherwise.
    std::optional<std::uintptr_t> innermost_key(std::uint64_t id) const;

private:
    struct Entry
    {
        std::uintptr_t     key;
        ModalState::Dialog dialog;
    };
    std::vector<Entry> m_entries; // outermost first
    std::uint64_t      m_next_id = 1;
};

// How a dialog is closed unanswered, from the buttons it has. `no_button_refuses`: its No button is
// still labelled No (or Cancel), not relabelled as one of the choices.
enum class Decline { cancel, no };
Decline decline_answer(bool has_cancel_button, bool has_no_button, bool no_button_refuses);

// True from the moment a quit (quit_app) ends a dialog unanswered, until that quit gives up. The
// restore prompt reads it to keep its backup rather than delete it.
bool closing_dialogs_to_quit();
void set_closing_dialogs_to_quit(bool closing);

// The system session is ending: the process ends as soon as the event that says so returns, so a close
// must never wait for a dialog then.
void mark_session_ending();
bool session_ending();

// The startup "restore unsaved items?" prompt is open while one of these lives.
class RestorePromptOpen
{
public:
    RestorePromptOpen();
    ~RestorePromptOpen();
    RestorePromptOpen(const RestorePromptOpen&)            = delete;
    RestorePromptOpen& operator=(const RestorePromptOpen&) = delete;
};
bool restore_prompt_open();

// Why new_project / load_project refuse: while the restore prompt waits, they would point the app's
// record of the backup it offers (last_backup_path) at the new project's, and orphan it.
std::optional<std::string> pending_restore_refusal();

// Why quit_app refuses, decided before anything closes; nullopt when it may go ahead.
std::optional<std::string> quit_refusal(const ModalState& modal, bool discard_changes, bool project_dirty);

// What quit_app tells its caller it is about to close unanswered.
std::vector<std::string> quit_notes(const ModalState& modal);

// get_scene_info's `open_dialogs` (the app's dialogs' titles), `system_dialog_open` and
// `untracked_modal_loop`.
void add_open_dialogs(nlohmann::json& result, const ModalState& modal);

// The active_warnings entry while something modal is open; nullopt otherwise.
std::optional<nlohmann::json> open_dialog_warning(const ModalState& modal);

// A quit that gave up: noted with what was still open, reported in active_warnings until the next
// quit_app, and cleared by it.
void                          note_quit_failed(const ModalState& still_open);
void                          clear_quit_failure();
std::optional<nlohmann::json> quit_failed_warning();

// Holds a close that cannot be vetoed back until nothing modal is left.
class ModalUnwinder
{
public:
    struct Hooks
    {
        std::function<ModalState()>        modal_state;
        std::function<void(std::uint64_t)> end_dialog; // ends that dialog unanswered
        // Runs a task on the main thread after the event loop has had a turn. Not CallAfter: wx runs an
        // event posted from a pending event in the same pass, before an ended modal loop has returned.
        std::function<void(std::function<void()>)> run_after_a_turn;
        std::function<void()>                      cancel_turn; // drops the task run_after_a_turn holds
        std::function<bool()>                      session_ending;
    };

    enum class Hold
    {
        go_on,    // nothing modal is open (any more): close now
        held,     // do not close now: the innermost dialog is ended, and the close is asked again later
        given_up, // still modal after max_turns: do not close, and nothing is asked again
    };

    static constexpr int k_default_max_turns = 200;

    explicit ModalUnwinder(Hooks hooks, int max_turns = k_default_max_turns);

    // Asked by the close before anything is torn down; `close` is the close, to ask again.
    Hold hold_back(std::function<void()> close);

    // The turns the latest hold has taken: 1 on its first, and still its count on the go_on or
    // given_up that ends it; 0 for a close that was never held. Its log lines are told apart by it.
    int turns() const { return m_turns; }

private:
    Hold finish(Hold hold);
    void end_innermost(const ModalState& modal);

    Hooks                      m_hooks;
    int                        m_max_turns;
    int                        m_turns = 0;
    bool                       m_finished = false; // the latest hold has ended: the next close starts afresh
    std::vector<std::uint64_t> m_ended;
};

// A quit request from the system (the Dock's Quit, a quit Apple Event, a logout, wx's
// wxEVT_QUERY_END_SESSION) is refused while anything modal is open, as wx's own macOS handler and macOS
// apps do: it arrives inside the dialog's event loop, where closing the frame is the abort this file is
// about. The user answers the dialog and quits again; nothing is torn down, so nothing is lost.
bool refuse_session_end(const ModalState& modal, bool can_veto);

// The app's side, defined in OrcaMCPQuitApp.cpp; main thread only.
void           track_modal_dialogs();   // registers the hook that feeds the app's ModalStack; once, at startup
ModalState     current_modal_state();
ModalUnwinder& modal_unwinder();        // runs its turns on a timer
// Asked by the main frame's close handler, for a close that cannot be vetoed, before anything is torn
// down: true while a dialog is open, when `close` is asked again after a turn (the innermost dialog
// ended); false when the close may go on now. Logs the first turn and the outcome, and notes a quit
// that gave up for active_warnings.
bool hold_close_while_modal(std::function<void()> close);

}}} // namespace Slic3r::GUI::OrcaMCP
