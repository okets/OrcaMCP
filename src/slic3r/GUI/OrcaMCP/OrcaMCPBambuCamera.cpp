#include "OrcaMCPBambuCamera.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPServer.hpp"
#include "OrcaMCPUiJob.hpp"
#include "slic3r/GUI/CameraCapture.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MediaPlayCtrl.h"
#include "slic3r/GUI/Monitor.hpp"
#include "slic3r/GUI/StatusPanel.hpp"
#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <boost/log/trivial.hpp>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <wx/base64.h>
#include <wx/image.h>
#include <wx/mstream.h>

namespace Slic3r::GUI::OrcaMCP {
namespace {
using json = nlohmann::json;

json failure(const std::string& code, const std::string& message)
{
    return {{"status", "error"}, {"code", code}, {"message", message}};
}

MediaPlayCtrl* player_if_built()
{
    auto* monitor = MonitorPanel::if_built();
    auto* status = monitor ? monitor->get_status_panel() : nullptr;
    return status ? status->get_media_play_ctrl() : nullptr;
}

bool selected_printer_matches(const std::string& id)
{
    auto* manager = wxGetApp().getDeviceManager();
    auto* machine = manager ? manager->get_selected_machine() : nullptr;
    return machine && machine->get_dev_id() == id && machine->is_lan_mode_printer() &&
        machine->is_connected() && machine->m_is_online;
}

void release(const std::shared_ptr<CameraRequestState>& request)
{
    if (request->lease) {
        if (auto* player = player_if_built()) player->EndFrameCapture(request->lease);
        request->lease = 0;
    }
}

std::optional<json> phase(const std::shared_ptr<CameraRequestState>& request, McpWork work)
{
    return camera_phase_until(main_thread_gate(), request, std::move(work), [request] { release(request); });
}

json finish(const std::shared_ptr<CameraRequestState>& request, json result)
{
    return finish_camera_request(main_thread_gate(), request, std::move(result), [request] { release(request); },
        [](std::exception_ptr error) {
            try { std::rethrow_exception(error); }
            catch (const std::exception&) { BOOST_LOG_TRIVIAL(error) << "OrcaMCP camera lease cleanup failed"; }
            catch (...) { BOOST_LOG_TRIVIAL(error) << "OrcaMCP camera lease cleanup failed with an unknown exception"; }
        });
}

json capture(const std::string& printer_id)
{
    using namespace std::chrono_literals;
    auto request = std::make_shared<CameraRequestState>();
    request->printer_id = printer_id;
    request->requested = std::chrono::steady_clock::now();
    const auto wait = std::min(tool_wait_cap(), std::chrono::milliseconds(8000));
    request->deadline = request->requested + wait;
    if (wait.count() <= 0) return camera_deadline_failure();
    try {
        const auto initial = phase(request, [request]() -> json {
            if (!selected_printer_matches(request->printer_id))
                return failure("camera_unavailable", "printer_id must match the connected selected native LAN printer");
            auto* monitor = MonitorPanel::ensure();
            auto* status = monitor ? monitor->get_status_panel() : nullptr;
            auto* player = status ? status->get_media_play_ctrl() : nullptr;
            if (!player) return failure("camera_unavailable", "Native camera player is unavailable");
            // Construction may pump the event loop or take longer than the
            // deadline. Never start a late session or use a changed selection.
            if (camera_request_expired(request)) return camera_deadline_failure();
            if (!selected_printer_matches(request->printer_id))
                return failure("camera_identity_changed", "Native printer changed while preparing the camera player");
            if (player->FramePrinterId().empty())
                player->SetMachineObject(wxGetApp().getDeviceManager()->get_selected_machine());
            request->lease = player->BeginFrameCapture(request->printer_id);
            if (!request->lease) return failure("camera_unavailable", "Native LAN camera cannot start or is backing off");
            return {{"ready", true}};
        });
        if (!initial || !initial->value("ready", false))
            return finish(request, initial ? *initial : camera_deadline_failure());
        json result;
        bool saw_old_frame = false;
        while (!camera_request_expired(request)) {
            const auto polled = phase(request, [request]() -> json {
                auto* player = player_if_built();
                if (!selected_printer_matches(request->printer_id) || !player || !player->FrameCaptureValid(request->lease, request->printer_id))
                    return failure("camera_identity_changed", "Native printer or camera playback ownership changed");
                wxImage frame;
                std::chrono::steady_clock::time_point decoded;
                std::chrono::system_clock::time_point received;
                if (!player->CaptureFrame(request->lease, request->printer_id, frame, decoded, received)) return {{"pending", true}};
                const auto now = std::chrono::steady_clock::now();
                if (!camera_frame_fresh(decoded, request->requested, now)) return {{"pending", true}, {"stale", true}};
                const int side = std::max(frame.GetWidth(), frame.GetHeight());
                if (side > 1280)
                    frame.Rescale(std::max(1, frame.GetWidth() * 1280 / side), std::max(1, frame.GetHeight() * 1280 / side));
                wxMemoryOutputStream stream;
                if (!frame.SaveFile(stream, wxBITMAP_TYPE_JPEG) || stream.GetSize() > 2 * 1024 * 1024)
                    return failure("camera_unavailable", "Camera JPEG encoding failed or exceeded 2 MiB");
                std::vector<unsigned char> bytes(stream.GetSize());
                stream.CopyTo(bytes.data(), bytes.size());
                const auto completed = std::chrono::steady_clock::now();
                if (!camera_frame_fresh(decoded, request->requested, completed)) return {{"pending", true}, {"stale", true}};
                return {{"status", "success"}, {"printer_id", request->printer_id},
                    {"received_at_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(received.time_since_epoch()).count()},
                    {"age_ms", std::chrono::duration_cast<std::chrono::milliseconds>(completed - decoded).count()},
                    {"timestamp_basis", "native_decode_receive_time_not_camera_exposure"},
                    {"width", frame.GetWidth()}, {"height", frame.GetHeight()},
                    {"image_base64", wxBase64Encode(bytes.data(), bytes.size()).ToStdString()}};
            });
            if (!polled) return finish(request, camera_deadline_failure());
            result = *polled;
            if (!result.value("pending", false)) return finish(request, std::move(result));
            saw_old_frame = saw_old_frame || result.value("stale", false);
            std::this_thread::sleep_until(std::min(request->deadline, std::chrono::steady_clock::now() + 50ms));
        }
        return finish(request, failure(saw_old_frame ? "camera_stale" : "camera_timeout", "No newly decoded fresh frame arrived within the capture deadline"));
    } catch (...) {
        finish(request, failure("camera_unavailable", "Camera capture failed"));
        throw;
    }
}
} // namespace

void register_bambu_camera_tools()
{
    OrcaMCPServer::register_tool({"get_bambu_camera_frame", OrcaMCPServer::ToolCategory::Printers, "Capture a native Bambu camera frame",
        "Capture one fresh frame from the selected connected native LAN Bambu printer. printer_id must match the native selection. "
        "Borrows owner playback or briefly starts idle playback, waits at most 8 seconds, and stops only capture-owned playback. "
        "Returns JPEG image content plus printer identity, native receive/decode time (not camera exposure time), dimensions and age. "
        "Unavailable, changed identity, stale and timeout outcomes return no image. Never starts a print or returns camera URLs/credentials.",
        {{"type", "object"}, {"properties", {{"printer_id", {{"type", "string"}, {"minLength", 1}}}}}, {"required", {"printer_id"}}},
        [](const json& args) { return capture(args.at("printer_id").get<std::string>()); }});
}
} // namespace Slic3r::GUI::OrcaMCP
