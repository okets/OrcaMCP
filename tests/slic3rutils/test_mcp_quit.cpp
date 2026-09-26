#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPQuit.hpp"

// Quitting while a modal dialog is open. On 2026-09-26 quit_app, sent while the startup "restore
// unsaved items?" prompt was open, aborted the app: the main frame was deleted inside the prompt's
// modal loop and deleted the prompt, which lived on its caller's stack. These drive the rule that
// holds such a close back -- end the innermost dialog unanswered, one per turn, close only once
// nothing modal is left -- with a fake app: its dialogs, its modal loop and its event-loop turns.

using namespace Slic3r::GUI::OrcaMCP;
using Hold = ModalUnwinder::Hold;

namespace {

ModalState::Dialog app_dialog(std::uint64_t id, std::string title) { return {id, std::move(title), false}; }
ModalState::Dialog system_dialog(std::uint64_t id, std::string title) { return {id, std::move(title), true}; }

// The app as the unwinder sees it. A dialog it is told to end leaves the stack when the next turn
// runs, as a modal loop returns only once the event loop has had a turn.
struct FakeApp
{
    std::vector<ModalState::Dialog>    dialogs; // innermost first
    bool                               untracked_loop = false;
    bool                               session_ending = false;
    std::vector<std::uint64_t>         ended;
    std::vector<std::function<void()>> turns;
    std::vector<std::uint64_t>         unwinding;
    bool                               refuses_to_end = false; // a dialog that stays however it is ended
    int                                cancelled_turns = 0;

    ModalState state() const
    {
        ModalState s;
        s.dialogs       = dialogs;
        s.in_modal_loop = !dialogs.empty() || untracked_loop;
        return s;
    }

    ModalUnwinder::Hooks hooks()
    {
        return {[this] { return state(); },
                [this](std::uint64_t id) {
                    ended.push_back(id);
                    if (!refuses_to_end)
                        unwinding.push_back(id);
                },
                [this](std::function<void()> task) { turns.push_back(std::move(task)); },
                [this] {
                    ++cancelled_turns;
                    turns.clear();
                },
                [this] { return session_ending; }};
    }

    // One turn of the event loop: ended dialogs return from their modal loops, then the task runs.
    bool run_turn()
    {
        if (turns.empty())
            return false;
        for (std::uint64_t id : unwinding)
            dialogs.erase(std::remove_if(dialogs.begin(), dialogs.end(), [id](const auto& d) { return d.id == id; }),
                          dialogs.end());
        unwinding.clear();
        auto task = std::move(turns.front());
        turns.erase(turns.begin());
        task();
        return true;
    }
};

// A close that asks the unwinder, as the main frame's close handler does, and counts how often it
// went on.
struct Close
{
    ModalUnwinder&                   unwinder;
    int                              went_on = 0;
    Hold                             last    = Hold::go_on;
    std::vector<std::pair<Hold, int>> seen; // each outcome, with the turns its hold had taken

    void operator()()
    {
        last = unwinder.hold_back([this] { (*this)(); });
        seen.emplace_back(last, unwinder.turns());
        if (last == Hold::go_on)
            ++went_on;
    }
};

const ModalState::Dialog restore_prompt = app_dialog(1, "OrcaMCP - Restore");
const ModalState::Dialog preferences    = app_dialog(2, "Preferences");

} // namespace

TEST_CASE("a close with nothing modal open goes on at once", "[McpQuit][orcamcp]")
{
    FakeApp       app;
    ModalUnwinder unwinder(app.hooks());
    Close         close{unwinder};

    close();

    CHECK(close.last == Hold::go_on);
    CHECK(app.ended.empty());
    CHECK(app.turns.empty());
}

TEST_CASE("a close while a dialog is open ends it, and goes on only once it has unwound", "[McpQuit][orcamcp]")
{
    FakeApp app;
    app.dialogs = {restore_prompt};
    ModalUnwinder unwinder(app.hooks());
    Close         close{unwinder};

    close();
    CHECK(close.last == Hold::held);
    CHECK(close.went_on == 0); // not inside the dialog's loop: that is the crash
    CHECK(app.ended == std::vector<std::uint64_t>{restore_prompt.id});

    REQUIRE(app.run_turn());
    CHECK(close.last == Hold::go_on);
    CHECK(close.went_on == 1);
    CHECK(app.turns.empty());
}

