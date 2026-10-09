#include "FlashforgeJobProgress.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"

namespace Slic3r { namespace FlashforgeJobProgress {

using nlohmann::json;

const char* const kExtendedInfoKey = "slicedProgress";

bool SliceTable::valid() const
{
    if (file_bytes == 0 || layer_start_bytes.empty() || layer_start_bytes.size() != layer_start_s.size() || !(total_s > 0))
        return false;
    for (size_t i = 0; i < layer_start_bytes.size(); ++i) {
        if (layer_start_bytes[i] >= file_bytes || !(layer_start_s[i] >= 0) || layer_start_s[i] > total_s)
            return false;
        if (i > 0 && (layer_start_bytes[i] <= layer_start_bytes[i - 1] || layer_start_s[i] < layer_start_s[i - 1]))
            return false;
    }
    return true;
}

SliceTable slice_table(const GCodeProcessorResult& result)
{
    constexpr size_t normal = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);

    SliceTable table;
    if (result.lines_ends.empty())
        return table;
    table.file_bytes = result.lines_ends.back();

    // A move's line starts where the line before it ends; gcode_id is 1-based, 0 for no line.
    const auto line_start = [&](unsigned int gcode_id) -> std::optional<std::uint64_t> {
        if (gcode_id == 0 || gcode_id > result.lines_ends.size())
            return std::nullopt;
        return gcode_id == 1 ? 0 : static_cast<std::uint64_t>(result.lines_ends[gcode_id - 2]);
    };

    double elapsed    = 0;
    long   last_layer = -1;
    for (const auto& move : result.moves) {
        if (static_cast<long>(move.layer_id) > last_layer) {
            if (const auto start = line_start(move.gcode_id);
                start && (table.layer_start_bytes.empty() || *start > table.layer_start_bytes.back())) {
                table.layer_start_bytes.push_back(*start);
                table.layer_start_s.push_back(elapsed);
                last_layer = static_cast<long>(move.layer_id);
            }
        }
        elapsed += move.time[normal];
    }
    table.total_s = elapsed;
    return table.valid() ? table : SliceTable{};
}

std::string to_json(const SliceTable& table)
{
    json layers = json::array();
    for (size_t i = 0; i < table.layer_start_bytes.size(); ++i)
        layers.push_back({table.layer_start_bytes[i], std::round(table.layer_start_s[i] * 10) / 10});
    return json{{"bytes", table.file_bytes}, {"total_s", table.total_s}, {"layers", std::move(layers)}}.dump();
}

std::optional<SliceTable> from_json(const std::string& text)
{
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("bytes") || !j["bytes"].is_number_unsigned() || !j.contains("total_s") ||
        !j["total_s"].is_number() || !j.contains("layers") || !j["layers"].is_array())
        return std::nullopt;

    SliceTable table;
    table.file_bytes = j["bytes"].get<std::uint64_t>();
    table.total_s    = j["total_s"].get<double>();
    for (const auto& layer : j["layers"]) {
        if (!layer.is_array() || layer.size() != 2 || !layer[0].is_number_unsigned() || !layer[1].is_number())
            return std::nullopt;
        table.layer_start_bytes.push_back(layer[0].get<std::uint64_t>());
        table.layer_start_s.push_back(layer[1].get<double>());
    }
    if (!table.valid())
        return std::nullopt;
    return table;
}

std::optional<SliceTable> from_extended_info(const std::map<std::string, std::string>& extended_info)
{
    const auto it = extended_info.find(kExtendedInfoKey);
    return it == extended_info.end() ? std::nullopt : from_json(it->second);
}

std::optional<SliceTable> for_upload(const std::map<std::string, std::string>& extended_info,
                                     bool                                      gcode_in_3mf,
                                     std::uint64_t                             uploaded_bytes)
{
    auto table = from_extended_info(extended_info);
    if (table && !gcode_in_3mf && uploaded_bytes != table->file_bytes)
        return std::nullopt;
    return table;
}

Reading read(const SliceTable& table, double byte_progress)
{
    Reading reading;
    if (!table.valid())
        return reading;
    const size_t n     = table.layer_start_bytes.size();
    reading.layers     = static_cast<int>(n);
    const double bytes = std::clamp(byte_progress, 0.0, 1.0) * static_cast<double>(table.file_bytes);

    // The layer whose start is the last one at or before `bytes`. Before the first, the start G-code:
    // the first layer's, its time spread over its bytes from 0.
    const auto after = std::upper_bound(table.layer_start_bytes.begin(), table.layer_start_bytes.end(), bytes,
                                        [](double b, std::uint64_t start) { return b < static_cast<double>(start); });
    if (after == table.layer_start_bytes.begin()) {
        reading.layer         = 1;
        reading.time_fraction = table.layer_start_s[0] * bytes / static_cast<double>(table.layer_start_bytes[0]) / table.total_s;
        return reading;
    }
    const size_t i = static_cast<size_t>(after - table.layer_start_bytes.begin()) - 1;
    reading.layer  = static_cast<int>(i) + 1;

    // Within the layer, time is spread over its bytes.
    const double from_b = static_cast<double>(table.layer_start_bytes[i]);
    const double to_b   = i + 1 < n ? static_cast<double>(table.layer_start_bytes[i + 1]) : static_cast<double>(table.file_bytes);
    const double to_s   = i + 1 < n ? table.layer_start_s[i + 1] : table.total_s;
    const double within = to_b > from_b ? std::clamp((bytes - from_b) / (to_b - from_b), 0.0, 1.0) : 0.0;
    const double done_s = table.layer_start_s[i] + within * (to_s - table.layer_start_s[i]);
    reading.time_fraction = std::clamp(done_s / table.total_s, 0.0, 1.0);
    return reading;
}

void apply(FlashforgeApi::PrinterStatus& status, const SliceTable& table)
{
    const bool active = status.state == "printing" || status.state == "paused";
    if (!active || !status.implausible_telemetry.empty() || !table.valid())
        return;
    const Reading reading  = read(table, status.progress);
    status.layer           = reading.layer;
    status.layers          = reading.layers;
    status.work_done       = reading.time_fraction;
    status.remaining_s     = FlashforgeApi::project_remaining_s(status.state, status.duration_s, reading.time_fraction);
    status.progress_source = "slice";
}

}} // namespace Slic3r::FlashforgeJobProgress
