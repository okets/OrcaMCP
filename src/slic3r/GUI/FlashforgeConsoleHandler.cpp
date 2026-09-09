#include "FlashforgeConsoleHandler.hpp"

#include "PrinterWebView.hpp"
#include "PrinterWebViewHandler.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPProjectMatch.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/Flashforge.hpp"
#include "slic3r/Utils/FlashforgeApi.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

// ── Command shaping ──────────────────────────────────────────────────────────────────────────

/// The vendor's own speed steps, and the only ones the page offers. A value from anywhere else is
/// refused rather than passed through to the firmware.
const double PRINT_SPEEDS[] = {50, 100, 125, 166};

/// What the machine will accept as a target, generously bounded. The printer is still the
/// authority - these only stop a typo becoming a command.
constexpr double MAX_NOZZLE_TEMP = 350, MAX_BED_TEMP = 150, MAX_CHAMBER_TEMP = 100;
/// Z compensation is an offset, nudged one step at a time; a whole millimetre is already far more
/// than a first layer.
constexpr double MAX_Z_COMPENSATION = 1.0;

/// The printer's untouched `detail` object inside a cached snapshot, or null when nothing has
/// arrived yet.
const json* raw_detail(const json& snapshot)
{
    if (!snapshot.is_object())
        return nullptr;
    const auto printer = snapshot.find("printer");
    if (printer == snapshot.end() || !printer->is_object())
        return nullptr;
    const auto raw = printer->find("raw");
    return (raw != printer->end() && raw->is_object()) ? &*raw : nullptr;
}

double json_double(const json& obj, const char* key, double fallback)
{
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_number()) ? it->get<double>() : fallback;
}

bool has_number(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_number();
}

/// Replaces `value` only when the page actually sent that field: everything else keeps what the
/// printer currently reports, which is the whole point of reading the snapshot. A field that is
/// present but not a number is refused rather than quietly dropped - silently keeping the current
/// speed when the caller meant to change it is the kind of near-miss nobody notices.
bool override_number(const json& params, const char* key, double& value, std::string& error)
{
    const auto it = params.find(key);
    if (it == params.end() || it->is_null())
        return true;
    if (!it->is_number()) {
        error = into_u8(wxString::Format(_L("'%s' must be a number."), from_u8(key)));
        return false;
    }
    value = it->get<double>();
    return true;
}

/// One half of circulateCtl_cmd: the side being toggled comes from the page, the other from the
/// printer's own report, so switching the exhaust cannot close the recirculation.
bool read_switch(const json& params, const json& raw, const char* param_key, const char* raw_key,
                 std::string& out, std::string& error)
{
    const auto it = params.find(param_key);
    if (it == params.end() || it->is_null()) {
        out = raw.value(raw_key, std::string()) == "open" ? "open" : "close";
        return true;
    }
    const std::string value = it->is_string() ? it->get<std::string>() : std::string();
    if (value != "open" && value != "close") {
        error = into_u8(wxString::Format(_L("'%s' must be \"open\" or \"close\"."), from_u8(param_key)));
        return false;
    }
    out = value;
    return true;
}

/// A temperature the page sent, bounded. Absent or null means "leave this one alone"; 0 means
/// "off", and the two must never be confused - the firmware's own sentinel for no-change is -200,
/// which set_temperatures writes for an empty optional.
bool read_target(const json& params, const char* key, double max_value, const wxString& label, json& out, std::string& error)
{
    out = nullptr;
    const auto it = params.find(key);
    if (it == params.end() || it->is_null())
        return true;

    const double value = it->is_number() ? it->get<double>() : -1;
    if (!it->is_number() || value < 0 || value > max_value) {
        error = into_u8(wxString::Format(_L("%s must be between 0 and %d °C."), label, static_cast<int>(max_value)));
        return false;
    }
    out = value;
    return true;
}