TEST_CASE("a hold's turn count tells its first turn and its end from the turns between", "[McpQuit][orcamcp]")
{
    // The close handler logs the first turn and the outcome only, not one line per 50 ms turn: held on
    // turn 1 is the start, go_on after turns is the end, and go_on after none is a close never held.
    FakeApp app;
    app.dialogs        = {restore_prompt};
    app.refuses_to_end = true;
    ModalUnwinder unwinder(app.hooks());
    Close         close{unwinder};

    close();
    REQUIRE(app.run_turn());
    REQUIRE(app.run_turn());
    app.dialogs.clear(); // the user answered it
    REQUIRE(app.run_turn());

    using Seen = std::vector<std::pair<Hold, int>>;
    CHECK(close.seen == Seen{{Hold::held, 1}, {Hold::held, 2}, {Hold::held, 3}, {Hold::go_on, 3}});

    close(); // the next close, with nothing open
    CHECK(close.seen.back() == std::pair<Hold, int>{Hold::go_on, 0});
}

TEST_CASE("stacked dialogs are ended innermost first, one per turn", "[McpQuit][orcamcp]")
{
    // Ending any other leaves an invisible modal: wx hides it but its loop does not return.
    FakeApp app;
    app.dialogs = {preferences, restore_prompt};
    ModalUnwinder unwinder(app.hooks());
    Close         close{unwinder};

    close();
    CHECK(app.ended == std::vector<std::uint64_t>{preferences.id});
    REQUIRE(app.run_turn());
    CHECK(app.ended == std::vector<std::uint64_t>{preferences.id, restore_prompt.id});
    CHECK(close.went_on == 0);
    REQUIRE(app.run_turn());
    CHECK(close.went_on == 1);
}

TEST_CASE("a dialog is ended once, however many turns it takes to unwind", "[McpQuit][orcamcp]")
{
    FakeApp app;
    app.dialogs        = {restore_prompt};
    app.refuses_to_end = true; // still on the stack turn after turn
    ModalUnwinder unwinder(app.hooks(), 5);
    Close         close{unwinder};

    close();
    for (int i = 0; i < 3; ++i)
        REQUIRE(app.run_turn());

    CHECK(app.ended == std::vector<std::uint64_t>{restore_prompt.id});
    CHECK(close.went_on == 0);
}

TEST_CASE("a dialog shown again after one was ended is ended too", "[McpQuit][orcamcp]")
{
    // A dialog on the stack, as dialogs are, opens at the address of the one before it. It is a new
    // showing with an id of its own, and must not be skipped as "ended already".
    FakeApp app;
    app.dialogs = {restore_prompt};
    ModalUnwinder unwinder(app.hooks());
    Close         close{unwinder};

    close();
    app.unwinding.clear();                                   // the first one returns...
    app.dialogs = {app_dialog(7, restore_prompt.title)};     // ...and its caller shows another at once
    REQUIRE(app.run_turn());

    CHECK(app.ended == std::vector<std::uint64_t>{restore_prompt.id, 7});
}

TEST_CASE("a system dialog on top is never ended, and nothing under it is either", "[McpQuit][orcamcp]")
{
    FakeApp app;
    app.dialogs = {system_dialog(3, "Open"), preferences};
    ModalUnwinder unwinder(app.hooks(), 4);
    Close         close{unwinder};

    close();
    CHECK(close.last == Hold::held);
    REQUIRE(app.run_turn());
    CHECK(app.ended.empty()); // Preferences is not the innermost: ending it would leave an invisible modal

    app.dialogs = {preferences}; // the user closed the file chooser
    REQUIRE(app.run_turn());
    CHECK(app.ended == std::vector<std::uint64_t>{preferences.id});
    REQUIRE(app.run_turn());
    CHECK(close.went_on == 1);
}

