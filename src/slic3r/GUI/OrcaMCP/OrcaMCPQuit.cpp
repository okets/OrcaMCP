#include "OrcaMCPQuit.hpp"

#include <algorithm>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// 'A', 'B' and 'C'
std::string quoted_list(const std::vector<std::string>& titles)
{
    std::string list;
    for (size_t i = 0; i < titles.size(); ++i) {
        if (i > 0)
            list += i + 1 == titles.size() ? " and " : ", ";
        list += "'" + titles[i] + "'";
    }
    return list;
}

const char* const k_system_dialog =
    "a system dialog (a file chooser, an alert, a colour or font panel), which the app cannot close itself";
const char* const k_untracked_loop = "a modal window the app cannot identify, and so cannot close";

// What is open, for a message: the app's dialogs by name, then the ones it cannot close.
std::string describe(const ModalState& modal)
{
    std::vector<std::string> parts;
    if (!modal.titles().empty())
        parts.push_back("the dialog " + quoted_list(modal.titles()));
    if (modal.system_dialog_open())
        parts.push_back(k_system_dialog);
    if (modal.untracked_modal_loop())
        parts.push_back(k_untracked_loop);
    std::string text;
    for (size_t i = 0; i < parts.size(); ++i)
        text += (i > 0 ? ", and " : "") + parts[i];
    return text;
}

bool                       g_closing_dialogs_to_quit = false;
bool                       g_session_ending          = false;
std::optional<std::string> g_quit_failure;

} // namespace

bool ModalState::system_dialog_open() const
{
    return std::any_of(dialogs.begin(), dialogs.end(), [](const Dialog& dialog) { return dialog.system; });
}

std::vector<std::string> ModalState::titles() const
{
    std::vector<std::string> out;
    for (const Dialog& dialog : dialogs)
        if (!dialog.system)
            out.push_back(dialog.title);
    return out;
}

void ModalStack::entered(std::uintptr_t key, std::string title, bool system)
{
    m_entries.push_back({key, {m_next_id++, std::move(title), system}});
}

void ModalStack::exited(std::uintptr_t key)
{
    // The most recent showing of that dialog: modal loops nest, so it is the innermost one of that key.
    auto it = std::find_if(m_entries.rbegin(), m_entries.rend(), [key](const Entry& entry) { return entry.key == key; });
    if (it != m_entries.rend())
        m_entries.erase(std::next(it).base());
}

std::vector<ModalState::Dialog> ModalStack::innermost_first() const
{
    std::vector<ModalState::Dialog> out;
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
        out.push_back(it->dialog);
    return out;
}

std::optional<std::uintptr_t> ModalStack::innermost_key(std::uint64_t id) const
{
    if (m_entries.empty() || m_entries.back().dialog.id != id)
        return std::nullopt;
    return m_entries.back().key;
}

Decline decline_answer(bool has_cancel_button, bool has_no_button, bool no_button_refuses)
{
    return !has_cancel_button && has_no_button && no_button_refuses ? Decline::no : Decline::cancel;
}

bool closing_dialogs_to_quit() { return g_closing_dialogs_to_quit; }
void set_closing_dialogs_to_quit(bool closing) { g_closing_dialogs_to_quit = closing; }

void mark_session_ending() { g_session_ending = true; }
bool session_ending() { return g_session_ending; }

std::optional<std::string> quit_refusal(const ModalState& modal, bool discard_changes, bool project_dirty)
{
    // What no answer can get past comes first.
    if (modal.system_dialog_open())
        return std::string("the app is showing ") + k_system_dialog + "; close it in the app, then call quit_app again";
    if (modal.untracked_modal_loop())
        return std::string("the app is showing ") + k_untracked_loop + "; close it in the app, then call quit_app again";
    if (!discard_changes && project_dirty)
        return std::string("project has unsaved changes; call save_project first or pass discard_changes=true");
    if (!discard_changes && !modal.titles().empty())
        return "a dialog is open in the app: " + quoted_list(modal.titles()) +
               "; answer it there, or pass discard_changes=true to close it unanswered";
    return std::nullopt;
}

std::vector<std::string> quit_notes(const ModalState& modal)
{
    std::vector<std::string> notes;
    for (const std::string& title : modal.titles())
        notes.push_back("The dialog '" + title + "' was open; it is closed unanswered, as when the system logs out.");
    return notes;
}

void add_open_dialogs(nlohmann::json& result, const ModalState& modal)
{
    result["open_dialogs"]       = modal.titles();
    result["system_dialog_open"] = modal.system_dialog_open();
}

std::optional<nlohmann::json> open_dialog_warning(const ModalState& modal)
{
    if (!modal.anything_open())
        return std::nullopt;
    return nlohmann::json{
        {"level", "warning"},
        {"type", "OpenDialog"},
        {"message", "The app is showing " + describe(modal) + ", waiting for the user. Tool calls still run while it "
                    "is open; quit_app closes the app's own dialogs unanswered."},
    };
}

void note_quit_failed(const ModalState& still_open)
{
    g_quit_failure = "quit_app could not close the app: " + describe(still_open) +
                     " is still open 10 s after it was closed unanswered. The app is still running and its unsaved "
                     "changes are kept; close the dialog in the app, then call quit_app again.";
}

void clear_quit_failure() { g_quit_failure.reset(); }

std::optional<nlohmann::json> quit_failed_warning()
{
    if (!g_quit_failure)
        return std::nullopt;
    return nlohmann::json{{"level", "error"}, {"type", "QuitFailed"}, {"message", *g_quit_failure}};
}

ModalUnwinder::ModalUnwinder(Hooks hooks, int max_turns) : m_hooks(std::move(hooks)), m_max_turns(max_turns) {}

ModalUnwinder::Hold ModalUnwinder::hold_back(std::function<void()> close)
{
    const bool holding = m_turns > 0;
    if (m_hooks.session_ending())
        return finish(Hold::go_on);
    const ModalState modal = m_hooks.modal_state();
    if (!modal.anything_open())
        return finish(holding ? Hold::released : Hold::go_on);
    if (m_turns >= m_max_turns)
        return finish(Hold::given_up);
    ++m_turns;
    end_innermost(modal);
    m_hooks.run_after_a_turn(std::move(close));
    return holding ? Hold::held : Hold::first_held;
}

// The next close starts afresh, and no turn of this one is left to run.
ModalUnwinder::Hold ModalUnwinder::finish(Hold hold)
{
    if (m_turns > 0)
        m_hooks.cancel_turn();
    m_turns = 0;
    m_ended.clear();
    return hold;
}

// Only the innermost, and only an app dialog: ending one under another leaves an invisible modal (wx
// hides it, but its loop does not return), and a system panel only the user can close. A dialog ended
// once is left to return from its modal loop.
void ModalUnwinder::end_innermost(const ModalState& modal)
{
    if (modal.dialogs.empty() || modal.dialogs.front().system)
        return;
    const std::uint64_t innermost = modal.dialogs.front().id;
    if (std::find(m_ended.begin(), m_ended.end(), innermost) != m_ended.end())
        return;
    m_ended.push_back(innermost);
    m_hooks.end_dialog(innermost);
}

}}} // namespace Slic3r::GUI::OrcaMCP
