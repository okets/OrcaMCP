#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/STL.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>

using namespace Slic3r;

static inline std::string stl_path(const char* path)
{
	return std::string(TEST_DATA_DIR) + "/test_stl/" + path;
}

SCENARIO("Reading an STL file", "[stl]") {
	GIVEN("umlauts in the path of a binary STL file, Czech characters in the file name") {
        WHEN("STL file is read") {
			Slic3r::Model model;
			THEN("load should succeed") {
                REQUIRE(Slic3r::load_stl(stl_path("Geräte/20mmbox-čřšřěá.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
            }
        }
    }
	GIVEN("in ASCII format") {
		WHEN("line endings LF") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-LF.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
		WHEN("line endings CRLF") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-CRLF.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
#if 0
		// ASCII STLs ending with just carriage returns are not supported. These were used by the old Macs, while the Unix based MacOS uses LFs as any other Unix.
		WHEN("line endings CR") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-CR.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}

#endif
		WHEN("nonstandard STL file (text after ending tags, invalid normals, for example infinities)") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-nonstandard.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
	}
}

TEST_CASE("Storing an STL says whether the file was written", "[stl]")
{
    TriangleMesh mesh = make_cube(10., 10., 10.);
    const std::string missing_folder = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path() / "cube.stl").string();
    CHECK_FALSE(Slic3r::store_stl(missing_folder.c_str(), &mesh, /*binary=*/true));
    CHECK_FALSE(Slic3r::store_stl(missing_folder.c_str(), &mesh, /*binary=*/false));

    const boost::filesystem::path written = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("%%%%-cube.stl");
    CHECK(Slic3r::store_stl(written.string().c_str(), &mesh, /*binary=*/true));
    Slic3r::Model model;
    CHECK(Slic3r::load_stl(written.string().c_str(), &model));
    boost::filesystem::remove(written);
}

namespace {

// A binary STL of an axis-aligned cube of `size` mm at the origin, written byte by byte as some generators write it:
// every normal zero and `header` as its 80-byte label. At whole millimetres every byte after the header is below 128.
std::string binary_cube_stl(float size, const std::string& header = "binary cube")
{
    const TriangleMesh      cube = make_cube(size, size, size);
    const indexed_triangle_set& its = cube.its;
    std::string             bytes(80, ' ');
    bytes.replace(0, std::min<size_t>(header.size(), 80), header.substr(0, 80));
    const auto put = [&bytes](const void* data, size_t n) { bytes.append(static_cast<const char*>(data), n); };
    const uint32_t count = uint32_t(its.indices.size());
    put(&count, 4);
    for (const stl_triangle_vertex_indices& face : its.indices) {
        const float normal[3] = {0.f, 0.f, 0.f};
        put(normal, sizeof(normal));
        for (int corner = 0; corner < 3; ++corner) {
            const Vec3f& v = its.vertices[face[corner]];
            const float  xyz[3] = {v.x(), v.y(), v.z()};
            put(xyz, sizeof(xyz));
        }
        const uint16_t attributes = 0;
        put(&attributes, 2);
    }
    return bytes;
}

bool all_below_128(const std::string& bytes)
{
    return std::all_of(bytes.begin() + 80, bytes.end(), [](char c) { return static_cast<unsigned char>(c) < 128; });
}

size_t facets_loaded(const std::string& bytes, const char* name)
{
    const boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path(std::string("%%%%-") + name);
    {
        std::ofstream out(path.string(), std::ios::binary);
        out.write(bytes.data(), std::streamsize(bytes.size()));
    }
    Slic3r::Model model;
    const bool    loaded = Slic3r::load_stl(path.string().c_str(), &model);
    boost::filesystem::remove(path);
    return loaded && !model.objects.empty() ? model.objects.front()->volumes.front()->mesh().facets_count() : 0;
}

} // namespace

// admesh took a file for ASCII when the first 128 bytes after its header were all below 128, and a small binary STL
// at whole millimetres with zero normals has no byte above: it was read as text and loaded no geometry. A binary STL
// is exactly its 80-byte label, its facet count and 50 bytes per facet, which decides first.
TEST_CASE("A binary STL whose bytes are all below 128 loads as binary", "[stl]")
{
    for (const float size : {10.f, 15.f}) {
        const std::string bytes = binary_cube_stl(size);
        REQUIRE(all_below_128(bytes));
        CHECK(facets_loaded(bytes, "cube.stl") == 12);
    }
}

// A binary STL whose label starts with "solid", as some exporters write it, still loads as binary, and ASCII STLs as ASCII.
TEST_CASE("A binary STL labelled solid, and ASCII STLs, load as what they are", "[stl]")
{
    CHECK(facets_loaded(binary_cube_stl(10.f, "solid exported by a binary writer"), "solid-binary.stl") == 12);
    const std::string ascii = "solid cube\n"
                              " facet normal 0 0 -1\n  outer loop\n   vertex 0 0 0\n   vertex 10 10 0\n   vertex 10 0 0\n  endloop\n endfacet\n"
                              " facet normal 0 0 -1\n  outer loop\n   vertex 0 0 0\n   vertex 0 10 0\n   vertex 10 10 0\n  endloop\n endfacet\n"
                              " facet normal 0 -1 0\n  outer loop\n   vertex 0 0 0\n   vertex 10 0 0\n   vertex 10 0 10\n  endloop\n endfacet\n"
                              " facet normal 0 -1 0\n  outer loop\n   vertex 0 0 0\n   vertex 10 0 10\n   vertex 0 0 10\n  endloop\n endfacet\n"
                              "endsolid cube\n";
    CHECK(facets_loaded(ascii, "ascii.stl") == 4);
    Slic3r::Model model;
    REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-LF.stl").c_str(), &model));
    CHECK(model.objects.front()->volumes.front()->mesh().facets_count() == 12);
}