TEST_CASE("a modal loop that never unwinds is given up on, and the app is not closed under it", "[McpQuit][orcamcp]")
{
    FakeApp app;
    app.dialogs        = {restore_prompt};
    app.refuses_to_end = true;
    ModalUnwinder unwinder(app.hooks(), 3);
    Close         close{unwinder};

    close();
    int turns = 0;
    while (app.run_turn())
        ++turns;

    CHECK(turns == 3);
    CHECK(close.last == Hold::given_up);
    CHECK(close.went_on == 0);
}

TEST_CASE("an unwinder that gave up starts afresh on the next close", "[McpQuit][orcamcp]")
{
    FakeApp app;
    app.dialogs        = {restore_prompt};
    app.refuses_to_end = true;
    ModalUnwinder unwinder(app.hooks(), 1);
    Close         close{unwinder};

    close();
    while (app.run_turn()) {}
    REQUIRE(close.last == Hold::given_up);

    close(); // a new quit_app while the same dialog is still open
    CHECK(close.last == Hold::held);

    app.dialogs.clear();
    close();
    CHECK(close.went_on == 1);
}

TEST_CASE("a close at the end of the system session is never held", "[McpQuit][orcamcp]")
{
    // The process ends as soon as the end-session event returns, so a close held for a turn never
    // runs, and the teardown that saves the app config with it. The logout handler has already ended
    // the innermost dialog.
    FakeApp app;
    app.dialogs = {restore_prompt};
    ModalUnwinder unwinder(app.hooks());
    Close         close{unwinder};

    close();
    REQUIRE(close.last == Hold::held);
    app.session_ending = true;
    close();

    CHECK(close.last == Hold::go_on);
    CHECK(close.went_on == 1);
    CHECK(app.turns.empty()); // the turn that was pending is dropped
    CHECK(app.cancelled_turns >= 1);
}

TEST_CASE("a quit that gave up is reported until the next quit_app", "[McpQuit][orcamcp]")
{
    // quit_app has already answered "quitting"; the agent learns otherwise from active_warnings.
    clear_quit_failure();
    CHECK_FALSE(quit_failed_warning());

    ModalState still_open;
    still_open.dialogs       = {preferences};
    still_open.in_modal_loop = true;
    note_quit_failed(still_open);

    const auto warning = quit_failed_warning();
    REQUIRE(warning);
    CHECK((*warning)["type"] == "QuitFailed");
    CHECK((*warning)["message"].get<std::string>().find("'Preferences'") != std::string::npos);
    CHECK((*warning)["message"].get<std::string>().find("still open") != std::string::npos);

    clear_quit_failure();
    CHECK_FALSE(quit_failed_warning());
}

TEST_CASE("the modal stack gives every showing of a dialog an id of its own", "[McpQuit][orcamcp]")
{
    ModalStack stack;
    stack.entered(0x1000, "OrcaMCP - Restore", false);
    const auto first = stack.innermost_first();
    REQUIRE(first.size() == 1);
    stack.exited(0x1000);
    CHECK(stack.innermost_first().empty());

    stack.entered(0x1000, "OrcaMCP - Restore", false); // the same address, a new dialog
    const auto second = stack.innermost_first();
    REQUIRE(second.size() == 1);
    CHECK(second.front().id != first.front().id);
    CHECK_FALSE(stack.innermost_key(first.front().id));
    CHECK(stack.innermost_key(second.front().id) == std::uintptr_t{0x1000});
}

TEST_CASE("the modal stack lists plain, system and stacked dialogs innermost first", "[McpQuit][orcamcp]")
{
    // A plain wxDialog subclass (the flushing-volumes dialog, FilamentMapDialog) never enters
    // dialogStack; the hook sees it all the same, and it is the app's to end.
    ModalStack stack;
    stack.entered(0x1, "Preferences", false);
    stack.entered(0x2, "Flushing volumes for filament change", false);
    stack.entered(0x3, "Choose a file", true);

    const auto open = stack.innermost_first();
    REQUIRE(open.size() == 3);
    CHECK(open[0].title == "Choose a file");
    CHECK(open[0].system);
    CHECK(open[1].title == "Flushing volumes for filament change");
    CHECK_FALSE(open[1].system);
    CHECK(open[2].title == "Preferences");

    CHECK_FALSE(stack.innermost_key(open[1].id)); // only the innermost may be ended
    stack.exited(0x3);
    CHECK(stack.innermost_key(open[1].id) == std::uintptr_t{0x2});
}

