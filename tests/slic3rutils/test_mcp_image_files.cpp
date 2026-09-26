#include <catch2/catch_all.hpp>

#include <chrono>
#include <ctime>
#include <fstream>

#include <boost/filesystem.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPImageFiles.hpp"

using namespace Slic3r::GUI::OrcaMCP;

// Where the images go. They used to be written to a literal /tmp/, which is not a directory on
// Windows, and the render files were never cleaned up.

TEST_CASE("images are written to the temp directory under names that never repeat", "[orcamcp][ImageFiles]")
{
    namespace fs = boost::filesystem;
    const std::string first  = new_mcp_image_path(k_render_image_prefix, "0", ".png");
    const std::string second = new_mcp_image_path(k_render_image_prefix, "0", ".png");
    CHECK(first != second);  // two renders in the same second used to overwrite each other
    CHECK(fs::path(first).parent_path() == fs::path(mcp_image_directory()));
    CHECK(fs::equivalent(fs::path(mcp_image_directory()), fs::temp_directory_path()));
    CHECK(is_mcp_image_file_name(fs::path(first).filename().string()));
}

TEST_CASE("only this server's image names count as its images", "[orcamcp][ImageFiles]")
{
    CHECK(is_mcp_image_file_name("orcamcp_render_1789763152_1_0.png"));
    CHECK(is_mcp_image_file_name("orcamcp_preview_1789763152_0_grid.jpg"));
    CHECK_FALSE(is_mcp_image_file_name("orcamcp_render_1789763152_1_0.txt"));
    CHECK_FALSE(is_mcp_image_file_name("my_orcamcp_render_1.png"));
    CHECK_FALSE(is_mcp_image_file_name("id_rsa"));
}

TEST_CASE("an image path is readable only directly inside the temp directory", "[orcamcp][ImageFiles]")
{
    namespace fs = boost::filesystem;
    const std::string image = new_mcp_image_path(k_preview_image_prefix, "test", ".jpg");
    std::ofstream(image) << "not really a jpeg";
    CHECK(is_mcp_image_path(image));

    const fs::path dir(mcp_image_directory());
    CHECK_FALSE(is_mcp_image_path((dir / "orcamcp_render_" / ".." / "secret.png").string()));  // a name check passed this
    CHECK_FALSE(is_mcp_image_path((dir / "sub" / "orcamcp_render_1_0_0.png").string()));
    CHECK_FALSE(is_mcp_image_path((dir / "notes.png").string()));
    CHECK_FALSE(is_mcp_image_path("orcamcp_render_1_0_0.png"));  // relative: not inside it
    fs::remove(image);
}

TEST_CASE("startup cleanup removes only this server's images older than the safe age", "[orcamcp][ImageFiles]")
{
    // Another OrcaMCP running at the same time writes into the same directory; a render it made a
    // minute ago must survive this one starting up.
    namespace fs = boost::filesystem;
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("orcamcp-cleanup-%%%%-%%%%");
    fs::create_directories(dir);
    auto make = [&dir](const char* name, std::time_t age_s) {
        const fs::path p = dir / name;
        std::ofstream(p.string()) << "x";
        fs::last_write_time(p, std::time(nullptr) - age_s);
        return p;
    };
    const fs::path old_render   = make("orcamcp_render_1_0_0.png", 2 * 3600);
    const fs::path old_preview  = make("orcamcp_preview_1_0_turntable.jpg", 2 * 3600);
    const fs::path fresh_render = make("orcamcp_render_2_1_0.png", 60);
    const fs::path old_other    = make("notes.png", 2 * 3600);

    CHECK(remove_stale_mcp_images(dir.string(), std::chrono::hours(1)) == 2);
    CHECK_FALSE(fs::exists(old_render));
    CHECK_FALSE(fs::exists(old_preview));
    CHECK(fs::exists(fresh_render));
    CHECK(fs::exists(old_other));
    fs::remove_all(dir);
}

TEST_CASE("startup cleanup of a missing directory removes nothing", "[orcamcp][ImageFiles]")
{
    CHECK(remove_stale_mcp_images("/nonexistent/orcamcp-cleanup", std::chrono::hours(1)) == 0);
}
