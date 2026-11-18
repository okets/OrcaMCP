#include "OrcaMCPAtomicWrite.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace fs = boost::filesystem;

namespace {

// How many links in a row are followed: more is a loop, written through as it is.
constexpr int MAX_LINKS_FOLLOWED = 16;

// The file to replace: a symbolic link stays a link, so the write goes to the file it points to, through
// every link of a chain (a dotfiles manager's link to another link). A link written relative to its own
// folder resolves against that folder.
fs::path link_target(const fs::path& target)
{
    fs::path current = target;
    for (int followed = 0; followed < MAX_LINKS_FOLLOWED; ++followed) {
        boost::system::error_code ec;
        if (!fs::is_symlink(current, ec))
            return current;
        const fs::path pointed_at = fs::read_symlink(current, ec);
        if (ec)
            return current; // unreadable link: write through it and let the rename report what is wrong
        current = pointed_at.is_absolute() ? pointed_at : current.parent_path() / pointed_at;
    }
    return target;
}

} // namespace

std::string write_file_atomically(const fs::path& target, const std::string& content, WritePermissions permissions)
{
    const fs::path destination = link_target(target);
    const fs::path temporary   = destination.string() + ".tmp";

    // The permissions the new file takes: the old file's, when there is one to copy; the temporary's
    // own defaults when there is not.
    boost::system::error_code ec;
    fs::perms existing = fs::no_perms;
    if (permissions == WritePermissions::KeepExisting) {
        const fs::file_status old = fs::status(destination, ec);
        existing                  = ec ? fs::no_perms : old.permissions();
    }

    {
        boost::nowide::ofstream out(temporary.string(), std::ios::binary | std::ios::trunc);
        out << content;
        out.close(); // a full disk shows only once the buffer flushes
        if (!out) {
            boost::system::error_code ignored;
            fs::remove(temporary, ignored);
            return "cannot write " + temporary.string();
        }
    }

    if (permissions == WritePermissions::OwnerOnly)
        fs::permissions(temporary, fs::owner_read | fs::owner_write, ec);
    else if (existing != fs::no_perms)
        fs::permissions(temporary, existing, ec);

    fs::rename(temporary, destination, ec);
    if (ec) {
        boost::system::error_code ignored;
        fs::remove(temporary, ignored);
        return "cannot write " + destination.string() + ": " + ec.message();
    }
    return {};
}

}}} // namespace Slic3r::GUI::OrcaMCP
