#include "FlashforgeConsoleHandler.hpp"

#include "PrinterWebView.hpp"
#include "PrinterWebViewHandler.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/Flashforge.hpp"
#include "slic3r/Utils/FlashforgeApi.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <boost/log/trivial.hpp>

using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

namespace {

// Poll cadences. A filament load has to feel live, a running print wants a steady beat, and an idle
// machine deserves neither the network traffic nor the wake-ups.
constexpr std::chrono::milliseconds POLL_LOADING{1000};
constexpr std::chrono::milliseconds POLL_PRINTING{2000};
constexpr std::chrono::milliseconds POLL_IDLE{5000};

// matlStationInfo.stateAction: 0 free, 1 supply wire (loading), 2 withdraw wire (unloading).
constexpr int ACTION_LOADING = 1, ACTION_UNLOADING = 2;

/// Vendor identifiers that are not the page's business. The check code and serial never leave the
/// preset, and the raw `detail` object is passed through wholesale, so it gets filtered here.
const char* const REDACTED_RAW_KEYS[] = {"flashRegisterCode", "polarRegisterCode", "macAddr"};

int json_int(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_number_integer()) ? it->get<int>() : 0;
}

std::chrono::milliseconds cadence_for(const FlashforgeApi::PrinterStatus& status)
{
    if (status.raw.is_object()) {
        const auto station = status.raw.find("matlStationInfo");
        if (station != status.raw.end() && station->is_object()) {
            const int action = json_int(*station, "stateAction");
            if (action == ACTION_LOADING || action == ACTION_UNLOADING)
                return POLL_LOADING;
        }
    }
    return (status.state == "printing" || status.state == "heating" || status.state == "busy") ? POLL_PRINTING
                                                                                              : POLL_IDLE;
}

json printer_json(const FlashforgeApi::PrinterStatus& status)
{
    json printer = OrcaMCP::status_to_json(status);
    if (printer["raw"].is_object())
        for (const char* key : REDACTED_RAW_KEYS)
            printer["raw"].erase(key);
    return printer;
}

/// The console page for one Flashforge printer: answers its `status` request, then keeps pushing.
///
/// Threading: script messages and delivery run on the GUI thread; every local-API call runs on the
/// poll thread, because a blocking HTTP request on the GUI thread freezes the whole slicer. The
/// thread is started by the page's first request and joined in the destructor, and an `alive` token
/// keeps a CallAfter that outlives the handler from touching it.
class FlashforgeConsoleHandler final : public PrinterWebViewHandler
{
public:
    explicit FlashforgeConsoleHandler(PrinterWebView& owner) : PrinterWebViewHandler(owner) {}

    ~FlashforgeConsoleHandler() override
    {
        *m_alive = false;
        stop_polling();
    }

    void on_script_message(wxWebViewEvent& evt) override
    {
        const wxString message = evt.GetString();
        if (message.empty())
            return;

        const json request = json::parse(message.ToUTF8().data(), nullptr, false);
        if (request.is_discarded() || !request.is_object())
            return;

        const int         id     = json_int(request, "id");
        const std::string method = request.value("method", std::string());

        if (method == "status") {
            start_polling();
            answer(id, last_status());
            return;
        }

        answer_error(id, method, "Unknown method '" + method + "'");
    }

private:
    // ── Delivery (GUI thread) ────────────────────────────────────────────────────────────────

    /// Stamps the fields only the GUI thread can read, then evaluates the reply into the page.
    void answer(int id, json result)
    {
        result["app_dark"] = wxGetApp().dark_mode();
        run_in_page(json{{"id", id}, {"method", "status"}, {"ok", true}, {"result", std::move(result)}});
    }

    void answer_error(int id, const std::string& method, const std::string& error)
    {
        run_in_page(json{{"id", id}, {"method", method}, {"ok", false}, {"error", error}});
    }

    void run_in_page(const json& message)
    {
        if (browser() == nullptr)
            return;
        // ensure_ascii, so the reply survives the trip through wxString regardless of what the
        // printer put in a file name.
        const wxString payload = wxString::FromUTF8(message.dump(-1, ' ', true, json::error_handler_t::replace));
        WebView::RunScript(browser(), "if (window.orcaFlashforge) window.orcaFlashforge.receive(" + payload + ");");
    }

    // ── Polling ──────────────────────────────────────────────────────────────────────────────

