#include "FlashforgePrinterAgent.hpp"

#include "Flashforge.hpp"
#include "FlashforgeApi.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <boost/log/trivial.hpp>

#include <chrono>
#include <utility>

namespace Slic3r {

namespace {

const std::string FlashforgePrinterAgent_VERSION = "1.0.0";

// The Device tab expects a non-empty access code; Flashforge's check code is a secret we do
// not want on screen, so the placeholder Orca already uses elsewhere stands in for it.
const char* const ACCESS_CODE_PLACEHOLDER = "88888888";

uint64_t now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string json_string(const nlohmann::json& obj, const char* key)
{
    if (obj.contains(key) && obj[key].is_string())
        return obj[key].get<std::string>();
    return {};
}

} // namespace

FlashforgePrinterAgent::FlashforgePrinterAgent(std::string log_dir) : m_log_dir(std::move(log_dir)) {}

FlashforgePrinterAgent::~FlashforgePrinterAgent() { stop_polling(); }

AgentInfo FlashforgePrinterAgent::get_agent_info_static()
{
    return AgentInfo{"flashforge", "FlashForge", FlashforgePrinterAgent_VERSION, "FlashForge LAN API printer agent"};
}

void FlashforgePrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_cloud_agent = std::move(cloud);
}

// ============================================================================
// Communication
// ============================================================================

int FlashforgePrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos;
    (void) flag;
    return handle_request(dev_id, json_str);
}

int FlashforgePrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    (void) qos;
    (void) flag;
    return handle_request(dev_id, json_str);
}

int FlashforgePrinterAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    (void) username;
    (void) use_ssl; // the local API is plain HTTP on its own port

    if (dev_id.empty()) {
        BOOST_LOG_TRIVIAL(error) << "FlashforgePrinterAgent: connect_printer missing dev_id";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    // The host is built from the printer preset, which is where print_host, the serial number
    // and the check code live. dev_ip carries only host:port, not the credentials.
    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (!preset_bundle) {
        BOOST_LOG_TRIVIAL(error) << "FlashforgePrinterAgent: no preset bundle; cannot reach the printer";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    DynamicPrintConfig config = preset_bundle->printers.get_edited_preset().config;
    auto               host   = std::make_shared<Flashforge>(&config);
    if (!host->has_local_api_credentials()) {
        BOOST_LOG_TRIVIAL(warning) << "FlashforgePrinterAgent: printer preset has no serial number / access code; "
                                      "status polling is unavailable";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    stop_polling(); // a previous selection may still be polling a different printer

    {
        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
        m_host        = std::move(host);
        m_access_code = password.empty() ? ACCESS_CODE_PLACEHOLDER : password;
        m_firmware_version.clear();
    }

    start_polling(dev_id);
    BOOST_LOG_TRIVIAL(info) << "FlashforgePrinterAgent: connect_printer dev_id=" << dev_id << " host=" << dev_ip;
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::disconnect_printer()
{
    stop_polling();
    {
        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
        m_host.reset();
        m_firmware_version.clear();
    }
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Certificates / Discovery / Binding
// ============================================================================

int FlashforgePrinterAgent::check_cert() { return BAMBU_NETWORK_SUCCESS; }

void FlashforgePrinterAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    (void) dev_id;
    (void) lan_only;
}

bool FlashforgePrinterAgent::start_discovery(bool start, bool sending)
{
    // GUI_App::select_machine creates the MachineObject straight from the selected preset, so
    // there is nothing to announce here; the SSDP probe Flashforge does have belongs to the
    // "Discover" button in PhysicalPrinterDialog, not to device selection.
    (void) start;
    (void) sending;
    return true;
}

int FlashforgePrinterAgent::ping_bind(std::string ping_code)
{
    (void) ping_code;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    (void) dev_ip;
    (void) sec_link;
    (void) detect;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::bind(std::string      dev_ip,
                                 std::string      dev_id,
                                 std::string      dev_model,
                                 std::string      sec_link,
                                 std::string      timezone,
                                 bool             improved,
                                 OnUpdateStatusFn update_fn)
{
    (void) dev_ip;
    (void) dev_id;
    (void) dev_model;
    (void) sec_link;
    (void) timezone;
    (void) improved;
    (void) update_fn;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::unbind(std::string dev_id)
{
    (void) dev_id;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::request_bind_ticket(std::string* ticket)
{
    if (ticket)
        ticket->clear();
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback)
{
    // No cloud snapshot source; report failure so the caller falls back (see OrcaPrinterAgent).
    (void) dev_id;
    (void) file_name;
    (void) callback;
    return -1;
}

int FlashforgePrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_on_server_err_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Machine Selection
// ============================================================================

std::string FlashforgePrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_selected_machine;
}

int FlashforgePrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_selected_machine = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Print Job Operations
// ============================================================================

int FlashforgePrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    (void) wait_fn;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::start_local_print_with_record(PrintParams      params,
                                                          OnUpdateStatusFn update_fn,
                                                          WasCancelledFn   cancel_fn,
                                                          OnWaitFn         wait_fn)
{
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    (void) wait_fn;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::start_send_gcode_to_sdcard(PrintParams      params,
                                                       OnUpdateStatusFn update_fn,
                                                       WasCancelledFn   cancel_fn,
                                                       OnWaitFn         wait_fn)
{
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    (void) wait_fn;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    (void) params;
    (void) update_fn;
    (void) cancel_fn;
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

// ============================================================================
// Callback Registration
// ============================================================================

int FlashforgePrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_on_ssdp_msg_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_on_printer_connected_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_on_subscribe_failure_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_on_message_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_on_user_message_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_on_local_connect_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_on_local_message_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    m_queue_on_main_fn = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Command translation
// ============================================================================

std::shared_ptr<Flashforge> FlashforgePrinterAgent::get_host() const
{
    std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
    return m_host;
}

int FlashforgePrinterAgent::run_host_command(const std::function<bool(const Flashforge&, wxString&)>& command, const char* what)
{
    const std::shared_ptr<Flashforge> host = get_host();
    if (!host) {
        BOOST_LOG_TRIVIAL(warning) << "FlashforgePrinterAgent: " << what << " with no connected printer";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }

    wxString msg;
    if (!command(*host, msg)) {
        BOOST_LOG_TRIVIAL(error) << "FlashforgePrinterAgent: " << what << " failed: " << msg.ToUTF8().data();
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::handle_request(const std::string& dev_id, const std::string& json_str)
{
    const auto json = nlohmann::json::parse(json_str, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        BOOST_LOG_TRIVIAL(error) << "FlashforgePrinterAgent: invalid JSON request";
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    }

    // "pushall" asks the printer to re-send its full state. The poll loop already pushes the
    // full state on every tick, so acknowledging is honest - and keeps MachineObject from
    // logging a publish failure on every reconnect.
    if (json.contains("pushing") && json["pushing"].is_object())
        return BAMBU_NETWORK_SUCCESS;

    if (json.contains("print") && json["print"].is_object())
        return handle_print_command(dev_id, json["print"]);
    if (json.contains("system") && json["system"].is_object())
        return handle_system_command(dev_id, json["system"]);
    if (json.contains("info") && json["info"].is_object() && json_string(json["info"], "command") == "get_version")
        return send_version_info(dev_id);

    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::handle_print_command(const std::string& dev_id, const nlohmann::json& print)
{
    (void) dev_id;
    const std::string cmd = json_string(print, "command");

    if (cmd == "pause")
        return run_host_command([](const Flashforge& host, wxString& msg) { return host.pause_job(msg); }, "pause");
    if (cmd == "resume")
        return run_host_command([](const Flashforge& host, wxString& msg) { return host.resume_job(msg); }, "resume");
    if (cmd == "stop")
        return run_host_command([](const Flashforge& host, wxString& msg) { return host.cancel_job(msg); }, "stop");

    BOOST_LOG_TRIVIAL(info) << "FlashforgePrinterAgent: unsupported print command '" << cmd << "'";
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::handle_system_command(const std::string& dev_id, const nlohmann::json& system)
{
    const std::string cmd = json_string(system, "command");

    if (cmd == "get_access_code")
        return send_access_code(dev_id);

    if (cmd == "ledctrl") {
        // The UI drives chamber_light and chamber_light2 as one pair (DevLamp::CtrlSetChamberLight);
        // the printer has a single light, so only the first node reaches it.
        const std::string node = json_string(system, "led_node");
        if (node == "chamber_light2")
            return BAMBU_NETWORK_SUCCESS;
        if (node != "chamber_light")
            return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;

        const std::string mode = json_string(system, "led_mode");
        if (mode != "on" && mode != "off")
            return ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE; // e.g. "flashing": no Flashforge equivalent

        const bool on = mode == "on";
        return run_host_command([on](const Flashforge& host, wxString& msg) { return host.set_light(on, msg); }, "set_light");
    }

    BOOST_LOG_TRIVIAL(info) << "FlashforgePrinterAgent: unsupported system command '" << cmd << "'";
    return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
}

int FlashforgePrinterAgent::send_version_info(const std::string& dev_id)
{
    std::string version;
    {
        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
        version = m_firmware_version;
    }

    nlohmann::json payload;
    payload["info"]["command"] = "get_version";
    payload["info"]["result"]  = "success";
    payload["info"]["module"]  = nlohmann::json::array(
        {nlohmann::json{{"name", "ota"}, {"sw_ver", version}, {"product_name", "FlashForge"}}});

    dispatch_message(dev_id, payload.dump());
    return BAMBU_NETWORK_SUCCESS;
}

int FlashforgePrinterAgent::send_access_code(const std::string& dev_id)
{
    std::string access_code;
    {
        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
        access_code = m_access_code;
    }

    nlohmann::json payload;
    payload["system"]["command"]     = "get_access_code";
    payload["system"]["access_code"] = access_code;

    dispatch_message(dev_id, payload.dump());
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Polling
// ============================================================================

void FlashforgePrinterAgent::start_polling(const std::string& dev_id)
{
    stop_polling(); // never overwrite a joinable thread
    {
        std::lock_guard<std::mutex> lock(m_poll_mutex);
        m_poll_stop = false;
    }
    m_poll_thread = std::thread([this, dev_id]() { run_poll_loop(dev_id); });
}

void FlashforgePrinterAgent::stop_polling()
{
    {
        std::lock_guard<std::mutex> lock(m_poll_mutex);
        m_poll_stop = true;
    }
    m_poll_cv.notify_all();
    if (m_poll_thread.joinable())
        m_poll_thread.join();
}

bool FlashforgePrinterAgent::wait_for_next_poll(std::chrono::milliseconds interval)
{
    std::unique_lock<std::mutex> lock(m_poll_mutex);
    m_poll_cv.wait_for(lock, interval, [this]() { return m_poll_stop; });
    return !m_poll_stop;
}

void FlashforgePrinterAgent::run_poll_loop(std::string dev_id)
{
    bool announced_connected = false;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_poll_mutex);
            if (m_poll_stop)
                break;
        }

        const std::shared_ptr<Flashforge> host = get_host();
        if (!host)
            break;

        FlashforgeApi::PrinterStatus status;
        wxString                     msg;
        if (!host->fetch_status(status, msg)) {
            BOOST_LOG_TRIVIAL(debug) << "FlashforgePrinterAgent: status poll failed: " << msg.ToUTF8().data();
            // Keep polling: MachineObject times the connection out on its own when pushes stop.
            if (!wait_for_next_poll(POLL_INTERVAL_IDLE))
                break;
            continue;
        }

        if (!announced_connected) {
            {
                std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
                m_firmware_version = status.firmware;
            }
            dispatch_local_connect(ConnectStatusOk, dev_id, "0");
            dispatch_printer_connected(dev_id);
            announced_connected = true;
        }

        nlohmann::json payload = FlashforgeApi::flashforge_status_to_bambu_payload(status);
        payload["t_utc"]       = now_ms();
        dispatch_message(dev_id, payload.dump());

        const bool printing = payload["print"]["gcode_state"] == "RUNNING";
        if (!wait_for_next_poll(printing ? POLL_INTERVAL_PRINTING : POLL_INTERVAL_IDLE))
            break;
    }

    BOOST_LOG_TRIVIAL(info) << "FlashforgePrinterAgent: status polling stopped for dev_id=" << dev_id;
}

// ============================================================================
// Dispatch
// ============================================================================

void FlashforgePrinterAgent::dispatch_message(const std::string& dev_id, const std::string& payload)
{
    OnMessageFn   local_fn;
    OnMessageFn   cloud_fn;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
        local_fn = m_on_local_message_fn;
        cloud_fn = m_on_message_fn;
        queue_fn = m_queue_on_main_fn;
    }

    if (!local_fn && !cloud_fn) {
        BOOST_LOG_TRIVIAL(warning) << "FlashforgePrinterAgent: no message callback registered";
        return;
    }

    auto dispatch = [dev_id, payload, local_fn, cloud_fn]() {
        if (local_fn)
            local_fn(dev_id, payload);
        else
            cloud_fn(dev_id, payload);
    };

    if (queue_fn)
        queue_fn(dispatch);
    else
        dispatch();
}

void FlashforgePrinterAgent::dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg)
{
    OnLocalConnectedFn local_fn;
    QueueOnMainFn      queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
        local_fn = m_on_local_connect_fn;
        queue_fn = m_queue_on_main_fn;
    }
    if (!local_fn)
        return;

    auto dispatch = [state, dev_id, msg, local_fn]() { local_fn(state, dev_id, msg); };
    if (queue_fn)
        queue_fn(dispatch);
    else
        dispatch();
}

void FlashforgePrinterAgent::dispatch_printer_connected(const std::string& dev_id)
{
    OnPrinterConnectedFn connected_fn;
    QueueOnMainFn        queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
        connected_fn = m_on_printer_connected_fn;
        queue_fn     = m_queue_on_main_fn;
    }
    if (!connected_fn)
        return;

    auto dispatch = [dev_id, connected_fn]() { connected_fn(dev_id); };
    if (queue_fn)
        queue_fn(dispatch);
    else
        dispatch();
}

} // namespace Slic3r
