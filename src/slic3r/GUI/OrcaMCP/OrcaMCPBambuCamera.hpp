#pragma once

#include <nlohmann/json.hpp>
#include "OrcaMCPMainThreadGate.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Slic3r::GUI::OrcaMCP {
void register_bambu_camera_tools();

struct CameraRequestState {
    std::string printer_id;
    std::chrono::steady_clock::time_point requested, deadline;
    std::atomic<bool> canceled{false};
    std::uint64_t lease{0}; // GUI thread only
};

inline bool camera_request_expired(const std::shared_ptr<CameraRequestState>& request)
{
    return request->canceled.load() || std::chrono::steady_clock::now() >= request->deadline;
}

inline nlohmann::json camera_deadline_failure()
{
    return {{"status", "error"}, {"code", "camera_timeout"},
        {"message", "Camera capture deadline expired (at most 8 seconds, including GUI queue waits)"}};
}

// Both closures own their captured state. Late work may finish after the HTTP
// caller returns, but cannot export its result or leave its lease unreleased.
inline std::optional<nlohmann::json> camera_phase_until(MainThreadGate& gate,
    std::shared_ptr<CameraRequestState> request, McpWork work, std::function<void()> release)
{
    const auto deadline = request->deadline;
    return call_owned_until(gate, [request, work = std::move(work), release = std::move(release)]() -> nlohmann::json {
        if (camera_request_expired(request)) { release(); return camera_deadline_failure(); }
        try {
            auto result = work();
            if (camera_request_expired(request)) { release(); return camera_deadline_failure(); }
            return result;
        } catch (...) {
            release();
            throw;
        }
    }, deadline);
}

// Cleanup shares the original absolute deadline. A late cleanup is explicitly
// pending and owns its state until the GUI can run it, or native shutdown wins.
inline nlohmann::json finish_camera_request(MainThreadGate& gate, std::shared_ptr<CameraRequestState> request,
    nlohmann::json result, std::function<void()> release, std::function<void(std::exception_ptr)> on_error)
{
    request->canceled.store(true);
    const auto cleaned = call_owned_until(gate, [release] {
        release();
        return nlohmann::json::object();
    }, request->deadline);
    if (!cleaned) {
        const bool shutdown = gate.is_closed();
        if (!shutdown) gate.post_owned(std::move(release), std::move(on_error));
        nlohmann::json refused = shutdown ? nlohmann::json{{"status", "error"}, {"code", "camera_unavailable"},
            {"message", "Native app is shutting down; native playback owns teardown"}} :
            result.value("code", std::string()) == "camera_stale" ? std::move(result) : camera_deadline_failure();
        refused["cleanup_pending"] = !shutdown;
        return refused;
    }
    if (std::chrono::steady_clock::now() >= request->deadline && result.value("code", std::string()) != "camera_stale")
        return camera_deadline_failure();
    return result;
}

// Camera metadata keeps the ordinary text envelope; only a successful camera
// capture may append an MCP image. No image is returned on a refusal.
inline nlohmann::json camera_frame_content(nlohmann::json result)
{
    nlohmann::json image;
    if (result.value("status", std::string()) == "success" && result.contains("image_base64")) {
        image = {{"type", "image"}, {"mimeType", "image/jpeg"}, {"data", result.at("image_base64")}};
    }
    result.erase("image_base64");
    nlohmann::json content = nlohmann::json::array({{{"type", "text"}, {"text", result.dump()}}});
    if (!image.is_null()) content.push_back(std::move(image));
    return {{"content", std::move(content)}};
}
}
