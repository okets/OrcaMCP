#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/OrcaMCP/OrcaMCPPrintDialog.hpp"

using namespace Slic3r::GUI::OrcaMCP;

TEST_CASE("Print plate mode requires an explicit dialog call on the current plate", "[McpPrintDialog]")
{
    CHECK_FALSE(print_dialog_argument_refusal(nlohmann::json::object()));
    CHECK_FALSE(print_dialog_argument_refusal({{"dialog_mode", "send"}}));
    CHECK_FALSE(print_dialog_argument_refusal({{"dialog_mode", "print_plate"}, {"direct", false}}));
    CHECK(print_dialog_argument_refusal({{"dialog_mode", "print_plate"}}));
    CHECK(print_dialog_argument_refusal({{"dialog_mode", "print_plate"}, {"direct", true}}));
    CHECK(print_dialog_argument_refusal({{"dialog_mode", "print_plate"}, {"direct", false}, {"all_plates", true}}));
    CHECK(print_dialog_argument_refusal({{"dialog_mode", "unknown"}}));
    CHECK(print_dialog_argument_refusal({{"dialog_mode", 1}}));
    CHECK(print_dialog_argument_refusal({{"dialog_mode", "print_plate"}, {"direct", "false"}}));
    CHECK(print_dialog_argument_refusal({{"dialog_mode", "print_plate"}, {"direct", false}, {"all_plates", 1}}));
}

TEST_CASE("Each print plate readiness failure prevents native dispatch", "[McpPrintDialog]")
{
    const PrintDialogState ready{true, false, true, false, true, true, true};
    CHECK_FALSE(print_dialog_refusal(ready));
    for (int field = 0; field < 7; ++field) {
        auto state = ready;
        switch (field) {
        case 0: state.plater_available = false; break;
        case 1: state.slicing = true; break;
        case 2: state.worker_idle = false; break;
        case 3: state.modal_open = true; break;
        case 4: state.same_plate = false; break;
        case 5: state.valid_slice = false; break;
        case 6: state.machine_select = false; break;
        }
        int dispatched = 0;
        CHECK_FALSE(dispatch_print_dialog_if_ready([&] { return print_dialog_refusal(state); }, [&] { ++dispatched; }));
        CHECK(dispatched == 0);
    }
}

TEST_CASE("Deferred print plate dispatch reads readiness again before opening", "[McpPrintDialog]")
{
    bool blocked = false;
    int dispatched = 0;
    auto check = [&]() -> std::optional<std::string> {
        return blocked ? std::optional<std::string>("G-code check failed") : std::nullopt;
    };
    REQUIRE_FALSE(check());
    auto deferred = [&] { return dispatch_print_dialog_if_ready(check, [&] { ++dispatched; }); };
    blocked = true;
    CHECK_FALSE(deferred());
    CHECK(dispatched == 0);
    blocked = false;
    CHECK(deferred());
    CHECK(dispatched == 1);
}
