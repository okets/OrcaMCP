#pragma once
#include <algorithm>
#include <nlohmann/json.hpp>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <stdexcept>
#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {
void register_bambu_print_tools();

struct BambuPrintFacts {
    bool modal = false, busy = false, slice_valid = false, connected = false;
    bool idle = false, storage_ready = false, ams_ready = false, credentials_ready = false;
    double age_s = -1, nozzle_mm = 0, profile_nozzle_mm = 0;
    std::string printer_type, profile_model, slice_id;
};
struct BambuFilament {
    int project_filament = 0;
    std::string type, profile_id, color;
    int nozzle_id = -1; // native cloud task nozzle ID, when the project supplies a filament map
};
struct BambuMapping {
    int project_filament = 0, ams_id = -1, slot_id = -1;
    bool present = false, ready = false, reading = true;
    std::string type, profile_id, color;
};
inline std::optional<std::string> bambu_print_refusal(const BambuPrintFacts& f, const std::string& expected) {
    if (f.modal || f.busy) return "Orca has an open modal dialog or a busy slicing/job worker";
    if (!f.slice_valid || f.slice_id.empty()) return "The selected plate needs a valid current slice and passing G-code check";
    if (!expected.empty() && expected != f.slice_id) return "The selected plate's slice identity changed; inspect again";
    if (f.printer_type != "C12" || f.profile_model != "Bambu Lab P1S" ||
        std::abs(f.nozzle_mm - 0.4) > 0.001 || std::abs(f.profile_nozzle_mm - 0.4) > 0.001 ||
        !std::isfinite(f.nozzle_mm) || !std::isfinite(f.profile_nozzle_mm))
        return "Native start currently requires a P1S with reported and sliced 0.4 mm nozzle";
    if (!f.connected || !std::isfinite(f.age_s) || f.age_s < 0 || f.age_s > 15)
        return "Native printer telemetry is disconnected, unknown or older than 15 seconds";
    if (!f.idle) return "Native printer state is not positively idle or has an error";
    if (!f.credentials_ready) return "Native LAN printer configuration is incomplete";
    if (!f.storage_ready) return "Native printer storage is not ready";
    if (!f.ams_ready) return "Native AMS state is not ready and idle";
    return {};
}
inline std::optional<std::string> bambu_mapping_refusal(const std::vector<BambuFilament>& used, const std::vector<BambuMapping>& mappings) {
    if (used.empty() || mappings.size() != used.size()) return "Map every used project filament exactly once";
    std::set<int> seen;
    for (const auto& m : mappings) {
        if (!seen.insert(m.project_filament).second) return "A project filament is mapped more than once";
        const BambuFilament* source = nullptr;
        for (const auto& f : used) if (f.project_filament == m.project_filament) source = &f;
        if (!source) return "Mapping names an unused or missing project filament";
        if (m.ams_id < 0 || m.ams_id > 3 || m.slot_id < 0 || m.slot_id > 3)
            return "P1S AMS and slot IDs must be zero-based values from 0 through 3";
        if (!m.present || !m.ready || m.reading) return "Mapped AMS slot is empty, unknown or reading";
        if (source->type.empty() || source->profile_id.empty() || m.type != source->type || m.profile_id != source->profile_id)
            return "Mapped AMS material family or native filament profile ID does not match the project";
    }
    return {};
}
// Native arrays always span the entire project filament list, including unused entries.
inline nlohmann::json bambu_mapping_arrays(size_t count, const std::vector<BambuFilament>& filaments,
    const std::vector<BambuMapping>& mappings) {
    using json = nlohmann::json;
    json legacy = json::array(), modern = json::array(), info = json::array();
    for (size_t index = 0; index < count; ++index) {
        int tray = -1;
        json slot{{"ams_id", 255}, {"slot_id", 255}};
        json metadata{{"ams", -1}, {"targetColor", ""}, {"filamentId", ""}, {"filamentType", ""}};
        for (const auto& m : mappings) if (m.project_filament == int(index + 1)) {
            const auto source = std::find_if(filaments.begin(), filaments.end(), [&](const BambuFilament& f) { return f.project_filament == m.project_filament; });
            if (source == filaments.end()) throw std::invalid_argument("Unvalidated Bambu mapping");
            tray = m.ams_id * 4 + m.slot_id;
            slot = {{"ams_id", m.ams_id}, {"slot_id", m.slot_id}};
            metadata = {{"ams", tray}, {"filamentId", source->profile_id}, {"filamentType", source->type},
                {"sourceColor", source->color}, {"targetColor", m.color}};
            if (source->nozzle_id >= 0) metadata["nozzleId"] = source->nozzle_id;
        }
        legacy.push_back(tray); modern.push_back(slot); info.push_back(metadata);
    }
    return {{"ams_mapping", legacy}, {"ams_mapping2", modern}, {"ams_mapping_info", info}};
}

// Main-thread, process-lifetime tombstones. Never erase a request after queueing or uncertainty.
class BambuSubmissionLedger {
    std::map<std::string, std::string> requests;
public:
    bool reserve(const std::string& id, const std::string& identity) { return requests.emplace(id, identity).second; }
    const std::string* find(const std::string& id) const { auto it = requests.find(id); return it == requests.end() ? nullptr : &it->second; }
};
inline bool bambu_dispatch_once(bool dry_run, const std::string& id, const std::string& identity,
    BambuSubmissionLedger& ledger, const std::function<void()>& dispatch) {
    if (dry_run || !ledger.reserve(id, identity)) return false;
    dispatch();
    return true;
}
}}}
