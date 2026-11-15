#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "libslic3r/GCode/GCodeProcessor.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {
// Bounded snapshot metadata only; never reads moves, live Print config, result.z_offset, or files.
nlohmann::json coordinate_frame_json(const MoveFrameProvenance& frame, const std::string& result_id);
}}}
