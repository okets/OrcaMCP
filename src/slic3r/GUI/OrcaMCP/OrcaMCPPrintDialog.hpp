#pragma once
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

inline std::optional<std::string> print_dialog_argument_refusal(const nlohmann::json& params)
{
    if (params.contains("dialog_mode") && (!params["dialog_mode"].is_string() ||
        (params["dialog_mode"] != "send" && params["dialog_mode"] != "print_plate")))
        return "dialog_mode must be send or print_plate";
    if (params.value("dialog_mode", std::string("send")) == "print_plate") {
        if ((params.contains("direct") && !params["direct"].is_boolean()) ||
            (params.contains("all_plates") && !params["all_plates"].is_boolean()))
            return "direct and all_plates must be booleans";
        if (params.value("direct", true))
            return "dialog_mode=print_plate requires direct=false";
        if (params.value("all_plates", false))
            return "dialog_mode=print_plate supports the current plate only";
    }
    return std::nullopt;
}

struct PrintDialogState
{
    bool plater_available;
    bool slicing;
    bool worker_idle;
    bool modal_open;
    bool same_plate;
    bool valid_slice;
    bool machine_select;
};

inline std::optional<std::string> print_dialog_refusal(const PrintDialogState& state)
{
    if (!state.plater_available) return "Plater not available or app is closing";
    if (state.slicing || !state.worker_idle) return "Wait for slicing and UI jobs to finish before opening Print plate";
    if (state.modal_open) return "Close the current modal dialog before opening Print plate";
    if (!state.same_plate) return "The current plate changed before Print plate opened";
    if (!state.valid_slice) return "Slice the current plate before opening Print plate";
    if (!state.machine_select) return "The current printer does not use the native machine-select Print plate dialog";
    return std::nullopt;
}

// The deferred GUI callback checks live readiness again, then opens only the native dialog.
template<class Check, class Open>
bool dispatch_print_dialog_if_ready(Check check, Open open)
{
    if (check()) return false;
    open();
    return true;
}

}}} // namespace Slic3r::GUI::OrcaMCP