TEST_CASE("a dialog is closed unanswered with its own no", "[McpQuit][orcamcp]")
{
    // Yes / No / Cancel: Cancel abandons what asked.
    CHECK(decline_answer(/*has_cancel_button=*/true, /*has_no_button=*/true, /*no_button_refuses=*/true) == Decline::cancel);
    CHECK(decline_answer(true, false, false) == Decline::cancel);
    // Yes / No: "Sync printer information?" goes on unless the answer is No.
    CHECK(decline_answer(false, true, true) == Decline::no);
    // A No relabelled as a choice ("Right: 0.6 mm") is not a refusal: Cancel, which its caller takes
    // as "neither", as it does its close box.
    CHECK(decline_answer(false, true, false) == Decline::cancel);
    CHECK(decline_answer(false, false, false) == Decline::cancel); // what its close box returns
}

TEST_CASE("quit_app refuses to close a dialog unanswered when told to keep unsaved work", "[McpQuit][orcamcp]")
{
    ModalState modal;
    modal.dialogs       = {restore_prompt};
    modal.in_modal_loop = true;

    const auto refusal = quit_refusal(modal, /*discard_changes=*/false, /*project_dirty=*/false);
    REQUIRE(refusal);
    CHECK(refusal->find("'OrcaMCP - Restore'") != std::string::npos);
    CHECK(refusal->find("discard_changes=true") != std::string::npos);

    CHECK_FALSE(quit_refusal(modal, /*discard_changes=*/true, /*project_dirty=*/false));
    const auto notes = quit_notes(modal);
    REQUIRE(notes.size() == 1);
    CHECK(notes.front().find("'OrcaMCP - Restore'") != std::string::npos);
}

TEST_CASE("quit_app refuses while the project is dirty and unsaved work is to be kept", "[McpQuit][orcamcp]")
{
    const ModalState nothing;
    const auto       refusal = quit_refusal(nothing, /*discard_changes=*/false, /*project_dirty=*/true);
    REQUIRE(refusal);
    CHECK(refusal->find("save_project") != std::string::npos);

    CHECK_FALSE(quit_refusal(nothing, false, false));
    CHECK_FALSE(quit_refusal(nothing, true, true));
    CHECK(quit_notes(nothing).empty());
}

TEST_CASE("quit_app refuses while a system dialog is open, whatever it is told", "[McpQuit][orcamcp]")
{
    ModalState modal;
    modal.dialogs       = {system_dialog(3, "Open"), preferences};
    modal.in_modal_loop = true;

    for (bool discard : {true, false}) {
        const auto refusal = quit_refusal(modal, discard, false);
        REQUIRE(refusal);
        CHECK(refusal->find("system dialog") != std::string::npos);
    }
}

TEST_CASE("an app dialog alone is not a system dialog", "[McpQuit][orcamcp]")
{
    // The flushing-volumes dialog is a plain wxDialog: quit_app closes it, it does not refuse.
    ModalState modal;
    modal.dialogs       = {app_dialog(4, "Flushing volumes for filament change")};
    modal.in_modal_loop = true;

    CHECK_FALSE(modal.system_dialog_open());
    CHECK_FALSE(quit_refusal(modal, /*discard_changes=*/true, false));
}

TEST_CASE("quit_app refuses while a modal loop runs that no dialog accounts for", "[McpQuit][orcamcp]")
{
    ModalState modal;
    modal.in_modal_loop = true;

    CHECK(modal.untracked_modal_loop());
    CHECK_FALSE(modal.system_dialog_open());
    const auto refusal = quit_refusal(modal, true, false);
    REQUIRE(refusal);
    CHECK(refusal->find("cannot identify") != std::string::npos);
}

TEST_CASE("new_project and load_project refuse while the restore prompt is open", "[McpQuit][orcamcp]")
{
    CHECK_FALSE(pending_restore_refusal());
    {
        const RestorePromptOpen open;
        CHECK(restore_prompt_open());
        const auto refusal = pending_restore_refusal();
        REQUIRE(refusal);
        CHECK(refusal->find("quit_app") != std::string::npos);
    }
    CHECK_FALSE(restore_prompt_open());
    CHECK_FALSE(pending_restore_refusal());
}

