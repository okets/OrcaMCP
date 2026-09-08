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

/// As tolerant as FlashforgeApi's own try_parse_json_int: this firmware is loose about types, and a
/// stateAction that arrives as "1" rather than 1 must not silently read as 0 - that would take the
/// load progress strip off the page altogether.
int json_int(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    if (it == obj.end())
        return 0;
    try {
        if (it->is_number_integer() || it->is_number_unsigned())
            return it->get<int>();
        if (it->is_number_float())
            return static_cast<int>(it->get<double>());
        if (it->is_boolean())
            return it->get<bool>() ? 1 : 0;
        if (it->is_string()) {
            const std::string text = it->get<std::string>();
            size_t            pos  = 0;
            const long        value = std::stol(text, &pos, 10);
            if (pos == text.size())
                return static_cast<int>(value);
        }
    } catch (...) {
    }
    return 0;
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

/// One run of the poll loop, and everything that outlives the thread it runs on.
///
/// The thread is detached rather than joined, because a fetch_status fifteen seconds into a dead
/// socket must never be something the GUI thread waits for - that is a frozen slicer every time a
/// printer is deselected or the app quits. So nothing the thread touches may belong to the handler:
/// it all lives here, jointly owned, and the thread simply stops mattering once `stop` is set.
struct PollSession
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    stop = false;

    /// False once the loop has left, so the handler knows a new session is needed.
    std::atomic_bool running{true};

    mutable std::mutex status_mutex;
    json               last_status;

    /// Written before the thread starts, read-only afterwards.
    json identity = json::object();

    void ask_to_stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        cv.notify_all();
    }

    bool stopped()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return stop;
    }

    /// Sleeps up to `interval`, waking early when the session is stopped. False means "stop now".
    bool wait(std::chrono::milliseconds interval)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, interval, [this]() { return stop; });
        return !stop;
    }

    void set_status(json snapshot)
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        last_status = std::move(snapshot);
    }

    json status() const
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        return last_status;
    }

    /// The snapshot shape for "no answer": the page renders the reason in its own banner, never as
    /// a dialog. `connecting` distinguishes "nothing has come back yet" from "the printer is gone".
    json offline(const std::string& error, bool connecting = false) const
    {
        json snapshot         = identity;
        snapshot["connected"] = false;
        snapshot["error"]     = error;
        snapshot["poll_ms"]   = static_cast<int>(POLL_IDLE.count());
        if (connecting)
            snapshot["connecting"] = true;
        return snapshot;
    }
};

/// The console page for one Flashforge printer: answers its `status` request, then keeps pushing.
///
/// Threading: script messages and delivery run on the GUI thread; every local-API call runs on a
/// poll thread, because a blocking HTTP request on the GUI thread freezes the whole slicer.
class FlashforgeConsoleHandler final : public PrinterWebViewHandler
{
public:
    explicit FlashforgeConsoleHandler(PrinterWebView& owner) : PrinterWebViewHandler(owner) {}

    ~FlashforgeConsoleHandler() override
    {
        *m_alive = false;
        suspend();
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
            // A suspended page is answered, but never gets to start anything: it is parked, and its
            // own watchdog asking again must not be able to revive a printer the user deselected.
            if (!m_suspended)
                start_polling();
            answer(id, last_status());
            return;
        }

        answer_error(id, method, "Unknown method '" + method + "'");
    }

    /// The page has left the tab bar. Polling a printer nobody has selected any more, for the life
    /// of the app, is not something a parked page gets to do - and since the page runs a watchdog
    /// that re-asks whenever the pushes go quiet, stopping the session is not enough on its own:
    /// the handler stays shut until on_shown(), and tells the page so it stops asking at all.
    void on_suspended() override
    {
        if (m_suspended)
            return;
        m_suspended = true;
        suspend();
        notify_page("suspended");
    }

    /// The page is on the tab bar again (or still is). Polling resumes at once rather than waiting
    /// for the watchdog to notice.
    void on_shown() override
    {
        if (!m_suspended)
            return;
        m_suspended = false;
        notify_page("resumed");
        start_polling();
    }

