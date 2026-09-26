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
    "a system dialog (a file chooser or an alert), which the app cannot close itself";

} // namespace

std::vector<std::string> ModalState::titles() const
{
    std::vector<std::string> out;
    out.reserve(dialogs.size());
    for (const Dialog& dialog : dialogs)
        out.push_back(dialog.title);
    return out;
}

std::optional<std::string> quit_refusal(const ModalState& modal, bool discard_changes, bool project_dirty)
{
    if (!discard_changes && project_dirty)
        return std::string("project has unsaved changes; call save_project first or pass discard_changes=true");
    if (!discard_changes && !modal.dialogs.empty())
        return "a dialog is open in the app: " + quoted_list(modal.titles()) +
               "; answer it there, or pass discard_changes=true to close it unanswered";
    if (modal.system_dialog_open())
        return std::string("the app is showing ") + k_system_dialog + "; close it in the app, then call quit_app again";
    return std::nullopt;
}

std::vector<std::string> quit_notes(const ModalState& modal)
{
    std::vector<std::string> notes;
    for (const std::string& title : modal.titles())
        notes.push_back("The dialog '" + title + "' was open; it is closed unanswered, as when the system logs out.");
    return notes;
}

ModalUnwinder::ModalUnwinder(Hooks hooks, int max_turns) : m_hooks(std::move(hooks)), m_max_turns(max_turns) {}

ModalUnwinder::Hold ModalUnwinder::hold_back(std::function<void()> close)
{
    const ModalState modal = m_hooks.modal_state();
    if (!modal.anything_open() || m_turns >= m_max_turns) {
        const Hold hold = modal.anything_open() ? Hold::given_up : Hold::go_on;
        m_turns = 0; // the next close starts afresh
        m_ended.clear();
        return hold;
    }
    ++m_turns;
    end_innermost(modal);
    m_hooks.run_after_a_turn(std::move(close));
    return Hold::held;
}

// One dialog per turn: upstream's DPIAware::EndModal refuses a dialog that is not the innermost one,
// and a dialog that was ended once is left to return from its modal loop.
void ModalUnwinder::end_innermost(const ModalState& modal)
{
    if (modal.dialogs.empty())
        return; // a system dialog: only the user can close it
    const std::uintptr_t innermost = modal.dialogs.front().id;
    if (std::find(m_ended.begin(), m_ended.end(), innermost) != m_ended.end())
        return;
    m_ended.push_back(innermost);
    m_hooks.end_dialog(innermost);
}

}}} // namespace Slic3r::GUI::OrcaMCP
