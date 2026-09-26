#include "OrcaMCPImageFiles.hpp"

#include <boost/filesystem.hpp>

#include <atomic>
#include <ctime>
#include <regex>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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
    const std::string name = std::string(prefix) + std::to_string(std::time(nullptr)) + "_" + std::to_string(mcp_process_id()) +
                             "_" + std::to_string(s_sequence.fetch_add(1)) + "_" + tag + extension;
    return (image_directory() / name).string();
}

long mcp_process_id()
{
#ifdef _WIN32
    return long(_getpid());
#else
    return long(getpid());
#endif
}

bool is_mcp_image_file_name(const std::string& file_name)
{
    // <prefix><time>_[<pid>_]<sequence>_<tag>.<png|jpg>: the pid is optional so older builds' images,
    // written without one, are still recognised and cleaned up.
    static const std::regex k_name("(orcamcp_render_|orcamcp_preview_)[0-9]+(_[0-9]+){1,2}_[A-Za-z0-9]+\\.(png|jpg)");
    return std::regex_match(file_name, k_name);
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

size_t remove_stale_mcp_images(const std::string& directory, std::chrono::seconds min_age)
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    if (!fs::is_directory(directory, ec))
        return 0;
    const std::time_t cutoff  = std::time(nullptr) - std::time_t(min_age.count());
    size_t            removed = 0;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        if (!fs::is_regular_file(path, ec) || !is_mcp_image_file_name(path.filename().string()))
            continue;
        const std::time_t written = fs::last_write_time(path, ec);
        if (!ec && written < cutoff && fs::remove(path, ec))
            ++removed;
    }
    return removed;
}

}}} // namespace Slic3r::GUI::OrcaMCP
