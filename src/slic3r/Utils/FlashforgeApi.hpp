#ifndef slic3r_Utils_FlashforgeApi_hpp_
#define slic3r_Utils_FlashforgeApi_hpp_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace FlashforgeApi {

struct NozzleTemp { double current{0}; double target{0}; };
struct MaterialSlot { int slot_id{0}; bool has_filament{false}; std::string material_name, material_color; };
struct PrinterStatus {
    std::string state;                 // normalized: ready|busy|heating|printing|paused|completed|error|cancelled|unknown, or the printer's own (downloading, unzipping)
    std::string print_file; double progress{0}; long duration_s{0};
    // Seconds left, PROJECTED from elapsed time and progress: duration_s * (1 - p) / p. The firmware's
    // own `estimatedTime` is not usable for this -- on 1.9.9 it tracked printDuration exactly on one
    // job, so a console that trusted it showed "17 min left" 17 minutes into a nine-hour job, and held
    // the job's whole estimate on another (2026-09-30). -1 when unknown:
    // no active job, no elapsed time yet, or progress under 2 %, where the projection swings wildly.
    long remaining_s{-1};
    // The firmware's `estimatedTime` as reported, untouched, so its real meaning can be studied
    // against more firmware versions. Never shown to the user as remaining time.
    long firmware_estimated_s{0};
    double bed_temp{0}, bed_target{0}, chamber_temp{0}, chamber_target{0};
    std::vector<NozzleTemp> nozzles;
    bool light_on{false}; std::string door; std::string error_code;
    // Installed nozzle bore in mm, from the printer's own `nozzleModel` ("0.4mm;0.4mm;..."). 0 when
    // the firmware does not report it -- the UI already renders an unknown diameter as "Unknown".
    double nozzle_diameter{0};
    // Nozzle material ("hardened_steel", "brass", ...). The local API reports a bore but never a
    // material, so this is not parsed from the printer: the caller fills it from the machine preset,
    // which is where the user declares what is fitted. Empty when unknown.
    std::string nozzle_type;
    std::string name, model, firmware, ip, camera_stream_url; int pid{0};
    bool has_material_station{false}; std::vector<MaterialSlot> slots;
    nlohmann::json raw;                // the untouched `detail` object
    // The job and control numbers of this report that no reading can be, each as the field and the
    // value the printer sent ("chamberFanSpeed 5177344"); empty when every one is a reading. Firmware
    // 1.9.9 fills all of them from other memory for half a second after a print starts (states
    // `downloading` and `unzipping`, 2026-09-30), some or all in range, so one entry means none of
    // them is the printer's: progress, the times, layers, speed, Z offset and fans. `remaining_s` is
    // then -1; `raw` keeps what was sent.
    std::vector<std::string> implausible_telemetry;
};

// The local API is inconsistent about how it types its integers: `code`/`err`/`hasMatlStation`/
// `slotCnt` arrive as a number on one firmware, a bool on another and a (sometimes padded) string on
// a third. Reads any of those into `out` and returns false -- without throwing -- for anything else.
bool try_parse_json_int(const nlohmann::json& value, int& out);

// Returns false with `error` set if the body is not a successful API response.
bool parse_detail(const std::string& body, PrinterStatus& out, std::string& error);

// Pure parsers for the local API's two list-shaped responses. Both take whatever the printer sent
// and skip any element that is not the shape they expect: firmware revisions we have not seen must
// cost us the one entry we cannot read, never the whole call.
// parse_gcode_list takes the parsed `gcodeList` response object (its `gcodeListDetail` sibling is
// the documented fallback) and returns the file names, accepting both a plain string element and a
// {"gcodeFileName": ...} object. Nameless entries are dropped.
std::vector<std::string> parse_gcode_list(const nlohmann::json& response);
// parse_material_slots takes the `slotInfos` array itself. A slot that does not report a `slotId`
// is numbered by its position in the result, 1-based, the way the API numbers them.
std::vector<MaterialSlot> parse_material_slots(const nlohmann::json& slot_infos);

// Re-shapes a PrinterStatus into the Bambu-flavoured `push_status` message that MachineObject
// (and therefore the Device tab) already knows how to parse. Pure and side-effect free: the
// caller stamps `t_utc` and dispatches. Optional fields are omitted rather than zero-filled so
// the UI keeps its last known value instead of flashing a fabricated one.
nlohmann::json flashforge_status_to_bambu_payload(const PrinterStatus& status);

// The switches the printer's own start screen offers (Creator 5 Pro, firmware 1.9.9: Flow
// Calibration, Leveling, Timelapse). The API's fourth, `firstLayerInspection`, is not on that
// screen and is always sent off.
struct PrintOptions
{
    bool leveling{false};
    bool flow_calibration{false};
    bool time_lapse{false};
};
bool operator==(const PrintOptions& a, const PrintOptions& b);

// The PrintHostUpload::extended_info of a Flashforge upload. Every sender builds it here
// (FlashforgePrintHostSendDialog, send_to_printer, FlashforgePrinterAgent) and
// Flashforge::upload_local_api turns it into the request's headers, so the keys are spelled once.
// `mappings` is the {toolId, slotId, ...} array the printer is sent; empty without the station.
std::map<std::string, std::string> make_upload_extended_info(const PrintOptions& options, bool use_material_station, const nlohmann::json& mappings);

// The options an extended_info carries. Only "1" is on; a missing key is off.
PrintOptions read_upload_print_options(const std::map<std::string, std::string>& extended_info);

// A stored file's `printingTime` in whole seconds, from a gcodeList response: its gcodeListDetail
// entry, else a {"gcodeFileName": ...} object in gcodeList. nullopt when the printer does not give a
// positive, finite number (older firmware, a plain name, 0, a flag, text that is not a number) or
// the file is not listed.
std::optional<long> parse_gcode_printing_time(const nlohmann::json& response, const std::string& file_name);

nlohmann::json make_credentials_payload(const std::string& serial, const std::string& check_code);
nlohmann::json make_control_payload(const std::string& serial, const std::string& check_code, const std::string& cmd, const nlohmann::json& args);
nlohmann::json make_temperature_args(std::optional<double> bed, std::optional<double> chamber, const std::vector<std::optional<double>>& nozzles); // -200 for absent
// printGcode's body. `timeLapseVideo` is there only when the time-lapse is asked for.
nlohmann::json make_print_gcode_payload(const std::string& serial, const std::string& check_code, const std::string& file_name, const PrintOptions& options, const nlohmann::json& material_mappings);

constexpr int kTempNoChange = -200;
constexpr int kPidCreator5 = 40, kPidCreator5Pro = 41;

// Whether the machine's own start screen is known to offer flow calibration and leveling before a
// print: the Creator 5 and 5 Pro, by the product id the printer reports (`detail.pid`). Other
// local-API Flashforges (Adventurer 5M, AD5X, ...) have not been checked, so nothing turns
// calibration on for them unasked, and the send dialog offers them no flow calibration.
bool start_screen_offers_calibration(int product_id);

// The machine as a report names it: the printer's own `model`, else its product id, else "unknown".
std::string printer_model_name(const PrinterStatus& status);

}} // namespace Slic3r::FlashforgeApi

#endif
