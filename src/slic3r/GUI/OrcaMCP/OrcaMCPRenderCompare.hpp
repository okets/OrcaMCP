#ifndef slic3r_OrcaMCPRenderCompare_hpp_
#define slic3r_OrcaMCPRenderCompare_hpp_

// render_plate_view's `compare_with`: a view drawn with the camera of an earlier picture of this session, set beside
// that picture and a mask of what changed. Pure: the records of the pictures written, the refusals, and the pixel
// difference -- no GL, no wx, no files (the picture is read by the caller) -- so it is unit-tested in
// tests/slic3rutils/test_render_compare.cpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.hpp"   // CameraFrame

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A picture render_plate_view wrote to a file: where, and the camera and size it was drawn with.
struct RenderRecord
{
    std::string  path;
    CameraFrame  frame;
    int          resolution  = 0;       // the picture is resolution x resolution
    int          plate_index = 0;
    bool         png         = true;
};

// The last k_capacity pictures this process wrote, newest last. Main thread only (render_plate_view's first and last
// steps); a picture remembered again under the same path replaces its record.
class RenderRecords
{
public:
    static constexpr size_t k_capacity = 64;

    void                remember(RenderRecord record);
    const RenderRecord* find(const std::string& path) const;
    size_t              size() const { return m_records.size(); }
    void                clear() { m_records.clear(); }

private:
    std::deque<RenderRecord> m_records;
};

RenderRecords& render_records();

// The record `path` names, for a view drawn on `plate_index` with `resolution_asked` (the call's, when it gave one).
// Throws std::runtime_error with what to send instead for: a path that is not one of this server's images, a picture
// not rendered in this session, a JPEG, another plate, another resolution.
const RenderRecord& compare_record(const RenderRecords& records, const std::string& path, int plate_index,
                                   std::optional<int> resolution_asked);

// True when both cameras have the same matrices and viewport (to `tolerance` in each entry).
bool same_camera(const CameraFrame& a, const CameraFrame& b, double tolerance = 1e-6);

// The largest picture compare_with reads: resolution is at most 2048, so 2048 x 2048 x 4 bytes (a PNG file is far
// smaller than its pixels, this caps what a planted file could make the server decode).
inline constexpr long long k_max_compare_file_bytes = 64ll * 1024 * 1024;

// Where two same-size RGB pictures differ: a pixel changed when its largest channel difference exceeds `threshold`.
struct ImageDifference
{
    size_t                  changed_pixels = 0;
    double                  changed_fraction = 0.;
    std::optional<std::array<int, 4>> changed_box;      // x0, y0, x1, y1 (x1, y1 inclusive); unset when nothing changed
    std::vector<uint8_t>    mask;                       // width * height, 1 where changed
};

// `before` and `now` are width * height * 3 bytes, row 0 first. Throws std::runtime_error when a size is not that.
ImageDifference image_difference(const std::vector<uint8_t>& before, const std::vector<uint8_t>& now, int width, int height,
                                 int threshold = 24);

// The difference picture: `now` in grey, the changed pixels red. width * height * 3 bytes.
std::vector<uint8_t> difference_picture(const std::vector<uint8_t>& now, const ImageDifference& difference, int width, int height);

// The answer's comparison object. `columns` are the picture's three columns, left to right.
nlohmann::json comparison_json(const std::string& before_path, const ImageDifference& difference, int resolution, int threshold);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
