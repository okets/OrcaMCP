#include "OrcaMCPRenderCompare.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <boost/filesystem.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPImageFiles.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

std::string canonical_picture_path(const std::string& path)
{
    namespace fs = boost::filesystem;
    if (!fs::path(path).is_absolute())
        return path;
    boost::system::error_code ec;
    const fs::path            resolved = fs::weakly_canonical(fs::path(path), ec);
    return ec ? path : resolved.string();
}

void stamp_record_file(RenderRecord& record)
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    const auto                bytes = fs::file_size(record.path, ec);
    if (ec)
        return;
    const std::time_t modified = fs::last_write_time(record.path, ec);
    if (ec)
        return;
    record.file_bytes = bytes;
    record.modified   = modified;
}

std::string changed_file_refusal(const RenderRecord& record)
{
    if (record.file_bytes == 0 && record.modified == 0)
        return "";
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    const auto                bytes = fs::file_size(record.path, ec);
    if (ec)
        return "";   // gone: the caller says so
    const std::time_t modified = fs::last_write_time(record.path, ec);
    if (!ec && bytes == record.file_bytes && modified == record.modified)
        return "";
    return "compare_with: the file of that picture has changed since render_plate_view wrote it (size or time differ), so it is not the picture whose camera "
           "was remembered; render it again with save_to_file";
}

void RenderRecords::remember(RenderRecord record)
{
    const std::string key = canonical_picture_path(record.path);
    m_records.erase(std::remove_if(m_records.begin(), m_records.end(), [&key](const RenderRecord& r) { return canonical_picture_path(r.path) == key; }),
                    m_records.end());
    m_records.push_back(std::move(record));
    while (m_records.size() > k_capacity)
        m_records.pop_front();
}

const RenderRecord* RenderRecords::find(const std::string& path) const
{
    const std::string key = canonical_picture_path(path);
    for (const RenderRecord& record : m_records)
        if (canonical_picture_path(record.path) == key)
            return &record;
    return nullptr;
}

RenderRecords& render_records()
{
    static RenderRecords records;
    return records;
}

const RenderRecord& compare_record(const RenderRecords& records, const std::string& path, int plate_index, std::optional<int> resolution_asked)
{
    if (!is_mcp_image_path(path))
        throw std::runtime_error("compare_with reads only pictures render_plate_view wrote (the file_path of an earlier render); \"" + path +
                                 "\" is not one");
    const RenderRecord* record = records.find(path);
    if (record == nullptr)
        throw std::runtime_error("compare_with: that picture was not rendered in this session, so its camera is not known; render it again "
                                 "with save_to_file, then compare the new picture with it");
    if (record->layer_plan)
        throw std::runtime_error("compare_with compares 3D views: that file is a layer plan (layer_view, drawn top-down from the sliced G-code), which has no "
                                 "camera to draw again; render a 3D view with save_to_file instead");
    if (!record->png)
        throw std::runtime_error("compare_with compares PNG pictures (a JPEG's artefacts read as changes); render the earlier picture again "
                                 "with save_to_file and image_format png");
    if (record->plate_index != plate_index)
        throw std::runtime_error("compare_with: that picture is of plate " + std::to_string(record->plate_index) + ", not plate " +
                                 std::to_string(plate_index) + "; send plate_index " + std::to_string(record->plate_index));
    if (resolution_asked && *resolution_asked != record->resolution)
        throw std::runtime_error("resolution " + std::to_string(*resolution_asked) + " differs from the earlier picture's (" +
                                 std::to_string(record->resolution) + "); a compared view is drawn at its size, so leave resolution out");
    return *record;
}

std::vector<uint8_t> flatten_on_background(const std::vector<uint8_t>& rgb, const std::vector<uint8_t>& alpha, const std::array<uint8_t, 3>& background)
{
    if (alpha.empty())
        return rgb;
    const size_t pixels = rgb.size() / 3;
    if (rgb.size() != pixels * 3 || alpha.size() != pixels)
        throw std::runtime_error("the picture and its alpha are not the same size");
    std::vector<uint8_t> out(rgb.size());
    for (size_t i = 0; i < pixels; ++i)
        for (size_t c = 0; c < 3; ++c)
            out[i * 3 + c] = uint8_t((int(rgb[i * 3 + c]) * int(alpha[i]) + int(background[c]) * (255 - int(alpha[i])) + 127) / 255);
    return out;
}