    /// GUI thread. Idempotent: a page reload asks again, and must not start a second poller.
    void start_polling()
    {
        if (m_poll_thread.joinable())
            return;

        DynamicPrintConfig config;
        std::string        host_type, error;
        if (!OrcaMCP::resolve_print_host_config(config, host_type, error)) {
            set_last_status(offline_status(error));
            return;
        }

        std::string preset;
        if (PresetBundle* bundle = wxGetApp().preset_bundle; bundle != nullptr)
            preset = bundle->printers.get_edited_preset().name;

        m_identity = json{{"preset", preset}, {"host", config.opt_string("print_host")}};

        {
            std::lock_guard<std::mutex> lock(m_poll_mutex);
            m_poll_stop = false;
        }
        m_poll_thread = std::thread([this, config = std::move(config)]() mutable { run_poll_loop(std::move(config)); });
    }

    void stop_polling()
    {
        {
            std::lock_guard<std::mutex> lock(m_poll_mutex);
            m_poll_stop = true;
        }
        m_poll_cv.notify_all();
        if (m_poll_thread.joinable())
            m_poll_thread.join();
    }

    /// Sleeps up to `interval`, waking early when polling is stopped. False means "stop now".
    bool wait_for_next_poll(std::chrono::milliseconds interval)
    {
        std::unique_lock<std::mutex> lock(m_poll_mutex);
        m_poll_cv.wait_for(lock, interval, [this]() { return m_poll_stop; });
        return !m_poll_stop;
    }

    /// Poll thread. `config` is this thread's own copy; Flashforge copies the strings it needs out
    /// of it at construction, so nothing here reaches back into the preset.
    void run_poll_loop(DynamicPrintConfig config)
    {
        const Flashforge host(&config);
        if (!host.has_local_api_credentials()) {
            push(offline_status(
                into_u8(_L("This printer preset has no serial number or access code, so its status cannot be read."))));
            return;
        }

        while (true) {
            {
                std::lock_guard<std::mutex> lock(m_poll_mutex);
                if (m_poll_stop)
                    break;
            }

            FlashforgeApi::PrinterStatus status;
            wxString                     message;
            std::chrono::milliseconds    interval = POLL_IDLE;

            if (host.fetch_status(status, message)) {
                json snapshot   = m_identity;
                snapshot["connected"] = true;
                snapshot["printer"]   = printer_json(status);
                interval              = cadence_for(status);
                snapshot["poll_ms"]   = static_cast<int>(interval.count());
                push(std::move(snapshot));
            } else {
                push(offline_status(message.ToUTF8().data()));
            }

            if (!wait_for_next_poll(interval))
                break;
        }
    }

    /// Any thread. The snapshot shape for "the printer did not answer": the page renders the reason
    /// in its own banner, never as a dialog.
    json offline_status(const std::string& error) const
    {
        json snapshot         = m_identity;
        snapshot["connected"] = false;
        snapshot["error"]     = error;
        snapshot["poll_ms"]   = static_cast<int>(POLL_IDLE.count());
        return snapshot;
    }

    /// Any thread. Caches the snapshot (so a later request answers instantly) and pushes it.
    void push(json snapshot)
    {
        set_last_status(snapshot);

        auto alive = m_alive;
        wxGetApp().CallAfter([this, alive, snapshot = std::move(snapshot)]() mutable {
            if (*alive)
                answer(0, std::move(snapshot)); // id 0 = unsolicited push
        });
    }

    void set_last_status(json snapshot)
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        m_last_status = std::move(snapshot);
    }

    /// Any thread. Before the first poll comes back there is nothing to report yet - which is
    /// "connecting", not "unreachable", and the page renders it without the error banner.
    json last_status() const
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        if (!m_last_status.is_null())
            return m_last_status;

        json snapshot         = offline_status(std::string());
        snapshot["connecting"] = true;
        return snapshot;
    }

    // Set on the GUI thread before the poll thread starts, read-only afterwards.
    json m_identity = json::object();

    mutable std::mutex m_status_mutex;
    json               m_last_status;

    std::thread             m_poll_thread;
    std::mutex              m_poll_mutex;
    std::condition_variable m_poll_cv;
    bool                    m_poll_stop = true;

    // Cleared before the poll thread is joined, so a CallAfter queued by the last poll cannot touch
    // a destroyed handler.
    std::shared_ptr<std::atomic_bool> m_alive = std::make_shared<std::atomic_bool>(true);
};

} // namespace

wxString flashforge_console_url()
{
    return wxString::Format("file://%s/web/flashforge/index.html", from_u8(resources_dir()));
}

std::unique_ptr<PrinterWebViewHandler> make_flashforge_console_handler(PrinterWebView& owner)
{
    return std::make_unique<FlashforgeConsoleHandler>(owner);
}

} // namespace GUI
} // namespace Slic3r
