#include "OrcaMCPBambuPrint.hpp"
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPGcodeCheck.hpp"
#include "OrcaMCPQuit.hpp"
#include "OrcaMCPResultIdentity.hpp"
#include "OrcaMCPSliceProgress.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/SelectMachine.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/Jobs/PrintJob.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <utility>
#include <wx/app.h>

namespace Slic3r { namespace GUI { namespace OrcaMCP {
namespace {
using json = nlohmann::json;
struct Submission {
    std::string printer_id, identity, state = "preparing";
};
// These maps are touched only on the main thread, including Job::finalize.
BambuSubmissionLedger ledger;
std::map<std::string, std::shared_ptr<Submission>> submissions;

// Keep native transport and lifecycle, but never open dialogs or forward raw transport errors.
class QuietCtl final : public Job::Ctl {
    Job::Ctl& parent;
public:
    explicit QuietCtl(Job::Ctl& ctl) : parent(ctl) {}
    void update_status(int, const std::string&) override {}
    bool was_canceled() const override { return parent.was_canceled(); }
    void clear_percent() override {}
    void show_error_info(const std::string&, int, const std::string&, const std::string&) override {}
    std::future<void> call_on_main_thread(std::function<void()> fn) override { return parent.call_on_main_thread(std::move(fn)); }
};
class SubmissionJob final : public Job {
    std::shared_ptr<PrintJob> native;
    std::shared_ptr<Submission> submission;
public:
    SubmissionJob(std::shared_ptr<PrintJob> job, std::shared_ptr<Submission> record)
        : native(std::move(job)), submission(std::move(record)) {}
    void process(Ctl& ctl) override {
        ctl.call_on_main_thread([record = submission] { record->state = "submitting"; }).wait();
        QuietCtl quiet(ctl);
        native->process(quiet);
    }
    void finalize(bool canceled, std::exception_ptr& error) override {
        native->finalize(canceled, error);
        submission->state = native->submission_succeeded() ? "submitted" :
            native->submission_attempted() ? "uncertain" : "failed_before_submission";
        // The caller can query a sanitized outcome. No modal exception handler.
        error = nullptr;
    }
};

std::string color_rgba(std::string color) {
    if (!color.empty() && color.front() == '#') color.erase(0, 1);
    if (color.size() == 6) color += "FF";
    return color;
}
struct Inspection {
    Plater* plater = nullptr;
    MachineObject* machine = nullptr;
    PartPlate* plate = nullptr;
    int plate_index = -1;
    BambuPrintFacts facts;
    std::vector<BambuFilament> filaments;
    std::vector<BambuMapping> mappings;
    size_t project_filament_count = 0;
};
std::optional<std::string> inspect(const json& args, Inspection& out) {
    out.plater = wxGetApp().plater();
    auto* manager = wxGetApp().getDeviceManager();
    auto* bundle = wxGetApp().preset_bundle;
    if (!out.plater || !manager || !bundle || !wxGetApp().getAgent()) return "Native Orca printer context is unavailable";
    // Require explicit identity AND the same native selected connection. Never select/connect here.
    out.machine = manager->get_selected_machine();
    if (!out.machine || out.machine->get_dev_id() != args.at("printer_id").get<std::string>())
        return "printer_id must match the currently selected native printer connection";
    auto& machine = *out.machine;
    if (!machine.is_lan_mode_printer()) return "Native MCP start currently supports LAN mode only";
    auto& plates = out.plater->get_partplate_list();
    out.plate_index = plates.get_curr_plate_index();
    out.plate = plates.get_curr_plate();
    auto& f = out.facts;
    f.modal = current_modal_state().anything_open();
    f.busy = !out.plater->get_ui_job_worker().is_idle() ||
        pipeline_busy(pipeline_state(*out.plater, plates.get_plate_count())) != PipelineBusy::idle ||
        out.plater->is_background_process_update_scheduled();
    if (f.modal || f.busy) return "Orca has an open modal dialog or a busy slicing/job worker";
    f.slice_valid = out.plate && !out.plate->is_apply_result_invalid() &&
        out.plate->is_slice_result_ready_for_print() && out.plate->is_valid_gcode_file() &&
        out.plate->has_printable_instances() && plate_gcode_checked(*out.plate);
    if (out.plate) f.slice_id = plate_native_result_identity(*out.plate, out.plate_index);
    f.printer_type = machine.printer_type;
    const auto& cfg = bundle->printers.get_edited_preset().config;
    f.profile_model = cfg.opt_string("printer_model");
    const auto* diameters = cfg.option<ConfigOptionFloats>("nozzle_diameter");
    f.profile_nozzle_mm = diameters && diameters->values.size() == 1 ? diameters->values.front() : 0;
    if (machine.GetNozzleSystem()) f.nozzle_mm = machine.GetNozzleSystem()->GetExtNozzle(0).GetNozzleDiameter();
    f.connected = machine.is_connected() && machine.m_is_online && machine.native_start_snapshot_ready &&
        machine.m_full_msg_count > 0;
    f.age_s = std::chrono::duration<double>(std::chrono::system_clock::now() - machine.last_push_time).count();
    f.idle = (machine.print_status == "IDLE" || machine.print_status == "FINISH") &&
        !machine.is_system_printing() && !machine.is_in_calibration() && !machine.is_in_upgrading() && machine.print_error == 0;
    f.storage_ready = machine.GetStorage() && machine.GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_NORMAL;
    f.ams_ready = machine.HasAms() && machine.ams_status_main == AmsStatusMain::AMS_STATUS_MAIN_IDLE;
    f.credentials_ready = machine.has_access_right() && !machine.get_access_code().empty() && !machine.get_dev_ip().empty();
    if (auto error = bambu_print_refusal(f, args.value("expected_slice_id", std::string()))) return error;
    out.project_filament_count = bundle->filament_presets.size();
    auto full = bundle->full_config();
    const auto* colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    for (int id : out.plate->get_used_filaments()) {
        if (id < 1 || size_t(id) > out.project_filament_count) return "Slice references a missing project filament";
        const auto* preset = bundle->filaments.find_preset(bundle->filament_presets[size_t(id - 1)]);
        if (!preset || bundle->is_mixed_filament(size_t(id - 1))) return "Missing or mixed project filament is unsupported";
        std::string display;
        out.filaments.push_back({id, full.get_filament_type(display, id - 1), preset->filament_id,
            colors && size_t(id) <= colors->values.size() ? color_rgba(colors->values[size_t(id - 1)]) : std::string()});
        const auto nozzle_maps = out.plate->get_filament_maps();
        if (size_t(id) <= nozzle_maps.size()) {
            const auto native = static_cast<FilamentMapNozzleId>(nozzle_maps[size_t(id - 1)]);
            if (native == FilamentMapNozzleId::NOZZLE_LEFT) out.filaments.back().nozzle_id = int(CloudTaskNozzleId::NOZZLE_LEFT);
            else if (native == FilamentMapNozzleId::NOZZLE_RIGHT) out.filaments.back().nozzle_id = int(CloudTaskNozzleId::NOZZLE_RIGHT);
            else return "Project filament has an unsupported native nozzle mapping";
        }
    }
    auto system = machine.GetFilaSystem();
    for (const auto& entry : args.at("mappings")) {
        BambuMapping m;
        m.project_filament = entry.at("project_filament").get<int>();
        m.ams_id = entry.at("ams_id").get<int>();
        m.slot_id = entry.at("slot_id").get<int>();
        if (m.ams_id >= 0 && m.ams_id <= 3 && m.slot_id >= 0 && m.slot_id <= 3 && system) {
            const auto* ams = system->GetAmsById(std::to_string(m.ams_id));
            auto* tray = ams ? ams->GetTray(std::to_string(m.slot_id)) : nullptr;
            int bit = m.ams_id * 4 + m.slot_id;
            if (tray && ams->IsExist()) {
                m.present = tray->is_exists && !tray->is_slot_placeholder && ((machine.tray_exist_bits >> bit) & 1);
                m.ready = tray->is_tray_info_ready();
                m.reading = ((machine.tray_reading_bits >> bit) & 1) != 0;
                m.type = tray->get_filament_type(); m.profile_id = tray->setting_id; m.color = color_rgba(tray->color);
            }
        }
        out.mappings.push_back(std::move(m));
    }
    return bambu_mapping_refusal(out.filaments, out.mappings);
}

json observed(MachineObject* machine) {
    if (!machine) return {{"state", "unknown"}};
    const auto age = std::chrono::duration<double>(std::chrono::system_clock::now() - machine->last_push_time).count();
    const bool fresh = machine->native_start_snapshot_ready && machine->is_connected() && machine->m_is_online && age >= 0 && age <= 15;
    return {{"state", fresh ? machine->print_status : "unknown"}, {"telemetry_age_s", age},
        {"fresh", fresh}, {"correlated_to_submission", false}};
}
json submission_json(const std::string& id, const Submission& record) {
    auto* manager = wxGetApp().getDeviceManager();
    auto* selected = manager ? manager->get_selected_machine() : nullptr;
    if (selected && selected->get_dev_id() != record.printer_id) selected = nullptr;
    return {{"request_id", id}, {"printer_id", record.printer_id}, {"submission_state", record.state},
        {"slice_id", record.identity}, {"observed_printer", observed(selected)},
        {"retry_safe", false}, {"idempotency_scope", "current Orca process only; never retry after restart without reconciling printer state"}};
}
json start(const json& args) {
    return run_on_main_thread([args]() -> json {
        const bool dry_run = args.value("dry_run", true);
        const auto id = args.value("request_id", std::string());
        if (!dry_run) {
            if (id.empty() || args.value("expected_slice_id", std::string()).empty())
                return error_response("Actual start requires request_id and expected_slice_id from a dry run");
            // Lookup before admission, so a retry returns the original outcome even if now busy.
            if (auto found = submissions.find(id); found != submissions.end())
                return submission_json(id, *found->second);
        }
        Inspection i;
        if (auto refusal = inspect(args, i)) return error_response(*refusal);
        const auto mappings = bambu_mapping_arrays(i.project_filament_count, i.filaments, i.mappings);
        json result{{"dry_run", dry_run}, {"printer_id", i.machine->get_dev_id()}, {"slice_id", i.facts.slice_id},
            {"plate_index", i.plate_index}, {"native_mapping", mappings}, {"telemetry_age_s", i.facts.age_s},
            {"nozzle_mm", i.facts.nozzle_mm}, {"observed_printer", observed(i.machine)}};
        std::shared_ptr<Submission> record;
        const bool dispatched = bambu_dispatch_once(dry_run, id, i.facts.slice_id, ledger, [&] {
            record = std::make_shared<Submission>();
            record->printer_id = i.machine->get_dev_id(); record->identity = i.facts.slice_id;
            submissions.emplace(id, record);
            bool enqueue_attempted = false;
            try {
                // Same packager and native factory as SelectMachineDialog. No transport is recreated here.
                if (i.plater->send_gcode(i.plate_index, [](int, int, int, bool&) {}) < 0) {
                    record->state = "failed_before_submission"; return;
                }
                // Packaging may pump events; re-read all admission facts immediately before queueing.
                Inspection current;
                if (auto refusal = inspect(args, current); refusal || current.facts.slice_id != i.facts.slice_id ||
                    bambu_mapping_arrays(current.project_filament_count, current.filaments, current.mappings) != mappings) {
                    record->state = "failed_before_submission"; return;
                }
                auto job = make_print_job_for_machine(*current.machine);
                job->m_print_type = "from_normal";
                job->set_project_name(i.plater->get_project_name().utf8_string());
                job->task_ams_mapping = mappings["ams_mapping"].dump();
                job->task_ams_mapping2 = mappings["ams_mapping2"].dump();
                job->task_ams_mapping_info = mappings["ams_mapping_info"].dump();
                job->task_nozzles_info = "[]"; // P1S has neither dual nozzles nor a hotend rack.
                job->task_use_ams = true;
                const bool leveling = args.value("bed_leveling", true), timelapse = args.value("timelapse", false);
                job->set_print_config("", leveling, false, false, timelapse, false, false, leveling ? 1 : 0, 0, 0, -1);
                job->notify_plater_on_completion(false);
                job->on_check_ip_address_fail([] {}); // mandatory native preflight callback, no modal behavior
                i.plater->get_print_job_data(&job->job_data);
                job->job_data.is_from_plater = false; // pin the prepared package, never re-read a changed project
                job->plate_data.is_valid = true;
                job->plate_data.plate_count = i.plater->get_partplate_list().get_plate_count();
                job->plate_data.cur_plate_index = i.plate_index + 1;
                job->plate_data.bed_type = i.plate->get_bed_type(true);
                auto wrapper = std::make_shared<SubmissionJob>(job, record);
                record->state = "queued";
                enqueue_attempted = true;
                if (!i.plater->get_ui_job_worker().push(wrapper)) record->state = "failed_before_submission";
            } catch (...) {
                // No exception text: transport errors can contain connection credentials.
                record->state = enqueue_attempted ? "uncertain" : "failed_before_submission";
            }
        });
        if (dry_run) { result["submission_state"] = "not_submitted"; return result; }
        if (!dispatched) return error_response("Request ID is consumed; no resend was attempted");
        return submission_json(id, *record);
    });
}
} // namespace

void register_bambu_print_tools() {
    using ToolCategory = OrcaMCPServer::ToolCategory;
    OrcaMCPServer::register_tool({"start_bambu_print", ToolCategory::Printers, "Inspect or submit a native Bambu print",
        "Dry-run by default: inspect the current selected sliced plate, explicit selected LAN P1S 0.4 printer and project-to-AMS mapping without exporting or sending anything. "
        "Actual submission requires dry_run=false, expected_slice_id from inspection and a unique request_id. Uses Orca PrintJob; never opens a dialog. "
        "AMS IDs/slots are zero-based; project filaments are one-based. Exact native material family/profile identity and fresh native idle telemetry are required. "
        "A repeated request_id never resends within this process. Submission success does not prove physical printing; poll get_bambu_print_submission. "
        "After restart all request IDs are forgotten: reconcile with the printer before any new call. Bed must be clear before actual submission.",
        {{"type", "object"}, {"properties", {
            {"printer_id", {{"type", "string"}, {"minLength", 1}}},
            {"dry_run", {{"type", "boolean"}, {"default", true}}},
            {"expected_slice_id", {{"type", "string"}, {"description", "Native slice identity returned by dry run; required for actual start"}}},
            {"request_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
            {"bed_leveling", {{"type", "boolean"}, {"default", true}}},
            {"timelapse", {{"type", "boolean"}, {"default", false}}},
            {"mappings", {{"type", "array"}, {"minItems", 1}, {"items", {{"type", "object"}, {"properties", {
                {"project_filament", {{"type", "integer"}, {"minimum", 1}}},
                {"ams_id", {{"type", "integer"}, {"minimum", 0}, {"maximum", 3}}},
                {"slot_id", {{"type", "integer"}, {"minimum", 0}, {"maximum", 3}}}}},
                {"required", {"project_filament", "ams_id", "slot_id"}}, {"additionalProperties", false}}}}}
        }}, {"required", {"printer_id", "mappings"}}, {"additionalProperties", false}}, start});
    OrcaMCPServer::register_tool({"get_bambu_print_submission", ToolCategory::Printers, "Observe native Bambu submission",
        "Read a request_id submitted in this Orca process. queued/submitted/uncertain describe submission, not physical printing. "
        "observed_printer is independent fresh native state, not correlated proof of this job. Never resends. Unknown IDs after restart require manual reconciliation.",
        {{"type", "object"}, {"properties", {{"request_id", {{"type", "string"}, {"minLength", 1}}}}},
            {"required", {"request_id"}}, {"additionalProperties", false}},
        [](const json& args) { return run_on_main_thread([args]() -> json {
            if (auto* plater = wxGetApp().plater()) plater->get_ui_job_worker().process_events();
            auto it = submissions.find(args.at("request_id").get<std::string>());
            if (it == submissions.end()) return error_response("Unknown request_id in this Orca process; no resend attempted");
            return submission_json(it->first, *it->second);
        }); }});
}
}}}
