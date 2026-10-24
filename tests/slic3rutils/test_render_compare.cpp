#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "slic3r/GUI/OrcaMCP/OrcaMCPImageFiles.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderCompare.hpp"

// compare_with's pure parts (OrcaMCPRenderCompare.cpp): the records of the pictures written, the refusals, and the
// pixel difference.

using namespace Slic3r::GUI::OrcaMCP;

namespace {

RenderRecord record_at(const std::string& path, int resolution = 64, int plate = 0, bool png = true)
{
    RenderRecord record;
    record.path        = path;
    record.resolution  = resolution;
    record.plate_index = plate;
    record.png         = png;
    record.frame.viewport = {0, 0, resolution, resolution};
    return record;
}

std::vector<uint8_t> grey(int width, int height, uint8_t value)
{
    return std::vector<uint8_t>(size_t(width) * size_t(height) * 3, value);
}

std::string message_of(const RenderRecords& records, const std::string& path, int plate, std::optional<int> resolution)
{
    try {
        compare_record(records, path, plate, resolution);
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

} // namespace

TEST_CASE("Identical pictures differ nowhere", "[RenderCompare][orcamcp]")
{
    const std::vector<uint8_t> a = grey(8, 6, 100);
    const ImageDifference d      = image_difference(a, a, 8, 6);
    CHECK(d.changed_pixels == 0);
    CHECK_THAT(d.changed_fraction, Catch::Matchers::WithinAbs(0., 1e-12));
    CHECK_FALSE(d.changed_box.has_value());
    CHECK(comparison_json("x", d, 8, 24)["unchanged"].get<bool>());
}

TEST_CASE("A changed block gives its box and fraction", "[RenderCompare][orcamcp]")
{
    const int w = 10, h = 8;
    std::vector<uint8_t> before = grey(w, h, 100), now = before;
    for (int y = 2; y < 4; ++y)          // a 3 x 2 block at x 4..6, y 2..3
        for (int x = 4; x < 7; ++x)
            for (int c = 0; c < 3; ++c)
                now[(size_t(y) * w + x) * 3 + c] = 200;
    const ImageDifference d = image_difference(before, now, w, h);
    CHECK(d.changed_pixels == 6);
    CHECK_THAT(d.changed_fraction, Catch::Matchers::WithinAbs(6. / 80., 1e-12));
    REQUIRE(d.changed_box.has_value());
    CHECK((*d.changed_box)[0] == 4);
    CHECK((*d.changed_box)[1] == 2);
    CHECK((*d.changed_box)[2] == 6);
    CHECK((*d.changed_box)[3] == 3);
    CHECK(d.mask[2 * w + 4] == 1);
    CHECK(d.mask[2 * w + 3] == 0);
}

TEST_CASE("A difference at or below the threshold is no change", "[RenderCompare][orcamcp]")
{
    std::vector<uint8_t> before = grey(4, 4, 100), now = before;
    now[0] = 124;   // exactly the threshold
    now[3] = 100; now[4] = 125;   // one above it on a second pixel's channel
    const ImageDifference d = image_difference(before, now, 4, 4, 24);
    CHECK(d.changed_pixels == 1);
    REQUIRE(d.changed_box.has_value());
    CHECK((*d.changed_box)[0] == 1);
    CHECK((*d.changed_box)[1] == 0);
}

TEST_CASE("Pictures of different sizes cannot be compared", "[RenderCompare][orcamcp]")
{
    CHECK_THROWS(image_difference(grey(4, 4, 0), grey(4, 5, 0), 4, 4));
    CHECK_THROWS(image_difference(grey(4, 4, 0), grey(4, 4, 0), 0, 4));
}

TEST_CASE("The difference picture marks changed pixels red on a pale grey of the picture", "[RenderCompare][orcamcp]")
{
    std::vector<uint8_t> before = grey(2, 1, 100), now = before;
    now[0] = now[1] = now[2] = 220;
    const ImageDifference d = image_difference(before, now, 2, 1);
    const std::vector<uint8_t> picture = difference_picture(now, d, 2, 1);
    REQUIRE(picture.size() == 6);
    CHECK(picture[0] > picture[1]);                // red
    CHECK(picture[1] == picture[2]);
    CHECK(picture[3] == picture[4]);               // grey
    CHECK(picture[3] == picture[5]);
    CHECK(picture[3] >= 150);
    CHECK_THROWS(difference_picture(now, d, 3, 1));
}

TEST_CASE("The records keep the last 64 pictures and find them by path", "[RenderCompare][orcamcp]")
{
    RenderRecords records;
    for (int i = 0; i < 70; ++i)
        records.remember(record_at("p" + std::to_string(i)));
    CHECK(records.size() == RenderRecords::k_capacity);
    CHECK(records.find("p0") == nullptr);
    CHECK(records.find("p5") == nullptr);
    REQUIRE(records.find("p6") != nullptr);
    CHECK(records.find("p69")->path == "p69");
    // the same path again replaces its record
    records.remember(record_at("p69", 128));
    CHECK(records.size() == RenderRecords::k_capacity);
    CHECK(records.find("p69")->resolution == 128);
}

TEST_CASE("compare_record refuses what cannot be compared, with what to send", "[RenderCompare][orcamcp]")
{
    RenderRecords records;
    const std::string png  = new_mcp_image_path(k_render_image_prefix, "0", ".png");
    const std::string jpg  = new_mcp_image_path(k_render_image_prefix, "0", ".jpg");
    const std::string lost = new_mcp_image_path(k_render_image_prefix, "0", ".png");
    records.remember(record_at(png, 64, 0));
    records.remember(record_at(jpg, 64, 0, false));

    CHECK(message_of(records, png, 0, std::nullopt).empty());
    CHECK(message_of(records, png, 0, 64).empty());
    CHECK(compare_record(records, png, 0, std::nullopt).resolution == 64);

    CHECK(message_of(records, "/etc/passwd", 0, std::nullopt).find("only pictures render_plate_view wrote") != std::string::npos);
    CHECK(message_of(records, "relative.png", 0, std::nullopt).find("only pictures render_plate_view wrote") != std::string::npos);
    CHECK(message_of(records, lost, 0, std::nullopt).find("not rendered in this session") != std::string::npos);
    CHECK(message_of(records, jpg, 0, std::nullopt).find("PNG") != std::string::npos);
    CHECK(message_of(records, png, 1, std::nullopt).find("plate 0") != std::string::npos);
    CHECK(message_of(records, png, 0, 128).find("leave resolution out") != std::string::npos);
}

TEST_CASE("Two cameras are the same only when every matrix entry and the viewport agree", "[RenderCompare][orcamcp]")
{
    CameraFrame a, b;
    a.viewport = b.viewport = {0, 0, 64, 64};
    CHECK(same_camera(a, b));
    b.view(0, 3) += 1e-3;
    CHECK_FALSE(same_camera(a, b));
    CHECK(same_camera(a, b, 1e-2));
    b = a;
    b.viewport = {0, 0, 128, 128};
    CHECK_FALSE(same_camera(a, b));
}
