#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderLight.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPRenderRequest.hpp"

// The mcp_render shader (resources/shaders/{110,140}) is compiled only by the running app, so this is the one guard
// before it: the files exist in both dialects, declare every uniform the render sets, and keep the thumbnail
// shader's lighting constants as they are. A shader that does not build on a driver is also logged and left out
// at startup (GLShadersManager::init), which no test can reach.

using namespace Slic3r::GUI::OrcaMCP;

namespace {

boost::filesystem::path shaders_dir() { return boost::filesystem::path(PROFILES_DIR).parent_path() / "shaders"; }

std::string read_all(const boost::filesystem::path& path)
{
    std::ifstream in(path.string());
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::set<std::string> declared_uniforms(const std::string& source)
{
    static const std::regex pattern(R"(^\s*uniform\s+\w+\s+(\w+)\s*;)", std::regex::multiline);
    std::set<std::string> names;
    for (std::sregex_iterator it(source.begin(), source.end(), pattern), end; it != end; ++it)
        names.insert((*it)[1]);
    return names;
}

// The lines that define thumbnail's lighting: its #defines and constants.
std::vector<std::string> lighting_lines(const std::string& source)
{
    std::vector<std::string> lines;
    std::istringstream in(source);
    for (std::string line; std::getline(in, line);)
        if (line.rfind("#define", 0) == 0 || line.rfind("const vec3", 0) == 0)
            lines.push_back(line);
    return lines;
}

} // namespace

TEST_CASE("mcp_render exists in both shader dialects", "[RenderShaderFiles]")
{
    for (const char* version : {"110", "140"})
        for (const char* file : {"mcp_render.vs", "mcp_render.fs"})
            CHECK(boost::filesystem::exists(shaders_dir() / version / file));
}

TEST_CASE("mcp_render declares every uniform the render sets", "[RenderShaderFiles]")
{
    // thumbnail's own, set by RenderThumbnail beside the settings
    const std::set<std::string> vertex_own   = {"view_model_matrix", "projection_matrix", "view_normal_matrix", "volume_world_matrix"};
    const std::set<std::string> fragment_own = {"ban_light", "uniform_color", "emission_factor"};
    for (const char* version : {"110", "140"}) {
        std::set<std::string> all = declared_uniforms(read_all(shaders_dir() / version / "mcp_render.vs"));
        for (const std::string& name : declared_uniforms(read_all(shaders_dir() / version / "mcp_render.fs")))
            all.insert(name);
        INFO("dialect " << version);
        for (const std::string& name : shader_settings_uniform_names())
            CHECK(all.count(name) == 1);
        for (const std::string& name : vertex_own)
            CHECK(all.count(name) == 1);
        for (const std::string& name : fragment_own)
            CHECK(all.count(name) == 1);
    }
}

TEST_CASE("mcp_render keeps thumbnail's lighting constants", "[RenderShaderFiles]")
{
    for (const char* version : {"110", "140"}) {
        const std::vector<std::string> thumbnail = lighting_lines(read_all(shaders_dir() / version / "thumbnail.vs"));
        const std::vector<std::string> mcp       = lighting_lines(read_all(shaders_dir() / version / "mcp_render.vs"));
        INFO("dialect " << version);
        REQUIRE_FALSE(thumbnail.empty());
        CHECK(mcp == thumbnail);
    }
}

TEST_CASE("mcp_render keeps thumbnail's fragment colour", "[RenderShaderFiles]")
{
    for (const char* version : {"110", "140"}) {
        const std::string thumbnail = read_all(shaders_dir() / version / "thumbnail.fs");
        const std::string mcp       = read_all(shaders_dir() / version / "mcp_render.fs");
        INFO("dialect " << version);
        // thumbnail's colour line and its z cut are in the new fragment shader unchanged
        for (const char* needle : {"intensity.y) + uniform_color.rgb * (intensity.x + emission_factor), uniform_color.a)", "world_pos.z < 0.0"})
            CHECK(mcp.find(needle) != std::string::npos);
        CHECK(thumbnail.find("clipping_plane") == std::string::npos);
    }
}

TEST_CASE("mcp_render's default settings are what thumbnail does", "[RenderShaderFiles]")
{
    const ShaderSettings defaults;
    for (const char* version : {"110", "140"}) {
        const std::string thumbnail = read_all(shaders_dir() / version / "thumbnail.vs");
        const std::string mcp       = read_all(shaders_dir() / version / "mcp_render.vs");
        INFO("dialect " << version);

        // The ambient intensity the shipped thumbnail shader uses is the default the render sets.
        static const std::regex ambient(R"(#define\s+INTENSITY_AMBIENT\s+([0-9.]+))");
        std::smatch match;
        REQUIRE(std::regex_search(thumbnail, match, ambient));
        CHECK(std::stod(match[1]) == Catch::Approx(double(defaults.ambient)));

        // The raking light's brightness arithmetic uses thumbnail's top diffuse: 0.8 * INTENSITY_CORRECTION (0.6).
        CHECK(thumbnail.find("#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)") != std::string::npos);
        CHECK(thumbnail.find("#define INTENSITY_CORRECTION 0.6") != std::string::npos);
        CHECK(k_top_diffuse == Catch::Approx(0.8 * 0.6));
        CHECK(defaults.top_gain == 1.f);
        CHECK(mcp.find("NdotL * LIGHT_TOP_DIFFUSE * top_gain") != std::string::npos);

        // Left at the defaults the second light is on and the top light is the fixed one: thumbnail has the front light
        // term, and mcp_render takes it unless front_light is off and the fixed top direction unless use_light_dir.
        CHECK(defaults.front_light);
        CHECK_FALSE(defaults.use_light_dir);
        CHECK(thumbnail.find("LIGHT_FRONT_DIR") != std::string::npos);
        CHECK(mcp.find("if (front_light)") != std::string::npos);
        CHECK(mcp.find("use_light_dir ?") != std::string::npos);

        // The default clipping plane (0, 0, 0, 1) cuts nothing: dot(world_pos, plane) is world_pos.w = 1.
        CHECK(defaults.clipping_plane[0] == 0.);
        CHECK(defaults.clipping_plane[1] == 0.);
        CHECK(defaults.clipping_plane[2] == 0.);
        CHECK(defaults.clipping_plane[3] > 0.);

        // The specular scale multiplies thumbnail's highlight, so the default 1 leaves it as it is.
        CHECK(defaults.specular == 1.f);
        CHECK(mcp.find("intensity.y = specular * LIGHT_TOP_SPECULAR *") != std::string::npos);
        CHECK(thumbnail.find("intensity.y = LIGHT_TOP_SPECULAR *") != std::string::npos);
    }
}