TEST_CASE("get_scene_info and active_warnings name what is open", "[McpQuit][orcamcp]")
{
    nlohmann::json   none;
    const ModalState nothing;
    add_open_dialogs(none, nothing);
    CHECK(none["open_dialogs"] == nlohmann::json::array());
    CHECK(none["system_dialog_open"] == false);
    CHECK(none["untracked_modal_loop"] == false);
    CHECK_FALSE(open_dialog_warning(nothing));

    ModalState modal;
    modal.dialogs       = {preferences, restore_prompt};
    modal.in_modal_loop = true;
    nlohmann::json some;
    add_open_dialogs(some, modal);
    CHECK(some["open_dialogs"] == nlohmann::json::array({"Preferences", "OrcaMCP - Restore"}));
    CHECK(some["system_dialog_open"] == false);

    const auto warning = open_dialog_warning(modal);
    REQUIRE(warning);
    CHECK((*warning)["type"] == "OpenDialog");
    CHECK((*warning)["level"] == "warning");
    CHECK((*warning)["message"].get<std::string>().find("'Preferences'") != std::string::npos);

    ModalState system;
    system.dialogs       = {system_dialog(3, "Open")};
    system.in_modal_loop = true;
    nlohmann::json sys;
    add_open_dialogs(sys, system);
    CHECK(sys["open_dialogs"] == nlohmann::json::array());
    CHECK(sys["system_dialog_open"] == true);
    const auto system_warning = open_dialog_warning(system);
    REQUIRE(system_warning);
    CHECK((*system_warning)["message"].get<std::string>().find("system dialog") != std::string::npos);
}

TEST_CASE("get_scene_info reports a modal loop no dialog accounts for", "[McpQuit][orcamcp]")
{
    // quit_app refuses for it, so the scene must not look as if nothing were open.
    ModalState modal;
    modal.in_modal_loop = true;
    nlohmann::json result;
    add_open_dialogs(result, modal);

    CHECK(result["open_dialogs"] == nlohmann::json::array());
    CHECK(result["untracked_modal_loop"] == true);
    const auto warning = open_dialog_warning(modal);
    REQUIRE(warning);
    CHECK((*warning)["message"].get<std::string>().find("cannot identify") != std::string::npos);
}

TEST_CASE("a quit request from the system closes the app normally when nothing is open", "[McpQuit][orcamcp]")
{
    const ModalState nothing;
    for (bool can_veto : {true, false}) {
        const auto response = respond_to_session_end(nothing, can_veto);
        CHECK(response.close_frame);
        CHECK_FALSE(response.refuse);
        CHECK_FALSE(response.save_config_now); // the teardown saves it
    }
}

TEST_CASE("a quit request from the system is refused while a dialog is open, the config saved first", "[McpQuit][orcamcp]")
{
    // The Dock's Quit, a quit Apple Event and a logout arrive inside the dialog's event loop; closing
    // the frame there is the teardown-under-a-dialog abort. macOS apps refuse instead, as wx does, and
    // the config is saved in case the system ends the process regardless.
    ModalState modal;
    modal.dialogs       = {restore_prompt};
    modal.in_modal_loop = true;
    const auto response = respond_to_session_end(modal, /*can_veto=*/true);
    CHECK(response.refuse);
    CHECK_FALSE(response.close_frame);
    CHECK(response.save_config_now);

    ModalState untracked;
    untracked.in_modal_loop = true;
    CHECK(respond_to_session_end(untracked, true).refuse);
}

TEST_CASE("a session end that cannot be refused, with a dialog open, only saves the config", "[McpQuit][orcamcp]")
{
    // A Windows critical shutdown: the process is about to end. The frame's teardown would run inside
    // the dialog's loop, so it is left out; what the app must not lose, the config, is saved.
    ModalState modal;
    modal.dialogs       = {restore_prompt};
    modal.in_modal_loop = true;
    const auto response = respond_to_session_end(modal, /*can_veto=*/false);
    CHECK(response.save_config_now);
    CHECK_FALSE(response.refuse);
    CHECK_FALSE(response.close_frame);
}
