#ifndef __FLASHFORGE_PRINTER_AGENT_HPP__
#define __FLASHFORGE_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <wx/string.h>

namespace Slic3r {

class Flashforge;

/**
 * FlashforgePrinterAgent - Device tab support for Flashforge's local HTTP API.
 *
 * The Device tab (MonitorPanel + MachineObject) speaks one dialect: Bambu-shaped
 * `push_status` messages in, Bambu-shaped commands out. Flashforge printers speak their own
 * LAN JSON API, already wrapped by the Flashforge print host. This agent is the adapter
 * between the two and holds no state of its own beyond that translation:
 *
 *   poll (worker thread) -> Flashforge::fetch_status -> flashforge_status_to_bambu_payload
 *                        -> queue_on_main_fn -> MachineObject::parse_json
 *   command (GUI thread) -> Flashforge::pause_job / resume_job / cancel_job / set_light
 *
 * Everything the Flashforge API has no translation for (binding, cloud relay, print jobs -
 * those go through the print host's upload path, not the agent) reports
 * ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED rather than pretending to succeed.
 */
class FlashforgePrinterAgent final : public IPrinterAgent
{
public:
    explicit FlashforgePrinterAgent(std::string log_dir);
    ~FlashforgePrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Cloud Agent Dependency
    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    // Certificates - the local API authenticates with a serial + check code, not certificates.
    int  check_cert() override;
    void install_device_cert(std::string dev_id, bool lan_only) override;

    // Discovery
    bool start_discovery(bool start, bool sending) override;

    // Binding - no account to bind to; the printer is reached directly over the LAN.
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int bind(std::string dev_ip,
             std::string dev_id,
             std::string dev_model,
             std::string sec_link,
             std::string timezone,
             bool        improved,
             OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    int request_bind_ticket(std::string* ticket) override;
    int get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine Selection
    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    // Print Job Operations - Flashforge uploads go through the print host (PrintHostSendDialog),
    // so the agent-side job API has no translation.
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    // Callbacks
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

private:
    // Command translation (all called on the thread that published the command).
    int handle_request(const std::string& dev_id, const std::string& json_str);
    int handle_print_command(const std::string& dev_id, const nlohmann::json& print);
    int handle_system_command(const std::string& dev_id, const nlohmann::json& system);
    int send_version_info(const std::string& dev_id);
    int send_access_code(const std::string& dev_id);
    int run_host_command(const std::function<bool(const Flashforge&, wxString&)>& command, const char* what);

    // Polling (worker thread).
    void start_polling(const std::string& dev_id);
    void stop_polling();
    void run_poll_loop(std::string dev_id);
    /// Sleeps up to `interval`, waking early when polling is stopped. False means "stop now".
    bool wait_for_next_poll(std::chrono::milliseconds interval);

    // Dispatch back into the GUI, marshalled through queue_on_main_fn when one is registered.
    /// `sync_machine` additionally runs sync_machine_object() on the GUI thread, immediately before
    /// the payload reaches MachineObject::parse_json. Only status pushes need it.
    void dispatch_message(const std::string& dev_id, const std::string& payload, bool sync_machine = false);
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg);
    void dispatch_printer_connected(const std::string& dev_id);

    /// GUI thread only. Fills in the MachineObject fields the Device tab needs but a Bambu-shaped
    /// `push_status` cannot carry, exactly as MoonrakerPrinterAgent does after its own pushes.
    void sync_machine_object(const std::string& dev_id) const;

    std::shared_ptr<Flashforge> get_host() const;

    static constexpr std::chrono::milliseconds POLL_INTERVAL_PRINTING{2000};
    static constexpr std::chrono::milliseconds POLL_INTERVAL_IDLE{5000};

    mutable std::recursive_mutex m_state_mutex; // guards everything below it
    std::shared_ptr<Flashforge>  m_host;
    std::string                  m_selected_machine;
    std::string                  m_access_code;
    std::string                  m_firmware_version; // learned from the first successful poll
    std::string                  m_model_id;         // vendor model_id of the selected preset

    // Only the callbacks this agent actually invokes are stored; the rest of the IPrinterAgent
    // setters accept and drop their argument rather than keeping a member nothing ever reads.
    OnPrinterConnectedFn m_on_printer_connected_fn;
    OnMessageFn          m_on_message_fn;
    OnLocalConnectedFn   m_on_local_connect_fn;
    OnMessageFn          m_on_local_message_fn;
    QueueOnMainFn        m_queue_on_main_fn;

    // Poll thread lifecycle. m_poll_mutex is only ever held for the flag + the cv wait, so
    // stopping never has to wait on m_state_mutex or on an in-flight HTTP request's lock.
    std::thread             m_poll_thread;
    std::mutex              m_poll_mutex;
    std::condition_variable m_poll_cv;
    bool                    m_poll_stop = true;
};

} // namespace Slic3r

#endif // __FLASHFORGE_PRINTER_AGENT_HPP__
