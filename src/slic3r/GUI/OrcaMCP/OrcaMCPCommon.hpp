// src/slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp
#pragma once
#include <future>
#include <functional>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r { namespace GUI {
class Plater;
namespace OrcaMCP {

// Runs `func` on the wx main thread and blocks the calling HTTP worker until it returns.
// `func` must return nlohmann::json. Exceptions propagate to the caller.
template<typename Func>
nlohmann::json run_on_main_thread(Func&& func)
{
    std::promise<nlohmann::json> promise;
    auto future = promise.get_future();
    wxGetApp().CallAfter([&promise, func = std::forward<Func>(func)]() {
        try {
            promise.set_value(func());
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    return future.get();
}

// Always returns {"count": N, "warnings": [{level, message, type}...]}.
nlohmann::json get_active_warnings_json(Plater* plater);

void add_turntable_preview_if_requested(nlohmann::json& result, bool include_preview, int view_count = 4, int resolution = 256);

// RAII: suppress modal dialogs for the lifetime of the guard and collect their messages.
struct McpDialogSuppressionGuard
{
    McpDialogSuppressionGuard()  { clear_mcp_suppressed_messages(); set_mcp_dialog_suppression(true); }
    ~McpDialogSuppressionGuard() { set_mcp_dialog_suppression(false); }
    std::vector<std::string> messages() const { return get_mcp_suppressed_messages(); }
};

}}} // namespace Slic3r::GUI::OrcaMCP