std::optional<double> to_optional(const json& value)
{
    return value.is_number() ? std::optional<double>(value.get<double>()) : std::nullopt;
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
    /// Set by poke(); cleared by the wait it shortens. A separate flag rather than a shorter
    /// interval, so a wake that lands while the loop is inside fetch_status is not lost.
    bool wake = false;

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

    /// Asks for one immediate fetch without ending the session. A control command has just changed
    /// something and the page is holding a pending state until the printer confirms it, so the
    /// shorter that wait the better.
    void poke()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            wake = true;
        }
        cv.notify_all();
    }

    bool stopped()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return stop;
    }

    /// Sleeps up to `interval`, waking early when the session is stopped or poked. False means
    /// "stop now".
    bool wait(std::chrono::milliseconds interval)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, interval, [this]() { return stop || wake; });
        wake = false;
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

        if (method == "command") {
            handle_command(id, request.value("params", json::object()));
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
        // Whether the project still says what the machine is holding is a question only the GUI
        // thread can answer, and this is the one place every snapshot passes through on it. Null
        // when they agree, which is what keeps the page's suggestion off the screen.
        result["project_match"] = OrcaMCP::project_match_suggestion(result);
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

    /// The page holds the control in a pending state until this arrives, and reverts it on ok:false
    /// with `error` shown inline beside the control - never in a dialog.
    void answer_command(int id, bool ok, const std::string& error, json result = json())
    {
        run_in_page(json{{"id", id}, {"method", "command"}, {"ok", ok}, {"error", error},
                         {"result", std::move(result)}});
    }

    // ── Commands (dispatch on the GUI thread, execute off it) ────────────────────────────────

    /// Resolves what the command needs from the GUI thread - the preset's host and credentials, and
    /// the cached snapshot the untouched fields come from - then hands the blocking part to a
    /// worker. Nothing here talks to the printer.
    void handle_command(int id, const json& params)
    {
        if (m_suspended) {
            // The page is parked because its printer is no longer the selected one. A parked page
            // must not be able to move a machine the user has looked away from.
            answer_command(id, false, into_u8(_L("This printer is not the selected one any more.")));
            return;
        }

        // Ahead of the print-host lookup: this is the one command that never reaches the printer,
        // so a preset with no host configured must not make it fail with a host error. It reads the
        // station snapshot the poller already cached and rewrites the *project*. Preset work is
        // main-thread-only and fast, so it runs right here rather than on a worker - and through
        // exactly the helper the match_project_to_printer MCP tool calls, so the button and an
        // agent take the same path.
        if (params.value("name", std::string()) == "match_project_to_printer") {
            handle_project_match(id, params);
            return;
        }

        DynamicPrintConfig config;
        std::string        host_type, error;
        if (!OrcaMCP::resolve_print_host_config(config, host_type, error)) {
            answer_command(id, false, error);
            return;
        }

        json operation;
        if (!build_console_operation(params, last_status(), operation, error)) {
            answer_command(id, false, error);
            return;
        }

        // Idempotent, and the reason the poke below has something to wake: a console whose session
        // had ended (no credentials at the time, say) gets one back before its first command lands.
        start_polling();

        std::thread(&FlashforgeConsoleHandler::run_command, m_session, m_alive, this, std::move(config),
                    std::move(operation), id)
            .detach();
    }

    /// Reads the printer's material station out of the last snapshot and matches the project to it.
    /// GUI thread throughout: nothing here talks to the printer.
    void handle_project_match(int id, const json& params)
    {
        const json snapshot = last_status();
        const auto station  = OrcaMCP::material_slots_from_json(snapshot);
        if (station.empty()) {
            answer_command(id, false,
                           into_u8(_L("The printer has not reported its material station yet.")));
            return;
        }

        // Parsed the same way, and as strictly, as the MCP tool: a value nobody can read is refused
        // rather than dropped, because dropping a slot id silently would match every loaded slot.
        std::vector<int> slots;
        if (const auto requested = params.find("slots"); requested != params.end() && !requested->is_null()) {
            if (!requested->is_array()) {
                answer_command(id, false, into_u8(_L("slots must be a list of slot numbers.")));
                return;
            }
            for (const auto& value : *requested) {
                int slot = 0;
                if (!OrcaMCP::parse_integer_param(value, slot) || slot < 1) {
                    answer_command(id, false, into_u8(_L("slots must be whole numbers from 1 up.")));
                    return;
                }
                slots.push_back(slot);
            }
        }

        bool dry_run = false;
        if (const auto asked = params.find("dry_run"); asked != params.end() && !asked->is_null() &&
                                                       !OrcaMCP::parse_boolean_param(*asked, dry_run)) {
            answer_command(id, false, into_u8(_L("dry_run must be true or false.")));
            return;
        }

        json result = OrcaMCP::match_project_to_printer(station, slots, dry_run);
        const bool ok = result.value("status", std::string()) != "error";
        answer_command(id, ok, ok ? std::string() : result.value("message", std::string()), std::move(result));

        // The suggestion is computed from a snapshot, so the page needs a fresh one to see it go.
        if (ok)
            answer(0, last_status());
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

    // ── Commands (its own thread) ────────────────────────────────────────────────────────────

    /// One control request. Same discipline as the poll loop: its own config copy, a detached
    /// thread nobody waits for, and a reply that only reaches the handler through `alive`.
    static void run_command(std::shared_ptr<PollSession>      session,
                            std::shared_ptr<std::atomic_bool> alive,
                            FlashforgeConsoleHandler*         self,
                            DynamicPrintConfig                config,
                            json                              operation,
                            int                               id)
    {
        const Flashforge  host(&config);
        const std::string kind = operation.value("kind", std::string());
        wxString          msg;
        bool              ok = false;

        if (kind == "light") {
            ok = host.set_light(operation.value("on", false), msg);
        } else if (kind == "job") {
            // Explicit, never a ternary's else: an action nobody recognises must not fall through
            // to cancelling somebody's print.
            const std::string action = operation.value("action", std::string());
            if (action == "pause")
                ok = host.pause_job(msg);
            else if (action == "resume")
                ok = host.resume_job(msg);
            else if (action == "stop")
                ok = host.cancel_job(msg);
            else
                msg = _L("Unknown job action.");
        } else if (kind == "temperature") {
            std::vector<std::optional<double>> nozzles;
            for (const auto& target : operation["nozzles"])
                nozzles.push_back(to_optional(target));
            ok = host.set_temperatures(to_optional(operation["bed"]), to_optional(operation["chamber"]), nozzles, msg);
        } else {
            ok = host.send_control(operation.value("cmd", std::string()), operation.value("args", json::object()), msg);
        }

        // The page is showing a pending control until the printer itself confirms the change, so
        // the next snapshot is wanted now rather than at the end of the cadence.
        if (ok && session)
            session->poke();

        const std::string error = ok ? std::string()
                                     : (msg.empty() ? into_u8(_L("The printer refused the command."))
                                                    : std::string(msg.ToUTF8().data()));
        if (!*alive)
            return;
        wxGetApp().CallAfter([alive, self, id, ok, error]() {
            if (*alive)
                self->answer_command(id, ok, error);
        });
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

bool build_console_operation(const json& params, const json& snapshot, json& operation, std::string& error)
{
    const std::string name = params.value("name", std::string());

    if (name == "light") {
        const auto on = params.find("on");
        if (on == params.end() || !on->is_boolean()) {
            error = _u8L("The light command needs an on/off value.");
            return false;
        }
        operation = json{{"kind", "light"}, {"on", on->get<bool>()}};
        return true;
    }

    if (name == "job") {
        const std::string action = params.value("action", std::string());
        if (action != "pause" && action != "resume" && action != "stop") {
            error = into_u8(wxString::Format(_L("Unknown job action '%s'."), from_u8(action)));
            return false;
        }
        operation = json{{"kind", "job"}, {"action", action}};
        return true;
    }

    if (name == "temperature") {
        json bed, chamber;
        if (!read_target(params, "bed", MAX_BED_TEMP, _L("Bed target"), bed, error) ||
            !read_target(params, "chamber", MAX_CHAMBER_TEMP, _L("Chamber target"), chamber, error))
            return false;

        // Four entries whatever the page sent, because that is the array the firmware takes; a null
        // is the one that becomes "leave this nozzle alone", and 0 is a real instruction to cool.
        const auto sent    = params.find("nozzles");
        json       nozzles = json::array();
        for (size_t tool = 0; tool < 4; ++tool) {
            json target = nullptr;
            if (sent != params.end() && sent->is_array() && tool < sent->size()) {
                const json one = json{{"t", sent->at(tool)}};
                if (!read_target(one, "t", MAX_NOZZLE_TEMP, _L("Nozzle target"), target, error))
                    return false;
            }
            nozzles.push_back(target);
        }

        operation = json{{"kind", "temperature"}, {"bed", bed}, {"chamber", chamber}, {"nozzles", nozzles}};
        return true;
    }

    // The two commands below each carry every field they own. Without a snapshot to read the
    // untouched ones from, sending either would reset whatever it did not mention.
    const json* raw = raw_detail(snapshot);
    if (raw == nullptr && (name == "filtration" || name == "printer_ctl")) {
        error = _u8L("The printer has not reported its current settings yet.");
        return false;
    }

    if (name == "filtration") {
        std::string internal, external;
        if (!read_switch(params, *raw, "internal", "internalFanStatus", internal, error) ||
            !read_switch(params, *raw, "external", "externalFanStatus", external, error))
            return false;
        operation = json{{"kind", "control"},
                         {"cmd", "circulateCtl_cmd"},
                         {"args", {{"internal", internal}, {"external", external}}}};
        return true;
    }

    if (name == "printer_ctl") {
        double z           = json_double(*raw, "zAxisCompensation", 0);
        double speed       = json_double(*raw, "printSpeedAdjust", 0);
        double chamber_fan = json_double(*raw, "chamberFanSpeed", 0);
        double cooling_fan = json_double(*raw, "coolingFanSpeed", 0);

        // An idle printer reports 0 % speed, which is not a speed it would accept back. Carrying it
        // into a Z nudge would ask the machine to stop moving, so an untouched speed reads 100 %.
        if (!(speed > 0))
            speed = 100;

        const bool changing_speed = params.contains("speed");
        if (!override_number(params, "zAxisCompensation", z, error) ||
            !override_number(params, "speed", speed, error) ||
            !override_number(params, "chamberFan", chamber_fan, error) ||
            !override_number(params, "coolingFan", cooling_fan, error))
            return false;

        if (changing_speed && std::find(std::begin(PRINT_SPEEDS), std::end(PRINT_SPEEDS), speed) == std::end(PRINT_SPEEDS)) {
            error = _u8L("Print speed must be one of 50, 100, 125 or 166 %.");
            return false;
        }
        if (std::abs(z) > MAX_Z_COMPENSATION) {
            error = _u8L("Z offset must be within \u00b11 mm.");
            return false;
        }

        json args = {{"zAxisCompensation", z},
                     {"speed", speed},
                     {"chamberFan", chamber_fan},
                     {"coolingFan", cooling_fan}};

        // Only a machine that reports a left cooling fan is told what to do with one. The Creator 5
        // reports none; a dual-head Flashforge does, and sending a 0 we invented would stop its
        // left part cooling in the middle of a print.
        if (has_number(*raw, "coolingLeftFanSpeed") || params.contains("coolingLeftFan")) {
            double cooling_left_fan = json_double(*raw, "coolingLeftFanSpeed", 0);
            if (!override_number(params, "coolingLeftFan", cooling_left_fan, error))
                return false;
            args["coolingLeftFan"] = cooling_left_fan;
        }

        operation = json{{"kind", "control"}, {"cmd", "printerCtl_cmd"}, {"args", std::move(args)}};
        return true;
    }

    error = into_u8(wxString::Format(_L("Unknown command '%s'."), from_u8(name)));
    return false;
}

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
