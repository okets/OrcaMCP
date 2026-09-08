#ifndef slic3r_FlashForge_hpp_
#define slic3r_FlashForge_hpp_

#include <optional>
#include <vector>
#include <string>
#include <wx/string.h>
#include <nlohmann/json_fwd.hpp>
#include "PrintHost.hpp"
#include "SerialMessage.hpp"
#include "SerialMessageType.hpp"
#include "FlashforgeApi.hpp"
#include "../../libslic3r/PrintConfig.hpp"

namespace Slic3r {
class DynamicPrintConfig;
class Http;

struct FlashforgeMaterialSlot
{
    int         slot_id {0}; // API is 1-based.
    bool        has_filament {false};
    std::string material_name;
    std::string material_color;
};

struct FlashforgeDiscoveredPrinter
{
    std::string name;
    std::string serial_number;
    std::string ip_address;
};

class Flashforge : public PrintHost
{
public:
    explicit Flashforge(DynamicPrintConfig *config);
    ~Flashforge() override = default;

    const char *get_name() const override;

    bool                       test(wxString &curl_msg) const override;
    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString &msg) const override;
    bool                       upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       has_auto_discovery() const override { return true; }
    bool                       can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string                get_host() const override { return m_host; }
    bool                       fetch_material_slots(std::vector<FlashforgeMaterialSlot>& slots, bool* supports_material_station, wxString& msg) const;
    static bool                discover_printers(std::vector<FlashforgeDiscoveredPrinter>& printers, wxString& msg, int timeout_ms = 10000, int idle_timeout_ms = 1500, int max_retries = 3);
    /// The page the Device (Web) tab should show for a Flashforge printer: its live MJPEG camera
    /// stream, which WebKit renders natively. Empty when the preset already names a `print_host_webui`
    /// (the user's choice wins) or has no host at all. See PrintHost::get_print_host_webui.
    static std::string         get_print_host_webui(DynamicPrintConfig* config);

    // Local API status and control. All are safe to call off the main thread; all return false and fill `msg` on failure.
    bool has_local_api_credentials() const { return !m_serial_number.empty() && !m_check_code.empty(); }
    bool fetch_status(FlashforgeApi::PrinterStatus& out, wxString& msg) const;
    bool send_control(const std::string& cmd, const nlohmann::json& args, wxString& msg) const;
    bool pause_job(wxString& msg) const;
    bool resume_job(wxString& msg) const;
    bool cancel_job(wxString& msg) const;
    bool set_light(bool on, wxString& msg) const;
    bool set_temperatures(std::optional<double> bed, std::optional<double> chamber, const std::vector<std::optional<double>>& nozzles, wxString& msg) const;
    bool list_gcode_files(std::vector<std::string>& files, wxString& msg) const;
    bool print_gcode_file(const std::string& file_name, bool leveling, const nlohmann::json& material_mappings, wxString& msg) const;

private:
    std::string m_host;
    std::string m_serial_number;
    std::string m_check_code;
    std::string m_console_port;
    const int m_bufferSize;
    GCodeFlavor m_gcFlavor;
    Slic3r::Utils::SerialMessage controlCommand          = {"~M601 S1\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage connectKlipperCommand   = {"~M640\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage connectLegacyCommand    = {"~M650\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage nozzlePosCommand        = {"~M114\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage deviceInfoCommand       = {"~M115\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage statusCommand           = {"~M119\r\n",Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage tempStatusCommand       = {"~M105\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage printStatusCommand      = {"~M27\r\n", Slic3r::Utils::Command};
    Slic3r::Utils::SerialMessage saveFileCommand         = {"~M29\r\n",Slic3r::Utils::Command};
    bool upload_local_api(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn) const;
    bool test_local_api(wxString& msg) const;
    bool request_local_api_json(const std::string& path, const std::string& body, std::string& response_body, wxString& error_msg) const;
    std::string make_http_url(const std::string& path) const;
    std::string extract_host_name() const;
    int  get_err_code_from_body(const std::string &body) const;
    bool connect(wxString& msg) const;
    bool start_print(wxString& msg, const std::string& filename) const;
};

} // namespace Slic3r

#endif
