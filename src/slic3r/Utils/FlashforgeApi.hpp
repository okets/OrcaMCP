#ifndef slic3r_Utils_FlashforgeApi_hpp_
#define slic3r_Utils_FlashforgeApi_hpp_

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace FlashforgeApi {

struct NozzleTemp { double current{0}; double target{0}; };
struct MaterialSlot { int slot_id{0}; bool has_filament{false}; std::string material_name, material_color; };
struct PrinterStatus {
    std::string state;                 // normalized: ready|busy|heating|printing|paused|completed|error|cancelled|unknown
    std::string print_file; double progress{0}; long duration_s{0}; long remaining_s{0};
    double bed_temp{0}, bed_target{0}, chamber_temp{0}, chamber_target{0};
    std::vector<NozzleTemp> nozzles;
    bool light_on{false}; std::string door; std::string error_code;
    std::string name, model, firmware, ip, camera_stream_url; int pid{0};
    bool has_material_station{false}; std::vector<MaterialSlot> slots;
    nlohmann::json raw;                // the untouched `detail` object
};

// Returns false with `error` set if the body is not a successful API response.
bool parse_detail(const std::string& body, PrinterStatus& out, std::string& error);

// Re-shapes a PrinterStatus into the Bambu-flavoured `push_status` message that MachineObject
// (and therefore the Device tab) already knows how to parse. Pure and side-effect free: the
// caller stamps `t_utc` and dispatches. Optional fields are omitted rather than zero-filled so
// the UI keeps its last known value instead of flashing a fabricated one.
nlohmann::json flashforge_status_to_bambu_payload(const PrinterStatus& status);

nlohmann::json make_credentials_payload(const std::string& serial, const std::string& check_code);
nlohmann::json make_control_payload(const std::string& serial, const std::string& check_code, const std::string& cmd, const nlohmann::json& args);
nlohmann::json make_temperature_args(std::optional<double> bed, std::optional<double> chamber, const std::vector<std::optional<double>>& nozzles); // -200 for absent
nlohmann::json make_print_gcode_payload(const std::string& serial, const std::string& check_code, const std::string& file_name, bool leveling, const nlohmann::json& material_mappings);

constexpr int kTempNoChange = -200;
constexpr int kPidCreator5 = 40, kPidCreator5Pro = 41;

}} // namespace Slic3r::FlashforgeApi

#endif
