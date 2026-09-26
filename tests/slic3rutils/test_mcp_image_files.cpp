#include <catch2/catch_all.hpp>

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
