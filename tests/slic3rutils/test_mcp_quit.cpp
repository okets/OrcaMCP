#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPQuit.hpp"

// Quitting while a modal dialog is open. On 2026-09-26 quit_app, sent while the startup "restore
// unsaved items?" prompt was open, aborted the app: the main frame was deleted inside the prompt's
// modal loop and deleted the prompt, which lived on its caller's stack. These drive the rule that
// holds such a close back -- end the app's own dialogs innermost first, close only once nothing
// modal is left -- with a fake app: its dialogs, its modal loop and its event-loop turns.

using namespace Slic3r::GUI::OrcaMCP;
using Hold = ModalUnwinder::Hold;

namespace {

// The app as the unwinder sees it. A dialog it is told to end leaves the stack when the next turn
// runs, as a modal loop returns only once the event loop has had a turn.
struct FakeApp
{
    std::vector<ModalState::Dialog>   dialogs; // innermost first
    bool                              system_dialog = false;
    std::vector<std::uintptr_t>       ended;
    std::vector<std::function<void()>> turns;
    std::vector<std::uintptr_t>       unwinding;
    bool                              refuses_to_end = false; // a dialog that stays however it is ended

    ModalState state() const
    {
        ModalState s;
        s.dialogs       = dialogs;
        s.in_modal_loop = !dialogs.empty() || system_dialog;
        return s;
    }

    ModalUnwinder::Hooks hooks()
    {
        return {[this] { return state(); },
                [this](std::uintptr_t id) {
                    ended.push_back(id);
                    if (!refuses_to_end)
                        unwinding.push_back(id);
                },
                [this](std::function<void()> task) { turns.push_back(std::move(task)); }};
    }

    // One turn of the event loop: ended dialogs return from their modal loops, then the task runs.
    bool run_turn()
    {
        if (turns.empty())
            return false;
        for (std::uintptr_t id : unwinding)
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
    ModalUnwinder& unwinder;
    int            went_on = 0;
    Hold           last    = Hold::go_on;

    void operator()()
    {
        last = unwinder.hold_back([this] { (*this)(); });
        if (last == Hold::go_on)
            ++went_on;
    }
};

const ModalState::Dialog restore_prompt{1, "OrcaMCP - Restore"};
const ModalState::Dialog preferences{2, "Preferences"};

} // namespace

TEST_CASE("a close with nothing modal open goes on at once", "[McpQuit][orcamcp]")
{
    FakeApp       app;
    ModalUnwinder unwinder(app.hooks());
    Close         close{unwinder};

    close();

    CHECK(close.went_on == 1);
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
    CHECK(app.ended == std::vector<std::uintptr_t>{restore_prompt.id});

    REQUIRE(app.run_turn());
    CHECK(close.went_on == 1);
    CHECK(app.turns.empty());
}

TEST_CASE("stacked dialogs are ended innermost first, one per turn", "[McpQuit][orcamcp]")
{
    // Upstream's DPIAware::EndModal refuses a dialog that is not the innermost one.
    FakeApp app;
    app.dialogs = {preferences, restore_prompt};
    ModalUnwinder unwinder(app.hooks());
    Close         close{unwinder};

    close();
    CHECK(app.ended == std::vector<std::uintptr_t>{preferences.id});
    REQUIRE(app.run_turn());
    CHECK(app.ended == std::vector<std::uintptr_t>{preferences.id, restore_prompt.id});
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

    CHECK(app.ended == std::vector<std::uintptr_t>{restore_prompt.id});
    CHECK(close.went_on == 0);
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

TEST_CASE("a system dialog is never ended by the app, only waited for", "[McpQuit][orcamcp]")
{
    FakeApp app;
    app.system_dialog = true;
    ModalUnwinder unwinder(app.hooks(), 4);
    Close         close{unwinder};

    close();
    CHECK(close.last == Hold::held);
    app.system_dialog = false; // the user closed it
    REQUIRE(app.run_turn());

    CHECK(app.ended.empty());
    CHECK(close.went_on == 1);
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

    app.dialogs.clear();
    close();
    CHECK(close.went_on == 1);

    app.dialogs        = {preferences};
    app.refuses_to_end = false;
    close();
    CHECK(close.last == Hold::held);
    CHECK(app.ended.back() == preferences.id);
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
    modal.in_modal_loop = true;

    for (bool discard : {true, false}) {
        const auto refusal = quit_refusal(modal, discard, false);
        REQUIRE(refusal);
        CHECK(refusal->find("system dialog") != std::string::npos);
    }
}