bool same_camera(const CameraFrame& a, const CameraFrame& b, double tolerance)
{
    return a.viewport == b.viewport && (a.view - b.view).cwiseAbs().maxCoeff() <= tolerance &&
           (a.projection - b.projection).cwiseAbs().maxCoeff() <= tolerance;
}

ImageDifference image_difference(const std::vector<uint8_t>& before, const std::vector<uint8_t>& now, int width, int height, int threshold)
{
    if (width <= 0 || height <= 0)
        throw std::runtime_error("a picture needs a size");
    const size_t pixels = size_t(width) * size_t(height);
    if (before.size() != pixels * 3 || now.size() != pixels * 3)
        throw std::runtime_error("the two pictures are not the same size");
    ImageDifference out;
    out.mask.assign(pixels, 0);
    int x0 = width, y0 = height, x1 = -1, y1 = -1;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t i = size_t(y) * size_t(width) + size_t(x);
            int largest = 0;
            for (size_t c = 0; c < 3; ++c)
                largest = std::max(largest, std::abs(int(before[i * 3 + c]) - int(now[i * 3 + c])));
            if (largest <= threshold)
                continue;
            out.mask[i] = 1;
            ++out.changed_pixels;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    out.changed_fraction = double(out.changed_pixels) / double(pixels);
    if (out.changed_pixels > 0)
        out.changed_box = std::array<int, 4>{x0, y0, x1, y1};
    return out;
}

std::vector<uint8_t> difference_picture(const std::vector<uint8_t>& now, const ImageDifference& difference, int width, int height)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (now.size() != pixels * 3 || difference.mask.size() != pixels)
        throw std::runtime_error("the picture and its difference are not the same size");
    std::vector<uint8_t> out(now.size());
    for (size_t i = 0; i < pixels; ++i) {
        if (difference.mask[i]) {
            out[i * 3] = 230; out[i * 3 + 1] = 30; out[i * 3 + 2] = 30;
            continue;
        }
        // A pale grey of the picture, so the changed pixels stand out and the shapes stay readable.
        const int grey = (int(now[i * 3]) * 299 + int(now[i * 3 + 1]) * 587 + int(now[i * 3 + 2]) * 114) / 1000;
        const uint8_t pale = uint8_t(150 + grey * 90 / 255);
        out[i * 3] = out[i * 3 + 1] = out[i * 3 + 2] = pale;
    }
    return out;
}

nlohmann::json comparison_json(const std::string& before_path, const ImageDifference& difference, int resolution, int threshold)
{
    nlohmann::json json;
    json["before"]           = before_path;
    json["changed_pixels"]   = difference.changed_pixels;
    json["changed_fraction"] = difference.changed_fraction;
    json["changed_box"]      = difference.changed_box ? nlohmann::json(*difference.changed_box) : nlohmann::json(nullptr);
    json["threshold"]        = threshold;
    const int column         = compare_column_px(resolution);
    json["columns"]          = nlohmann::json::array({"before", "now", "difference"});
    json["column_px"]        = column;
    json["x_offsets"]        = nlohmann::json::array({0, column, 2 * column});
    if (column != resolution)
        json["scaled"] = {{"from_px", resolution}, {"to_px", column},
                          {"note", "each column is scaled down to " + std::to_string(column) + " px to keep the sheet readable; changed_box and the pixel counts are of the full " +
                                   std::to_string(resolution) + " px pictures, so scale a box by column_px / " + std::to_string(resolution) + " before reading it off the sheet"}};
    json["unchanged"]        = difference.changed_pixels == 0;
    json["note"]             = "changed pixels are those whose largest colour channel differs by more than the threshold (0 to 255); "
                               "overlays, labels and anti-aliasing at moved edges count, so ask for the same overlays in both pictures; both pictures are "
                               "flattened onto the render background first, so transparency does not count";
    return json;
}

}}} // namespace Slic3r::GUI::OrcaMCP
