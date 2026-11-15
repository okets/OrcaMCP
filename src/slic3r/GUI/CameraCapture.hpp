#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace Slic3r::GUI {

// Main-thread ownership. A user action or target switch invalidates the lease;
// its eventual cleanup may never stop the replacement playback.
class CameraCaptureLease
{
public:
    std::uint64_t begin(bool playing) {
        m_owned = !playing;
        m_active = true;
        m_generation = ++s_next;
        return m_generation;
    }
    void invalidate() { m_generation = ++s_next; m_active = false; m_owned = false; }
    bool valid(std::uint64_t generation) const { return m_active && generation == m_generation; }
    bool finish(std::uint64_t generation) {
        if (!valid(generation)) return false;
        const bool stop = m_owned;
        invalidate();
        return stop;
    }
    bool owned() const { return m_active && m_owned; }
private:
    inline static std::atomic<std::uint64_t> s_next{0};
    std::uint64_t m_generation{0};
    bool m_active{false};
    bool m_owned{false};
};

inline bool camera_frame_fresh(std::chrono::steady_clock::time_point decoded,
                               std::chrono::steady_clock::time_point requested,
                               std::chrono::steady_clock::time_point now)
{
    return decoded >= requested && decoded <= now && now - decoded <= std::chrono::seconds(2);
}

inline bool camera_capture_expired(std::chrono::steady_clock::time_point requested,
                                   std::chrono::steady_clock::time_point now,
                                   std::chrono::milliseconds limit = std::chrono::seconds(8))
{
    return now - requested >= limit;
}

} // namespace Slic3r::GUI