private:
    // ── Delivery (GUI thread) ────────────────────────────────────────────────────────────────

    /// Stamps the fields only the GUI thread can read, then evaluates the reply into the page.
    void answer(int id, json result)
    {
        result["app_dark"] = wxGetApp().dark_mode();
        run_in_page(json{{"id", id}, {"method", "status"}, {"ok", true}, {"result", std::move(result)}});
    }

    /// A one-way note to the page: no id, nothing to resolve, just a change of state.
    void notify_page(const char* method)
    {
        run_in_page(json{{"id", 0}, {"method", method}, {"ok", true}});
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

    // ── Session lifecycle (GUI thread) ───────────────────────────────────────────────────────

    /// Idempotent, and a retry: a page reload asks again, and so does the page's watchdog whenever
    /// the pushes stop. Every failure below therefore recovers on the next request rather than
    /// needing the app restarted.
    void start_polling()
    {
        if (m_suspended || (m_session && m_session->running.load()))
            return;

        DynamicPrintConfig config;
        std::string        host_type, error;
        if (!OrcaMCP::resolve_print_host_config(config, host_type, error)) {
            // No print host on this preset (yet). Keep no session, so the next request re-resolves.
            m_session.reset();
            m_no_session_status = json{{"connected", false}, {"error", error},
                                       {"poll_ms", static_cast<int>(POLL_IDLE.count())}};
            return;
        }

        std::string preset;
        if (PresetBundle* bundle = wxGetApp().preset_bundle; bundle != nullptr)
            preset = bundle->printers.get_edited_preset().name;

        auto session       = std::make_shared<PollSession>();
        session->identity  = json{{"preset", preset}, {"host", config.opt_string("print_host")}};
        session->last_status = session->offline(std::string(), /*connecting*/ true);
        m_session          = session;

        std::thread(&FlashforgeConsoleHandler::run_poll_loop, session, m_alive, this, std::move(config)).detach();
    }

    /// Ends the current session without waiting for its thread: see PollSession.
    void suspend()
    {
        if (!m_session)
            return;
        m_session->ask_to_stop();
        m_no_session_status = m_session->status();
        m_session.reset();
    }

    json last_status() const
    {
        if (m_session)
            return m_session->status();
        return m_no_session_status.is_null() ? json{{"connected", false}, {"connecting", true},
                                                    {"poll_ms", static_cast<int>(POLL_IDLE.count())}}
                                             : m_no_session_status;
    }

    // ── The poll loop (its own thread) ───────────────────────────────────────────────────────

    /// `config` is this thread's own copy; Flashforge copies the strings it needs out of it at
    /// construction, so nothing here reaches back into the preset. `self` is only ever dereferenced
    /// inside a CallAfter, on the GUI thread, and only while `alive` says the handler is still there.
    static void run_poll_loop(std::shared_ptr<PollSession>      session,
                              std::shared_ptr<std::atomic_bool> alive,
                              FlashforgeConsoleHandler*         self,
                              DynamicPrintConfig                config)
    {
        struct Finished
        {
            std::shared_ptr<PollSession> session;
            ~Finished() { session->running = false; }
        } finished{session};

        const Flashforge host(&config);
        if (!host.has_local_api_credentials()) {
            // Nothing this session can do. It reports why and ends; the page's watchdog asks again
            // in a few seconds, and by then the preset may have gained its credentials.
            push(session, alive, self,
                 session->offline(into_u8(
                     _L("This printer preset has no serial number or access code, so its status cannot be read."))));
            return;
        }

        while (!session->stopped()) {
            FlashforgeApi::PrinterStatus status;
            wxString                     message;
            std::chrono::milliseconds    interval = POLL_IDLE;

            if (host.fetch_status(status, message)) {
                json snapshot         = session->identity;
                snapshot["connected"] = true;
                snapshot["printer"]   = printer_json(status);
                interval              = cadence_for(status);
                snapshot["poll_ms"]   = static_cast<int>(interval.count());
                push(session, alive, self, std::move(snapshot));
            } else {
                // Keep going: the printer coming back is exactly the case this has to survive.
                push(session, alive, self, session->offline(message.ToUTF8().data()));
            }

            if (!session->wait(interval))
                break;
        }
    }

    /// Any thread. Caches the snapshot on the session and hands it to the GUI thread to deliver.
    static void push(const std::shared_ptr<PollSession>&      session,
                     const std::shared_ptr<std::atomic_bool>& alive,
                     FlashforgeConsoleHandler*                self,
                     json                                     snapshot)
    {
        session->set_status(snapshot);

        // A stopped session is a session nobody is listening to, and possibly one whose handler is
        // already gone - so it does not even queue the hop.
        if (session->stopped() || !*alive)
            return;

        wxGetApp().CallAfter([alive, self, snapshot = std::move(snapshot)]() mutable {
            if (*alive)
                self->answer(0, std::move(snapshot)); // id 0 = unsolicited push
        });
    }

    /// True between on_suspended() and on_shown(). While set, nothing starts a session - not a
    /// `status` request, not start_polling() called from anywhere else.
    bool                         m_suspended = false;
    std::shared_ptr<PollSession> m_session;
    /// What last_status() answers with while there is no session: the reason the last one stopped,
    /// or why one could not start.
    json m_no_session_status;

    // Cleared before anything else in the destructor, so a CallAfter queued by a detached poll
    // thread cannot reach a handler that no longer exists.
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
