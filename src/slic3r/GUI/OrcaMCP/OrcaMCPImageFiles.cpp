#include "OrcaMCPImageFiles.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <atomic>
#include <ctime>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// Resolved, so a path handed back later compares equal to it whatever links the temp directory
// sits behind (/var is /private/var on macOS), and without the trailing separator $TMPDIR carries,
// so it equals the parent_path() of a file inside it.
boost::filesystem::path image_directory()
{
    boost::filesystem::path directory = boost::filesystem::weakly_canonical(boost::filesystem::temp_directory_path());
    directory.remove_trailing_separator();
    return directory;
}

} // namespace

std::string mcp_image_directory() { return image_directory().string(); }

std::string new_mcp_image_path(std::string_view prefix, const std::string& tag, const std::string& extension)
{
    static std::atomic<unsigned> s_sequence{0};
    const std::string name = std::string(prefix) + std::to_string(std::time(nullptr)) + "_" + std::to_string(s_sequence.fetch_add(1)) +
                             "_" + tag + extension;
    return (image_directory() / name).string();
}

bool is_mcp_image_file_name(const std::string& file_name)
{
    const bool ours  = boost::starts_with(file_name, k_render_image_prefix) || boost::starts_with(file_name, k_preview_image_prefix);
    const bool image = boost::ends_with(file_name, ".png") || boost::ends_with(file_name, ".jpg");
    return ours && image;
}

bool is_mcp_image_path(const std::string& path)
{
    namespace fs = boost::filesystem;
    const fs::path given(path);
    if (!given.is_absolute())
        return false;
    boost::system::error_code ec;
    const fs::path resolved = fs::weakly_canonical(given, ec);
    return !ec && is_mcp_image_file_name(resolved.filename().string()) && resolved.parent_path() == image_directory();
}

}}} // namespace Slic3r::GUI::OrcaMCP
