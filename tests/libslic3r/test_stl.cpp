#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/STL.hpp"

#include <boost/filesystem.hpp>

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
