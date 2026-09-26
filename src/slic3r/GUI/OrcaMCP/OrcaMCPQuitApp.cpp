// src/slic3r/GUI/OrcaMCP/OrcaMCPQuitApp.cpp
// The app's side of OrcaMCPQuit.hpp: the modal dialogs as wx shows them, ending the innermost one
// unanswered, and the timer the unwinder takes its turns on.
#include "OrcaMCPQuit.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/log/trivial.hpp>

#include <wx/colordlg.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/evtloop.h>
#include <wx/filedlg.h>
#include <wx/fontdlg.h>
#include <wx/modalhook.h>
#include <wx/msgdlg.h>
#include <wx/timer.h>
#include <wx/utils.h>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// Native panels: the system runs their modal loop, and EndModal cannot end them.
bool is_system_dialog(const wxDialog* dialog)
{
    return dynamic_cast<const wxMessageDialogBase*>(dialog) != nullptr ||
           dynamic_cast<const wxFileDialogBase*>(dialog) != nullptr ||
           dynamic_cast<const wxDirDialogBase*>(dialog) != nullptr ||
           dynamic_cast<const wxColourDialog*>(dialog) != nullptr ||
           dynamic_cast<const wxFontDialogBase*>(dialog) != nullptr;
}

std::uintptr_t key_of(const wxDialog* dialog) { return reinterpret_cast<std::uintptr_t>(dialog); }

// wx calls Enter before, and Exit after, every ShowModal: the app's dialogs, plain wxDialog subclasses
// included, and the native ones.
class TrackedModalDialogs : public wxModalDialogHook
{
public:
    ModalStack stack;

protected:
    int Enter(wxDialog* dialog) override
    {
        stack.entered(key_of(dialog), into_u8(dialog->GetTitle()), is_system_dialog(dialog));
        return wxID_NONE; // show it
    }
    void Exit(wxDialog* dialog) override { stack.exited(key_of(dialog)); }
};

TrackedModalDialogs& tracked_dialogs()
{
    // Never destroyed: a dialog can still open or close while the app object is torn down.
    static TrackedModalDialogs* const tracked = new TrackedModalDialogs;
    return *tracked;
}

// Runs one task on the main thread once the event loop has had a turn. One close is held back at a
// time, so one timer serves it: each turn replaces the task.
class TurnTimer : public wxTimer
{
public:
    void run_after_a_turn(std::function<void()> task)
    {
        m_task = std::move(task);
        StartOnce(k_turn_ms);
    }
    void cancel()
    {
        Stop();
        m_task = nullptr;
    }

private:
    static constexpr int k_turn_ms = 50; // ModalUnwinder::k_default_max_turns of these is 10 s

    void Notify() override
    {
        std::function<void()> task = std::move(m_task);
        m_task                     = nullptr;
        if (task)
            task();
    }

    std::function<void()> m_task;
};

// A No button that still says No, or Cancel (Tab's "delete preset?" relabels it so), in English or
// translated; not one relabelled as a choice.
bool refuses(const wxWindow* no_button)
{
    const wxString label = wxStripMenuCodes(no_button->GetLabel()).Trim().Trim(false);
    for (const wxString& refusal : {wxString("No"), _L("No"), wxString("Cancel"), _L("Cancel")})
        if (label.CmpNoCase(refusal) == 0)
            return true;
    return false;
}

int decline_id(const wxDialog* dialog)
{
    const wxWindow* no_button = dialog->FindWindow(wxID_NO);
    const Decline   decline   = decline_answer(dialog->FindWindow(wxID_CANCEL) != nullptr, no_button != nullptr,
                                               no_button != nullptr && refuses(no_button));
    return decline == Decline::no ? wxID_NO : wxID_CANCEL;
}

// Ends that showing of a dialog with its own "no", while it is the innermost one.
void end_dialog_unanswered(std::uint64_t id)
{
    const auto key = tracked_dialogs().stack.innermost_key(id);
    if (!key)
        return;
    auto* const dialog = reinterpret_cast<wxDialog*>(*key);
    const int   answer = decline_id(dialog);
    BOOST_LOG_TRIVIAL(info) << "OrcaMCP: closing the dialog '" << into_u8(dialog->GetTitle()) << "' unanswered ("
                            << (answer == wxID_NO ? "No" : "Cancel") << "), to quit";
    set_closing_dialogs_to_quit(true);
    dialog->EndModal(answer);
}

bool in_nested_event_loop()
{
    const wxEventLoopBase* loop = wxEventLoopBase::GetActive();
    return loop != nullptr && !loop->IsMain();
}

TurnTimer& turn_timer()
{
    static TurnTimer* const timer = new TurnTimer; // never destroyed, like the unwinder it serves
    return *timer;
}

} // namespace

void track_modal_dialogs()
{
    static const bool registered = (tracked_dialogs().Register(), true);
    (void) registered;
}

ModalState current_modal_state()
{
    ModalState state;
    state.dialogs       = tracked_dialogs().stack.innermost_first();
    state.in_modal_loop = in_nested_event_loop();
    return state;
}

ModalUnwinder& modal_unwinder()
{
    // Never destroyed, like the MCP gate: a close can be held back while the app object is torn down.
    static ModalUnwinder* const unwinder = new ModalUnwinder(
        {current_modal_state, end_dialog_unanswered,
         [](std::function<void()> task) { turn_timer().run_after_a_turn(std::move(task)); },
         [] { turn_timer().cancel(); }, session_ending});
    return *unwinder;
}

bool hold_close_while_modal(std::function<void()> close)
{
    ModalUnwinder&            unwinder = modal_unwinder();
    const ModalUnwinder::Hold hold     = unwinder.hold_back(std::move(close));
    const int                 turns    = unwinder.turns();
    switch (hold) {
    case ModalUnwinder::Hold::held:
        if (turns == 1)
            BOOST_LOG_TRIVIAL(info) << "OrcaMCP: the close waits for the open dialogs to end";
        return true;
    case ModalUnwinder::Hold::go_on:
        if (turns > 0)
            BOOST_LOG_TRIVIAL(info) << "OrcaMCP: the open dialogs have ended; the close goes on";
        return false;
    case ModalUnwinder::Hold::given_up:
        note_quit_failed(current_modal_state());
        set_closing_dialogs_to_quit(false);
        BOOST_LOG_TRIVIAL(error) << "OrcaMCP: " << quit_failed_warning()->at("message").get<std::string>();
        return true;
    }
    return true;
}

}}} // namespace Slic3r::GUI::OrcaMCP
