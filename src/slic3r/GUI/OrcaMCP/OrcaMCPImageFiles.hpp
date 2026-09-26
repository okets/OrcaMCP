#ifndef slic3r_OrcaMCPImageFiles_hpp_
#define slic3r_OrcaMCPImageFiles_hpp_

// Where render_plate_view and the turntable previews write their images, which of the files there
// are this server's, and which of them get_preview_base64 may read. Filesystem only: no GL, no wx.
// Unit-tested in tests/slic3rutils/test_mcp_image_files.cpp.

#include <string>
#include <string_view>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

inline constexpr std::string_view k_render_image_prefix  = "orcamcp_render_";
inline constexpr std::string_view k_preview_image_prefix = "orcamcp_preview_";

// The platform's temp directory, as boost::filesystem reports it, resolved. Images used to go to a
// literal "/tmp/", which is not a directory on Windows.
std::string mcp_image_directory();

// A new path in mcp_image_directory() for one image: "<prefix><time>_<sequence>_<tag><extension>".
// The per-process sequence number keeps images made within the same second from overwriting each
// other. `prefix` is one of the two above.
std::string new_mcp_image_path(std::string_view prefix, const std::string& tag, const std::string& extension);

// True for the file name of an image this server writes: either prefix, .png or .jpg.
bool is_mcp_image_file_name(const std::string& file_name);

// True when `path` is such an image directly inside mcp_image_directory(): the only files
// get_preview_base64 will read. A name check alone let "<anywhere>/orcamcp_render_/../<file>" through.
bool is_mcp_image_path(const std::string& path);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
