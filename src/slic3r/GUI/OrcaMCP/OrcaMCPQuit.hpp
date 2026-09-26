// src/slic3r/GUI/OrcaMCP/OrcaMCPQuit.hpp
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// Quitting while a modal dialog is open.
//
// A modal dialog runs a nested event loop, and every MCP tool call's work runs inside it until the
// user answers. A main frame closed from there is deleted at that loop's idle time, and deletes the
// dialog with it: a dialog that lives on its caller's stack. On 2026-09-26 quit_app, sent while the
// startup "restore unsaved items?" prompt was open, aborted the app that way ("pointer being freed was
// not allocated": ~Plater -> DestroyChildren -> ~MessageDialog). So a close that cannot be vetoed is
// held back until nothing modal is left: it ends the app's own modal dialogs, innermost first, the way
// the app does when the system logs out (EndModal(wxID_ABORT), GUI_App's wxEVT_QUERY_END_SESSION
// handler), and asks again once the event loop has had a turn.
//
// No wx here: the dialogs and the loop are read through hooks, so the tests drive it with fakes
// (tests/slic3rutils/test_mcp_quit.cpp).

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// What is modal right now.
struct ModalState
{
    struct Dialog
    {
        std::uintptr_t id = 0; // which dialog, so that each is ended once
        std::string    title;
    };
    std::vector<Dialog> dialogs;               // the app's own modal dialogs, innermost first
    bool                in_modal_loop = false; // a modal event loop runs: one of those, or the system's

    // A modal loop runs that is none of the app's dialogs: a system file chooser or alert, which the
    // app cannot end.
    bool system_dialog_open() const { return in_modal_loop && dialogs.empty(); }
    bool anything_open() const { return in_modal_loop || !dialogs.empty(); }
    std::vector<std::string> titles() const;
};

// Why quit_app refuses, decided before anything closes; nullopt when it may go ahead.
std::optional<std::string> quit_refusal(const ModalState& modal, bool discard_changes, bool project_dirty);

// What quit_app tells its caller it is about to close unanswered.
std::vector<std::string> quit_notes(const ModalState& modal);

// Holds a close that cannot be vetoed back until nothing modal is left.
class ModalUnwinder
{
public:
    struct Hooks
    {
        std::function<ModalState()>         modal_state;
        std::function<void(std::uintptr_t)> end_dialog; // EndModal(wxID_ABORT) on that dialog
        // Runs a task on the main thread after the event loop has had a turn. Not CallAfter: wx runs an
        // event posted from a pending event in the same pass, before an ended modal loop has returned.
        std::function<void(std::function<void()>)> run_after_a_turn;
    };

    enum class Hold
    {
        go_on,    // nothing modal is open: close now
        held,     // do not close now: the innermost dialog is ended, and the close is asked again later
        given_up, // still modal after max_turns: do not close, and nothing is asked again
    };

    static constexpr int k_default_max_turns = 200;

    explicit ModalUnwinder(Hooks hooks, int max_turns = k_default_max_turns);

    // Asked by the close before anything is torn down; `close` is the close, to ask again.
    Hold hold_back(std::function<void()> close);

private:
    void end_innermost(const ModalState& modal);

    Hooks                       m_hooks;
    int                         m_max_turns;
    int                         m_turns = 0;
    std::vector<std::uintptr_t> m_ended;
};

// The app's modal state (dialogStack and the active event loop) and the app's unwinder, which runs
// its turns on a timer. Defined in OrcaMCPCommon.cpp; main thread only.
ModalState      current_modal_state();
ModalUnwinder&  modal_unwinder();

}}} // namespace Slic3r::GUI::OrcaMCP
