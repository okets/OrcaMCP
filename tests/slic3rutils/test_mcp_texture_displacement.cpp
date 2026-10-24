#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPNextSteps.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.hpp"
#include "slic3r/GUI/TextureDisplacementObject.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"

// The texture displacement tools' decisions apart from the app: names, the library by id, one call's layer
// edit, masks, the answers' shapes.

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;
using nlohmann::json;

namespace {

TextureLibraryEntry entry(const std::string& name, const std::string& path, TextureSource source, const std::string& pack = {})
{
    TextureLibraryEntry e;
    e.name      = name;
    e.path      = path;
    e.source    = source;
    e.pack_name = pack;
    return e;
}

std::optional<LayerRequest> read(const json& params, std::string& error)
{
    return read_layer_request(params, error);
}

std::string refusal_of(const json& params)
{
    std::string error;
    const auto  req = read(params, error);
    REQUIRE_FALSE(req.has_value());
    return error;
}

TextureDisplacementLayer layer_in(int slot)
{
    TextureDisplacementLayer l;
    l.slot       = slot;
    l.image_data = std::make_shared<std::vector<unsigned char>>(4, 1);
    return l;
}

ModelObject* cube_object(Model& model)
{
    ModelObject* object = model.add_object();
    object->add_instance();
    object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)), ModelVolumeType::MODEL_PART, false);
    return object;
}

void paint_facets(ModelVolume& volume, int slot, int count)
{
    TriangleSelector selector(volume.mesh());
    for (int i = 0; i < count; ++i)
        selector.set_facet(i, EnforcerBlockerType::ENFORCER);
    volume.texture_displacement_facet(slot).set(selector);
}

} // namespace

TEST_CASE("names round-trip", "[McpTexture][orcamcp]")
{
    for (const std::string& n : projection_names())
        CHECK(projection_name(*projection_named(n)) == n);
    for (const std::string& n : blend_names())
        CHECK(blend_name(*blend_named(n)) == n);
    for (const std::string& n : tile_method_names())
        CHECK(tile_method_name(*tile_method_named(n)) == n);
    CHECK(*projection_named("VIEW") == TextureProjectionMethod::ViewProjected);
    CHECK(*projection_named("unwrap") == TextureProjectionMethod::LSCM);
    CHECK(*blend_named("override") == TextureBlendMode::Override);
    CHECK(blend_names().front() == "add");
    CHECK_FALSE(projection_named("planar").has_value());
    CHECK(std::string(texture_source_name(TextureSource::Plugin)) == "pack");
}

TEST_CASE("texture ids are unique and say where a texture comes from", "[McpTexture][orcamcp]")
{
    const std::vector<TextureLibraryEntry> lib = {
        entry("Bark", "/res/Bark.png", TextureSource::Shipped),
        entry("a", "/user/a.png", TextureSource::User),
        entry("a", "/user/a.jpg", TextureSource::User),
        entry("Bark", "/packs/one/Bark.png", TextureSource::Plugin, "One"),
        entry("Bark", "/packs/two/Bark.png", TextureSource::Plugin, "Two"),
        entry("Bark", "/packs/one2/Bark.png", TextureSource::Plugin, "One"),
    };
    const std::vector<std::string> ids = texture_ids(lib);
    REQUIRE(ids.size() == lib.size());
    CHECK(ids[0] == "builtin/Bark.png");
    CHECK(ids[1] == "user/a.png");
    CHECK(ids[2] == "user/a.jpg");
    CHECK(ids[3] == "pack/One/Bark.png");
    CHECK(ids[4] == "pack/Two/Bark.png");
    CHECK(ids[5] == "pack/One/Bark.png #2");
    CHECK(std::set<std::string>(ids.begin(), ids.end()).size() == ids.size());
    CHECK(texture_ids(lib) == ids);

    SECTION("a lookup by id, by stem in any case, ambiguous and unknown")
    {
        CHECK(find_texture(lib, ids, "user/a.jpg").index == 2);
        CHECK(find_texture(lib, ids, "USER/A.PNG").index == 1);
        CHECK(find_texture(lib, ids, "bark").error.find("more than one") != std::string::npos);
        CHECK(find_texture(lib, ids, "bark").error.find("builtin/Bark.png") != std::string::npos);
        const TextureLookup unknown = find_texture(lib, ids, "marble");
        CHECK(unknown.index == -1);
        CHECK(unknown.error.find("get_texture_library") != std::string::npos);
        const std::vector<TextureLibraryEntry> one = {entry("Wood", "/res/Wood.png", TextureSource::Shipped)};
        CHECK(find_texture(one, texture_ids(one), "wood").index == 0);
    }
    SECTION("a layer's image is found by its path")
    {
        TextureDisplacementLayer l;
        l.path = "/user/a.jpg";
        CHECK(*library_index_of(lib, l) == 2);
        l.path = "/elsewhere/a.jpg";
        CHECK_FALSE(library_index_of(lib, l).has_value());
    }
    SECTION("an entry's json")
    {
        const json j = library_entry_json(lib[3], ids[3], true);
        CHECK(j["pack"] == "One");
        CHECK(j["color"] == true);
        CHECK(j["file"] == "Bark.png");
        CHECK(library_entry_json(lib[0], ids[0], std::nullopt)["color"].is_null());
    }
}

TEST_CASE("a PNG header says what decode_height_texture will find", "[McpTexture][orcamcp]")
{
    const std::filesystem::path dir = std::filesystem::path(PROFILES_DIR).parent_path() / "textures" / "displacement";
    REQUIRE(std::filesystem::is_directory(dir));
    int total = 0, colour = 0;
    for (const auto& file : std::filesystem::directory_iterator(dir)) {
        if (file.path().extension() != ".png")
            continue;
        std::ifstream in(file.path(), std::ios::binary);
        auto          bytes = std::make_shared<std::vector<unsigned char>>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        TextureDisplacementLayer layer;
        layer.image_data = bytes;
        const DecodedHeightTexture decoded = decode_height_texture(layer, false);
        REQUIRE_FALSE(decoded.empty());
        const auto header = png_has_color(bytes->data(), bytes->size());
        INFO(file.path().filename().string());
        REQUIRE(header.has_value());
        CHECK(*header == decoded.has_color());
        CHECK(png_color_from_file(file.path().string()) == header);
        ++total;
        colour += *header ? 1 : 0;
    }
    CHECK(total == 44);
    CHECK(colour == 6);
    const unsigned char jpg[] = {0xff, 0xd8, 0xff, 0xe0, 0, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(png_has_color(jpg, sizeof jpg).has_value());
    CHECK_FALSE(png_color_from_file("/no/such/file.png").has_value());
}

TEST_CASE("an image and its colour twin are two textures", "[McpTexture][orcamcp]")
{
    DecodedHeightTexture grey;
    grey.width  = 4;
    grey.height = 2;
    grey.pixels = {0, 10, 20, 30, 40, 50, 60, 70};
    DecodedHeightTexture rgb = grey;
    for (uint8_t v : grey.pixels)
        rgb.rgb.insert(rgb.rgb.end(), {v, v, v});
    REQUIRE(rgb.has_color());

    // The app decodes the one as colour and the other not, so color: true is allowed on one only.
    CHECK_FALSE(same_texture(grey, rgb));
    CHECK(texture_identity(grey).hash != texture_identity(rgb).hash);
    CHECK(same_texture(rgb, rgb));
    CHECK(texture_identity(rgb).hash == texture_identity(DecodedHeightTexture(rgb)).hash);

    DecodedHeightTexture changed = grey;
    changed.pixels[3] = 31;
    CHECK_FALSE(same_texture(grey, changed));
    CHECK(texture_identity(grey).hash != texture_identity(changed).hash);

    DecodedHeightTexture tinted = rgb;
    tinted.rgb[0] = 200;
    CHECK_FALSE(same_texture(rgb, tinted));
    CHECK_FALSE(same_texture(grey, tinted));

    DecodedHeightTexture wider = grey;
    wider.width  = 2;
    wider.height = 4;
    CHECK_FALSE(same_texture(grey, wider));
}

TEST_CASE("every number is held to the panel's range", "[McpTexture][orcamcp]")
{
    const SettingBound& bound = GENERATE_REF(from_range(layer_setting_bounds()));
    INFO(bound.key);
    const bool is_adjust = std::string(bound.key) == "brightness" || std::string(bound.key) == "contrast" ||
                           std::string(bound.key) == "sharpness" || std::string(bound.key) == "blur" || std::string(bound.key) == "hue_deg";
    const bool is_int = std::string(bound.key) == "color_mix_count" || std::string(bound.key) == "color_denoise";
    const auto make = [&](double v) {
        json value = is_int ? json(long(v)) : json(v);
        return is_adjust ? json{{"action", "edit"}, {"layer", 0}, {"adjust_image", {{bound.key, value}}}}
                         : json{{"action", "edit"}, {"layer", 0}, {bound.key, value}};
    };
    std::string error;
    for (double v : {bound.min, bound.max, (bound.min + bound.max) / 2.}) {
        if (is_int)
            v = std::round(v);
        CHECK(read(make(v), error).has_value());
    }
    const double below = is_int ? bound.min - 1. : bound.min - 0.001, above = is_int ? bound.max + 1. : bound.max + 0.001;
    for (double v : {below, above}) {
        REQUIRE_FALSE(read(make(v), error).has_value());
        CHECK(error.find(bound.key) != std::string::npos);
        CHECK(error.find(bound.panel_label) != std::string::npos);
    }
    CHECK_FALSE(read(is_adjust ? json{{"action", "edit"}, {"layer", 0}, {"adjust_image", {{bound.key, "x"}}}}
                               : json{{"action", "edit"}, {"layer", 0}, {bound.key, "x"}}, error).has_value());
}

TEST_CASE("a layer request is read for its structure", "[McpTexture][orcamcp]")
{
    std::string error;
    SECTION("what is read")
    {
        const auto req = read({{"action", "edit"}, {"layer", 3}, {"depth_mm", 0.5}, {"offset", {0.25, -1}}, {"invert", true},
                               {"blend", "override"}, {"projection", "view"}, {"tile_method", "mirrored_repeat"},
                               {"view_direction", "Top"}, {"color_mix", false}, {"color_mix_count", 4}}, error);
        REQUIRE(req.has_value());
        CHECK(req->action == LayerAction::edit);
        CHECK(*req->layer == 3);
        CHECK_THAT(*req->settings.depth_mm, WithinAbs(0.5, 1e-6));
        CHECK(req->settings.offset->y() == -1.f);
        CHECK(*req->settings.blend == TextureBlendMode::Override);
        CHECK(*req->settings.projection == TextureProjectionMethod::ViewProjected);
        CHECK(*req->settings.tile_method == TextureTileMethod::MirroredRepeat);
        CHECK(*req->settings.view_direction == "top");
        CHECK(*req->settings.color_mix_count == 4);
        CHECK_FALSE(req->settings.empty());
        CHECK(read({{"action", "edit"}, {"layer", 0}}, error)->settings.empty());
    }
    SECTION("add takes the first free slot")
    {
        CHECK(read({{"action", "add"}, {"texture", "builtin/Bark.png"}}, error).has_value());
        CHECK(refusal_of({{"action", "add"}, {"layer", 2}}).find("first free slot") != std::string::npos);
    }
    SECTION("edit, remove and move need a slot")
    {
        for (const char* action : {"edit", "remove", "move"})
            CHECK(refusal_of({{"action", action}, {"to_position", 1}}).find("needs layer") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 8}}).find("0 to 7") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 1.5}}).find("integer") != std::string::npos);
    }
    SECTION("remove and move take nothing else")
    {
        CHECK(refusal_of({{"action", "remove"}, {"layer", 0}, {"depth_mm", 1}}).find("takes no depth_mm") != std::string::npos);
        CHECK(refusal_of({{"action", "move"}, {"layer", 0}, {"to_position", 1}, {"texture", "x"}}).find("takes no texture") != std::string::npos);
        CHECK(refusal_of({{"action", "move"}, {"layer", 0}}).find("to_position") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"to_position", 1}}).find("move") != std::string::npos);
        CHECK(read({{"action", "remove"}, {"layer", 0}, {"object_id", 1}}, error).has_value());
        CHECK(read({{"action", "move"}, {"layer", 0}, {"to_position", 1}}, error).has_value());
    }
    SECTION("the action and the types")
    {
        CHECK(refusal_of({{"layer", 0}}).find("action is required") != std::string::npos);
        CHECK(refusal_of({{"action", "spin"}}).find("add, edit, remove or move") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"invert", "yes"}}).find("true or false") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection", "flat"}}).find("one of triplanar") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"texture", 3}}).find("string") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"view_direction", "up"}}).find("one of current") != std::string::npos);
    }
    SECTION("texture and file_path are not both")
    {
        CHECK(refusal_of({{"action", "add"}, {"texture", "a"}, {"file_path", "/b.png"}}).find("send one") != std::string::npos);
    }
    SECTION("offset is two finite numbers")
    {
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"offset", {1}}}).find("2 numbers") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"offset", {1, "x"}}}).find("finite") != std::string::npos);
    }
    SECTION("a view is a direction or axes")
    {
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"view_direction", "top"},
                          {"view_axes", {{"right", {1, 0, 0}}, {"up", {0, 1, 0}}}}}).find("send one") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"view_axes", {{"right", {1, 0, 0}}, {"up", {2, 0, 0}}}}}).find("parallel") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"view_axes", {{"right", {1, 0, 0}}}}}).find("right and up") != std::string::npos);
        const auto req = read({{"action", "edit"}, {"layer", 0}, {"view_axes", {{"right", {1, 0, 0}}, {"up", {0, 0, 1}}}}}, error);
        REQUIRE(req.has_value());
        CHECK(req->settings.view_axes->second.z() == 1.);
    }
    SECTION("a projection frame")
    {
        const json frame = {{"view", "front"}, {"center", {1, 2, 3}}, {"width_mm", 20}, {"height_mm", 10}};
        const auto req   = read({{"action", "edit"}, {"layer", 0}, {"projection_frame", frame}}, error);
        REQUIRE(req.has_value());
        CHECK(req->settings.projection_frame->width_mm == 20.);
        CHECK(req->settings.projection_frame->center.z() == 3.);
        CHECK(read({{"action", "edit"}, {"layer", 0}, {"projection_frame", false}}, error)->settings.clear_projection_frame);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", true}}).find("not a frame") != std::string::npos);
        json bad = frame;
        bad["width_mm"] = 0;
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", bad}}).find("greater than 0") != std::string::npos);
        bad = frame;
        bad.erase("view");
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", bad}}).find("view") != std::string::npos);
        bad = frame;
        bad["right"] = {1, 0, 0};
        bad["up"]    = {0, 1, 0};
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", bad}}).find("not both") != std::string::npos);
        bad = frame;
        bad["colour"] = 1;
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", bad}}).find("no key \"colour\"") != std::string::npos);
    }
    SECTION("adjust_image needs a field")
    {
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"adjust_image", json::object()}}).find("at least one") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"adjust_image", {{"gamma", 1}}}}).find("no key \"gamma\"") != std::string::npos);
        const auto req = read({{"action", "edit"}, {"layer", 0}, {"adjust_image", {{"hue_deg", -90}}}}, error);
        REQUIRE(req.has_value());
        CHECK(adjustments_of(*req->settings.adjust_image).hue_deg == -90.f);
        CHECK(adjustments_of(*req->settings.adjust_image).brightness == 0.f);
    }
}

TEST_CASE("a layer edit is refused for the stack it lands on", "[McpTexture][orcamcp]")
{
    std::vector<TextureDisplacementLayer> stack = {layer_in(0), layer_in(2)};
    StackFacts                            facts;
    facts.physical_filaments = 4;
    const auto check = [&](const json& params, const TextureDisplacementLayer& after) {
        std::string error;
        const auto  req = read(params, error);
        REQUIRE(req.has_value());
        const auto it = req->layer ? std::find_if(stack.begin(), stack.end(), [&](const TextureDisplacementLayer& l) { return l.slot == *req->layer; })
                                   : stack.end();
        return layer_edit_refusal(1, stack, *req, it == stack.end() ? nullptr : &*it, after, facts);
    };

    SECTION("no free slot")
    {
        for (int s = 0; s < 8; ++s)
            if (s != 0 && s != 2)
                stack.push_back(layer_in(s));
        const auto why = check({{"action", "add"}}, layer_in(0));
        REQUIRE(why.has_value());
        CHECK(why->find("object 1") != std::string::npos);
        CHECK(why->find("8 texture layers") != std::string::npos);
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 2}}, layer_in(2)).has_value());
    }
    SECTION("no layer in that slot")
    {
        const auto why = check({{"action", "remove"}, {"layer", 1}}, layer_in(1));
        REQUIRE(why.has_value());
        CHECK(why->find("slot 1") != std::string::npos);
        CHECK(why->find("slots 0, 2") != std::string::npos);
        stack.clear();
        CHECK(check({{"action", "edit"}, {"layer", 0}}, layer_in(0))->find("add one") != std::string::npos);
    }
    SECTION("colour on a grayscale texture")
    {
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"color", true}}, layer_in(0))->find("no colours") != std::string::npos);
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"color", false}}, layer_in(0)).has_value());
        facts.texture_has_color = true;
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"color", true}}, layer_in(0)).has_value());
        // A layer that already colours is not refused for saying so again.
        facts.texture_has_color = false;
        stack[0].color_enabled  = true;
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"color", true}}, layer_in(0)).has_value());
    }
    SECTION("model_uv without UV")
    {
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"projection", "model_uv"}}, layer_in(0))->find("UV") != std::string::npos);
        // Already model_uv (the UV channel went with a 3MF reload): sent back, it is accepted.
        stack[0].projection_method = TextureProjectionMethod::ModelUV;
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"projection", "model_uv"}}, layer_in(0)).has_value());
        stack[0].projection_method = TextureProjectionMethod::Triplanar;
        facts.owner_has_uv = true;
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"projection", "model_uv"}}, layer_in(0)).has_value());
    }
    SECTION("more mixed colours than slots")
    {
        facts.physical_filaments = 30;
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"color_mix_count", 3}}, layer_in(0))->find("room for") != std::string::npos);
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"color_mix_count", 2}}, layer_in(0)).has_value());
        facts.color_mix_count = 3; // the stack already holds it
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"color_mix_count", 3}}, layer_in(0)).has_value());
        facts.color_mix_count = -1;
        facts.physical_filaments = 40;
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"color_mix_count", 1}}, layer_in(0)).has_value());
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"color_mix_count", 2}}, layer_in(0)).has_value());
    }
    SECTION("a view for a layer that does not project from one")
    {
        TextureDisplacementLayer after = layer_in(0);
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"view_direction", "top"}}, after)->find("projection view") != std::string::npos);
        after.projection_method = TextureProjectionMethod::ViewProjected;
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"view_direction", "top"}}, after).has_value());
    }
    SECTION("move to a position that is not there")
    {
        CHECK(check({{"action", "move"}, {"layer", 0}, {"to_position", 2}}, layer_in(0))->find("past the last position, 1") != std::string::npos);
        CHECK_FALSE(check({{"action", "move"}, {"layer", 0}, {"to_position", 1}}, layer_in(0)).has_value());
    }
    SECTION("adjusting a layer with no image")
    {
        TextureDisplacementLayer bare;
        bare.slot = 0;
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"adjust_image", {{"blur", 0.5}}}}, bare)->find("no image") != std::string::npos);
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"adjust_image", {{"blur", 0.5}}}}, layer_in(0)).has_value());
    }
}

TEST_CASE("a new layer takes the first free slot and the model's UV", "[McpTexture][orcamcp]")
{
    CHECK(*first_free_slot({}) == 0);
    CHECK(*first_free_slot({layer_in(0), layer_in(1), layer_in(3)}) == 2);
    std::vector<TextureDisplacementLayer> full;
    for (int s = 0; s < 8; ++s)
        full.push_back(layer_in(s));
    CHECK_FALSE(first_free_slot(full).has_value());
    CHECK(new_layer(4, false).slot == 4);
    CHECK(new_layer(4, false).projection_method == TextureProjectionMethod::Triplanar);
    CHECK(new_layer(4, true).projection_method == TextureProjectionMethod::ModelUV);
}

TEST_CASE("layer settings map onto the layer and report a change", "[McpTexture][orcamcp]")
{
    TextureDisplacementLayer layer = layer_in(0);
    std::string              error;
    const auto               req = read({{"action", "edit"}, {"layer", 0}, {"depth_mm", 1.5}, {"tile_size_mm", 20}, {"rotation_deg", 45},
                                         {"midlevel", 0.5}, {"smoothing", 0.25}, {"edge_fade", true}, {"edge_fade_amount", 0.75},
                                         {"offset", {0.5, 0.25}}, {"invert", true}, {"tile", false}, {"color", true},
                                         {"tile_method", "mirrored_repeat"}, {"projection", "spherical"}, {"blend", "multiply"}}, error);
    REQUIRE(req.has_value());
    CHECK(apply_layer_settings(layer, req->settings));
    CHECK(layer.depth_mm == 1.5f);
    CHECK(layer.tiling_scale == 20.f);
    CHECK(layer.rotation_deg == 45.f);
    CHECK(layer.midlevel == 0.5f);
    CHECK(layer.smoothing == 0.25f);
    CHECK(layer.edge_smoothing);
    CHECK(layer.edge_smoothing_amount == 0.75f);
    CHECK(layer.offset == Vec2f(0.5f, 0.25f));
    CHECK(layer.invert);
    CHECK_FALSE(layer.tile_enabled);
    CHECK(layer.color_enabled);
    CHECK(layer.tile_method == TextureTileMethod::MirroredRepeat);
    CHECK(layer.projection_method == TextureProjectionMethod::Spherical);
    CHECK(layer.blend_mode == TextureBlendMode::Multiply);
    CHECK_FALSE(apply_layer_settings(layer, req->settings));
    CHECK_FALSE(apply_layer_settings(layer, LayerSettings{}));

    SECTION("the panel's Clear for a projection frame")
    {
        layer.view_project_projective = true;
        LayerSettings clear;
        clear.clear_projection_frame = true;
        CHECK(apply_layer_settings(layer, clear));
        CHECK_FALSE(layer.view_project_projective);
        CHECK(layer.tile_enabled);
        CHECK_FALSE(apply_layer_settings(layer, clear));
    }
    SECTION("clearing a frame that is not there leaves tiling alone")
    {
        layer.tile_enabled = false;
        LayerSettings clear;
        clear.clear_projection_frame = true;
        CHECK_FALSE(apply_layer_settings(layer, clear));
        CHECK_FALSE(layer.tile_enabled);
    }
    SECTION("the stack's colour options")
    {
        TextureDisplacementOptions options;
        const auto stack_req = read({{"action", "edit"}, {"layer", 0}, {"color_mix", false}, {"color_mix_count", 5}, {"color_denoise", 4}}, error);
        REQUIRE(stack_req.has_value());
        CHECK(apply_stack_settings(options, stack_req->settings));
        CHECK_FALSE(options.color_mix_enabled);
        CHECK(options.color_mix_count == 5);
        CHECK(options.color_despeckle == 4);
        CHECK_FALSE(apply_stack_settings(options, stack_req->settings));
    }
}

TEST_CASE("a view projector is the camera's axes in the volume's frame", "[McpTexture][orcamcp]")
{
    Transform3d volume = Transform3d::Identity();
    volume.translate(Vec3d(5, -3, 2));
    volume.rotate(Eigen::AngleAxisd(0.7, Vec3d(1, 2, 3).normalized()));
    volume.scale(Vec3d(2., 1., 0.5));

    for (const char* view : {"top", "front", "left", "iso"}) {
        Camera camera;
        camera.select_view(view);
        const Matrix3d  view_rot = camera.get_view_matrix().matrix().block<3, 3>(0, 0);
        const Vec3d     right = view_rot.row(0).transpose(), up = view_rot.row(1).transpose();
        // capture_view_projection's own steps
        const Matrix3d  world_to_local = volume.matrix().block<3, 3>(0, 0).inverse();
        const Vec3d     local_right = world_to_local * right, local_up = world_to_local * up;
        const Vec3f     expect_right(local_right.normalized().cast<float>()), expect_up(local_up.normalized().cast<float>());
        const auto [r, u] = view_projector_axes(camera.get_dir_right(), camera.get_dir_up(), volume);
        INFO(view);
        CHECK((r - expect_right).norm() < 1e-6f);
        CHECK((u - expect_up).norm() < 1e-6f);
    }
    const auto [r, u] = view_projector_axes(Vec3d::Zero(), Vec3d::Zero(), volume);
    CHECK(r == Vec3f::UnitX());
    CHECK(u == Vec3f::UnitY());
}

TEST_CASE("an orthographic frame puts its corners at the image's corners", "[McpTexture][orcamcp]")
{
    const Vec3d right(1, 0, 0), up(0, 0, 1), center(10, 5, 8);
    const double width = 20., height = 10.;
    // The frame's top left is centre - right * w/2 + up * h/2; v runs down the image.
    const Vec3d top_left = center - right * (width / 2) + up * (height / 2);

    Transform3d identity = Transform3d::Identity();
    Transform3d moved    = Transform3d::Identity();
    moved.translate(Vec3d(4, -2, 1));
    moved.rotate(Eigen::AngleAxisd(0.9, Vec3d(0, 0, 1)));
    moved.scale(Vec3d(2., 3., 1.5));

    for (const Transform3d& volume : {identity, moved}) {
        const std::array<float, 12> m = orthographic_frame_matrix(right, up, center, width, height, volume);
        const Transform3d           back = volume.inverse();
        const auto uv_of = [&](const Vec3d& world) {
            Vec2f uv(-1e9f, -1e9f);
            REQUIRE(project_uv_projective(m, (back * world).cast<float>(), uv));
            return uv;
        };
        const Vec2f a = uv_of(top_left), b = uv_of(top_left + right * width), c = uv_of(top_left - up * height);
        CHECK_THAT(a.x(), WithinAbs(0., 1e-4));
        CHECK_THAT(a.y(), WithinAbs(0., 1e-4));
        CHECK_THAT(b.x(), WithinAbs(1., 1e-4));
        CHECK_THAT(b.y(), WithinAbs(0., 1e-4));
        CHECK_THAT(c.x(), WithinAbs(0., 1e-4));
        CHECK_THAT(c.y(), WithinAbs(1., 1e-4));
        const Vec2f mid = uv_of(center);
        CHECK_THAT(mid.x(), WithinAbs(0.5, 1e-4));
        CHECK_THAT(mid.y(), WithinAbs(0.5, 1e-4));
    }
    CHECK(orthographic_frame_matrix(right, up, center, width, height, identity)[11] == 1.f);
}

TEST_CASE("what the panel greys out but stores is reported", "[McpTexture][orcamcp]")
{
    TextureDisplacementOptions options;
    TextureDisplacementLayer   base = layer_in(0), top = layer_in(1);
    const std::vector<TextureDisplacementLayer> stack = {base, top};

    json inactive = inactive_settings(stack, base, options, true, false);
    CHECK(inactive.contains("blend"));
    CHECK(inactive.contains("edge_fade_amount"));
    CHECK(inactive.contains("color_mix_count"));
    CHECK_FALSE(inactive.contains("tile_size_mm"));
    CHECK_FALSE(inactive.contains("tile_method"));
    CHECK_FALSE(inactive_settings(stack, top, options, true, false).contains("blend"));

    SECTION("tile size under model_uv")
    {
        top.projection_method = TextureProjectionMethod::ModelUV;
        CHECK(inactive_settings(stack, top, options, true, false).contains("tile_size_mm"));
        CHECK_FALSE(inactive_settings(stack, top, options, true, false).contains("projection"));
        CHECK(inactive_settings(stack, top, options, false, false).contains("projection"));
        // Without UV the layer maps by mm, so the tile size is live.
        CHECK_FALSE(inactive_settings(stack, top, options, false, false).contains("tile_size_mm"));
    }
    SECTION("edge fade, tile and colour")
    {
        top.edge_smoothing = true;
        top.tile_enabled   = false;
        top.color_enabled  = true;
        const std::vector<TextureDisplacementLayer> colouring = {base, top};
        const json                                  j         = inactive_settings(colouring, top, options, false, false);
        CHECK_FALSE(j.contains("edge_fade_amount"));
        CHECK(j.contains("tile_method"));
        CHECK(j.contains("color"));
        CHECK_FALSE(j.contains("color_mix_count"));
        CHECK_FALSE(inactive_settings(colouring, top, options, false, true).contains("color"));
    }
    SECTION("placement under a projection frame")
    {
        top.projection_method       = TextureProjectionMethod::ViewProjected;
        top.view_project_projective = true;
        top.tile_enabled            = false;
        const json j                = inactive_settings(stack, top, options, false, false);
        for (const char* key : {"tile_size_mm", "rotation_deg", "offset", "tile_method"})
            CHECK(j.contains(key));
        top.tile_enabled = true; // tiling on under a frame: the method is live
        CHECK_FALSE(inactive_settings(stack, top, options, false, false).contains("tile_method"));
    }
    CHECK(layer_warnings(base).empty());
    base.midlevel = 0.5f;
    base.depth_mm = 2.f;
    CHECK(layer_warnings(base).size() == 1);
}

TEST_CASE("a mask indexing facets the mesh lacks is stale", "[McpTexture][orcamcp]")
{
    PaintData data;
    CHECK_FALSE(mask_is_stale(data, 12));
    data.triangles_to_split.emplace_back(11, 0);
    CHECK_FALSE(mask_is_stale(data, 12));
    CHECK(mask_is_stale(data, 11));
    data.triangles_to_split.emplace_back(-1, 4);
    CHECK(mask_is_stale(data, 100));
}

TEST_CASE("mask reports count what each layer paints, and a stale mask apart", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* part   = object->volumes.front();
    REQUIRE(part->mesh().its.indices.size() == 12);
    paint_facets(*part, 2, 4);

    MaskInput input;
    input.volume_id = 0;
    input.name      = "cube";
    input.mesh      = std::make_shared<TriangleMesh>(part->mesh());
    for (size_t slot = 0; slot < input.masks.size(); ++slot)
        input.masks[slot] = part->texture_displacement_facet(int(slot)).get_data();
    PaintData stale;
    stale.triangles_to_split.emplace_back(40, 0);
    input.masks[5] = stale;

    const auto reports = mask_reports({input});
    REQUIRE(reports.size() == 1);
    const MaskReport& painted = reports[0][2];
    CHECK(painted.painted_facets == 4);
    CHECK_THAT(painted.area_ratio, WithinAbs(1. / 3., 1e-6));
    CHECK(painted.name == "cube");
    CHECK_FALSE(painted.stale);
    CHECK(reports[0][5].stale);
    CHECK(reports[0][5].painted_facets == 0);
    CHECK(reports[0][0].painted_facets == 0);

    SECTION("a stale mask needs no bake, in the fast form too")
    {
        PaintData bad;
        bad.triangles_to_split.emplace_back(40, 0);
        bad.used_states[1] = true;
        part->texture_displacement_facet(5).set_data(PaintData(bad));
        part->texture_displacement_layers = {layer_in(5)};
        CHECK_FALSE(needs_bake({layer_in(5)}, reports));
        CHECK_FALSE(needs_bake_fast(*object, part));
        part->texture_displacement_layers = {layer_in(5), layer_in(2)};
        CHECK(needs_bake_fast(*object, part));
    }
    SECTION("a bake is needed for a painted layer only")
    {
        CHECK_FALSE(needs_bake({layer_in(0), layer_in(1)}, reports));
        CHECK(needs_bake({layer_in(2)}, reports));
        CHECK_FALSE(needs_bake({layer_in(5)}, reports));
        CHECK_FALSE(needs_bake({}, reports));

        part->texture_displacement_layers = {layer_in(2)};
        CHECK(needs_bake_fast(*object, part));
        part->texture_displacement_layers = {layer_in(1)};
        CHECK_FALSE(needs_bake_fast(*object, part));
        CHECK_FALSE(needs_bake_fast(*object, nullptr));
    }
}

TEST_CASE("a part's texture paint is told per layer slot, a stale mask apart", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* part   = object->volumes.front();
    paint_facets(*part, 2, 4);
    part->texture_displacement_layers = {layer_in(5), layer_in(2)};

    MaskInput input = mask_input_of(*part, 3);
    CHECK(input.volume_id == 3);
    CHECK(input.name == part->name);
    CHECK(input.masks[2] == part->texture_displacement_facet(2).get_data());
    PaintData stale;
    stale.triangles_to_split.emplace_back(40, 0);
    input.masks[5] = stale;

    CHECK(layer_slots_of(part) == std::vector<int>{2, 5});
    CHECK(layer_slots_of(nullptr).empty());

    const auto     reports = mask_reports({input});
    nlohmann::json layers  = texture_paint_layers_json(reports[0], layer_slots_of(part));
    REQUIRE(layers.size() == 2);
    CHECK(layers[0]["layer"] == 2);
    CHECK(layers[0]["painted_facets"] == 4);
    CHECK_THAT(layers[0]["coverage_percent"].get<double>(), WithinAbs(100. / 3., 1e-6));
    CHECK_FALSE(layers[0].contains("stale"));
    CHECK(layers[1]["layer"] == 5);
    CHECK(layers[1]["painted_facets"] == 0);
    CHECK(layers[1]["stale"] == true);
    CHECK(texture_paint_layers_json(reports[0], {9, -1}).empty());
}

TEST_CASE("an object and its volume rows say their texture state without a selector", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* part   = object->volumes.front();

    SECTION("a plain object has none")
    {
        const nlohmann::json fields = texture_object_fields(*object);
        CHECK(fields["texture_layers"] == 0);
        CHECK(fields["needs_bake"] == false);
        CHECK(fields["model_uv"] == false);
        const nlohmann::json row = texture_volume_fields(*object, 0);
        CHECK(row["texture_layers"] == 0);
        CHECK(row["model_uv"] == false);
        CHECK(row["texture_paint_layers"].empty());
    }
    SECTION("layers, paint and a stale mask")
    {
        paint_facets(*part, 2, 4);
        part->texture_displacement_layers = {layer_in(2), layer_in(5)};
        PaintData stale;
        stale.triangles_to_split.emplace_back(40, 0);
        stale.used_states[1] = true;
        part->texture_displacement_facet(5).set_data(PaintData(stale));

        const nlohmann::json fields = texture_object_fields(*object);
        CHECK(fields["texture_layers"] == 2);
        CHECK(fields["needs_bake"] == true);
        PaintData unpainted; // an entry whose state is 0: stored, but nothing is painted
        unpainted.triangles_to_split.emplace_back(0, 0);
        part->texture_displacement_facet(6).set_data(PaintData(unpainted));
        const nlohmann::json row = texture_volume_fields(*object, 0);
        CHECK(row["texture_layers"] == 2);
        // slot 5 is stale and slot 6 holds no painted state: neither is paint
        CHECK(row["texture_paint_layers"] == nlohmann::json::array({2}));

        part->texture_displacement_facet(2).set_data(PaintData());
        CHECK(texture_object_fields(*object)["needs_bake"] == false); // only the stale mask is left
    }
    SECTION("only the first model part owns the stack")
    {
        part->texture_displacement_layers = {layer_in(0)};
        ModelVolume* second = object->add_volume(part->mesh());
        second->texture_displacement_layers = {layer_in(0), layer_in(1)};
        CHECK(texture_volume_fields(*object, 0)["texture_layers"] == 1);
        CHECK(texture_volume_fields(*object, 1)["texture_layers"] == 0);
        CHECK(texture_object_fields(*object)["texture_layers"] == 1);
    }
}

TEST_CASE("a mask is stale when its first or last entry is outside the mesh", "[McpTexture][orcamcp]")
{
    const auto mask_of = [](std::initializer_list<int> triangles) {
        PaintData data;
        int       offset = 0;
        for (int triangle : triangles) {
            data.triangles_to_split.emplace_back(triangle, offset);
            offset += 4;
        }
        return data;
    };
    CHECK_FALSE(mask_is_stale(PaintData(), 12));
    CHECK_FALSE(mask_is_stale(mask_of({0, 5, 11}), 12));
    CHECK(mask_is_stale(mask_of({0, 5, 11}), 11)); // the last entry
    CHECK(mask_is_stale(mask_of({-1, 5}), 12));    // the first entry
    CHECK(mask_is_stale(mask_of({3, 12}), 12));
    CHECK_FALSE(mask_is_stale(mask_of({3, 12}), 13));
}

TEST_CASE("masks are copied and reported for the slots asked for", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* part   = object->volumes.front();
    paint_facets(*part, 2, 4);
    paint_facets(*part, 3, 6);

    const std::vector<int> only_two = {2};
    MaskInput              input    = mask_input_of(*part, 0, &only_two);
    CHECK_FALSE(input.masks[2].triangles_to_split.empty());
    CHECK(input.masks[3].triangles_to_split.empty());

    MaskInput all = mask_input_of(*part, 0);
    const auto limited = mask_reports({all}, &only_two);
    CHECK(limited[0][2].painted_facets == 4);
    CHECK(limited[0][3].painted_facets == 0); // slot 3 is painted but was not asked for
    CHECK(mask_reports({all})[0][3].painted_facets == 6);

    const nlohmann::json cleared = texture_paint_layers_cleared_json({2, 3});
    REQUIRE(cleared.size() == 2);
    CHECK(cleared[1]["layer"] == 3);
    CHECK(cleared[1]["painted_facets"] == 0);
    CHECK_FALSE(cleared[1].contains("stale"));
}

TEST_CASE("the texture layers painting a facet come from its own leaf states", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* part   = object->volumes.front();
    paint_facets(*part, 2, 4); // facets 0..3
    paint_facets(*part, 4, 1); // facet 0 only
    PaintData stale;
    stale.triangles_to_split.emplace_back(40, 0);
    stale.used_states[1] = true;
    part->texture_displacement_facet(5).set_data(PaintData(stale));

    MaskInput input = mask_input_of(*part, 0);
    CHECK(texture_slots_painting_facet(input, 0) == std::vector<int>{2, 4});
    CHECK(texture_slots_painting_facet(input, 3) == std::vector<int>{2});
    CHECK(texture_slots_painting_facet(input, 4).empty());
    CHECK(texture_slots_painting_facet(input, 7).empty());
    CHECK(texture_slots_painting_facet(input, -1).empty());
    CHECK(texture_slots_painting_facet(input, 99).empty());
    CHECK(has_texture_paint(*part));
    part->texture_displacement_facet(2).set_data(PaintData());
    part->texture_displacement_facet(4).set_data(PaintData());
    CHECK_FALSE(has_texture_paint(*part)); // slot 5 is stale
}

TEST_CASE("get_object_paint and clear_object_paint read mode and texture_layer", "[McpTexture][orcamcp]")
{
    PaintSelection selection;
    std::string    error;
    const std::vector<int> stack = {1, 4};

    SECTION("no mode is the four paint modes")
    {
        REQUIRE(resolve_paint_selection({}, stack, false, selection, error));
        CHECK(selection.modes.size() == 4);
        CHECK_FALSE(selection.texture);
        CHECK(selection.label == "all");
        CHECK_FALSE(selection.named_mode);
    }
    SECTION("texture_layer without mode texture is refused")
    {
        CHECK_FALSE(resolve_paint_selection({{"texture_layer", 1}}, stack, false, selection, error));
        CHECK(error.find("texture_layer belongs to mode texture") != std::string::npos);
        CHECK_FALSE(resolve_paint_selection({{"mode", "color"}, {"texture_layer", 1}}, stack, false, selection, error));
    }
    SECTION("a read of texture takes the stack's slots, a clear all eight")
    {
        REQUIRE(resolve_paint_selection({{"mode", "Texture"}}, stack, false, selection, error));
        CHECK(selection.texture);
        CHECK(selection.modes.empty());
        CHECK(selection.texture_slots == stack);
        CHECK(selection.label == "texture");
        REQUIRE(resolve_paint_selection({{"mode", "texture"}}, {}, true, selection, error));
        CHECK(selection.texture_slots.size() == TEXTURE_DISPLACEMENT_MAX_LAYERS);
    }
    SECTION("a named layer is that slot, in range")
    {
        REQUIRE(resolve_paint_selection({{"mode", "texture"}, {"texture_layer", 6}}, stack, true, selection, error));
        CHECK(selection.texture_slots == std::vector<int>{6});
        CHECK_FALSE(resolve_paint_selection({{"mode", "texture"}, {"texture_layer", 8}}, stack, true, selection, error));
        CHECK_FALSE(resolve_paint_selection({{"mode", "texture"}, {"texture_layer", -1}}, stack, true, selection, error));
        CHECK_FALSE(resolve_paint_selection({{"mode", "texture"}, {"texture_layer", "abc"}}, stack, true, selection, error));
    }
    SECTION("an unknown mode is refused")
    {
        CHECK_FALSE(resolve_paint_selection({{"mode", "mmu"}}, stack, false, selection, error));
        CHECK_FALSE(resolve_paint_selection({{"mode", 3}}, stack, false, selection, error));
    }
}

TEST_CASE("a texture mapping is refused when it cannot erase or renumber a mask", "[McpTexture][orcamcp]")
{
    CHECK(texture_remap_refusal({{0, 1}, {1, 0}}).empty()); // inversion
    CHECK(texture_remap_refusal({{0, 1}}).empty());
    CHECK(texture_remap_refusal({{1, 1}}).empty());
    CHECK(texture_remap_refusal({{1, 0}, {0, 1}}).empty());
    CHECK(texture_remap_refusal({{1, 0}}).find("clear_object_paint") != std::string::npos);
    CHECK_FALSE(texture_remap_refusal({{1, 0}, {0, 0}}).empty());
    CHECK(texture_remap_refusal({{2, 0}}).find("2 cannot be renumbered") != std::string::npos);
}

TEST_CASE("what a mesh edit dropped is told from the state before", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* part   = object->volumes.front();
    part->texture_displacement_layers = {layer_in(0), layer_in(3)};
    paint_facets(*part, 0, 2);
    paint_facets(*part, 3, 5);
    const TextureState before = texture_state_of(*object);
    REQUIRE(before.parts.size() == 1);
    CHECK(before.parts[0].painted_slots == std::vector<int>{0, 3});
    CHECK(before.parts[0].layers == 2);

    SECTION("nothing lost")
    {
        CHECK(texture_changes_json(before, {object}, part).empty());
    }
    SECTION("the masks were reset")
    {
        part->texture_displacement_facet(0).reset();
        part->texture_displacement_facet(3).reset();
        const json j = texture_changes_json(before, {object}, part);
        REQUIRE(j.contains("paint_lost"));
        CHECK(j["paint_lost"][0]["layers"] == json::array({0, 3}));
        CHECK(j["paint_lost"][0]["name"] == part->name);
        CHECK_FALSE(j.contains("layers_lost"));
    }
    SECTION("the part is gone, and its stack with it")
    {
        Model        other;
        ModelObject* fresh = cube_object(other);
        const json   j     = texture_changes_json(before, {fresh}, fresh->volumes.front());
        CHECK(j["paint_lost"][0]["layers"] == json::array({0, 3}));
        CHECK(j["layers_lost"] == 2);
    }
    SECTION("a stack that moved to the second part is left unused")
    {
        ModelVolume* second = object->add_volume(TriangleMesh(its_make_cube(5., 5., 5.)), ModelVolumeType::MODEL_PART, false);
        second->texture_displacement_layers = part->texture_displacement_layers;
        part->texture_displacement_layers.clear();
        part->texture_displacement_facet(0).reset();
        const json j = texture_changes_json(before, {object}, part);
        REQUIRE(j.contains("stacks_unused"));
        CHECK(j["stacks_unused"][0]["volume_id"] == 1);
        CHECK(j["stacks_unused"][0]["layers"] == 2);
        CHECK(j["layers_lost"] == 2);
        CHECK(j.contains("paint_lost"));
    }
}

TEST_CASE("a first part turned into a modifier leaves its stack unused, not lost", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* first  = object->volumes.front();
    ModelVolume* second = object->add_volume(TriangleMesh(its_make_cube(5., 5., 5.)), ModelVolumeType::MODEL_PART, false);
    first->texture_displacement_layers = {layer_in(0)};
    paint_facets(*first, 0, 3);
    const TextureState before = texture_state_of(*object);

    first->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    const json j = texture_changes_json(before, {object}, second);
    CHECK_FALSE(j.contains("paint_lost"));
    CHECK_FALSE(j.contains("layers_lost"));
    REQUIRE(j.contains("stacks_unused"));
    CHECK(j["stacks_unused"][0]["volume_id"] == 0);
}

TEST_CASE("a mesh edit's texture changes are told in words and found again by volume", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* part   = object->volumes.front();
    part->texture_displacement_layers = {layer_in(0), layer_in(2)};
    paint_facets(*part, 0, 2);
    paint_facets(*part, 2, 4);
    const TextureState before = texture_state_of(*object);
    CHECK(before.parts[0].stack_owner);
    CHECK(before.parts[0].volume == part);
    CHECK(before.volume_count == 1);

    SECTION("a repaired volume has a new id and lost its paint, not its layers")
    {
        part->texture_displacement_facet(0).reset();
        part->texture_displacement_facet(2).reset();
        part->set_new_unique_id();
        const json changes = texture_changes_json(before, {object}, nullptr);
        CHECK_FALSE(changes.contains("layers_lost"));
        REQUIRE(changes.contains("paint_lost"));
        const auto said = texture_changes_messages(changes, "object 3", "the mesh changed");
        REQUIRE(said.size() == 1);
        CHECK(said[0] == "object 3 lost the paint of texture layers 0 and 2 on part 0: the mesh changed; that region has to be painted again (paint_object mode texture)");
        json answer = {{"next_steps", json::array({{{"tool", "slice_all"}, {"why", "x"}}})}};
        add_texture_changes(answer, changes, 3, "the mesh changed");
        CHECK(answer["texture_changes"] == changes);
        CHECK(answer["info_messages"].size() == 1);
        REQUIRE(answer["next_steps"].size() == 2);
        CHECK(answer["next_steps"][0]["tool"] == "slice_all");
        CHECK(answer["next_steps"][1]["tool"] == "get_object_paint");
    }
    SECTION("a split to parts keeps the first piece with its layers under a new id")
    {
        part->texture_displacement_facet(0).reset();
        part->texture_displacement_facet(2).reset();
        part->set_new_unique_id();
        object->add_volume(TriangleMesh(its_make_cube(5., 5., 5.)), ModelVolumeType::MODEL_PART, false);
        const json changes = texture_changes_json(before, {object}, nullptr);
        CHECK_FALSE(changes.contains("layers_lost")); // would make set_texture_displacement add duplicate a stack
        CHECK_FALSE(changes.contains("stacks_unused"));
        REQUIRE(changes.contains("paint_lost"));
        CHECK(changes["paint_lost"][0]["layers"] == json::array({0, 2}));
    }
    SECTION("losing paint and layers is told together")
    {
        Model        other;
        ModelObject* fresh = cube_object(other);
        const json   changes = texture_changes_json(before, {fresh}, nullptr);
        CHECK(changes["layers_lost"] == 2);
        CHECK(changes.contains("paint_lost"));
        const auto steps = texture_changes_next_steps(1, changes);
        REQUIRE(steps.size() == 2);
        CHECK(steps[0].tool == "set_texture_displacement");
        CHECK(steps[1].tool == "get_object_paint");
    }
    SECTION("a reloaded part is a new volume in the same place, found by position")
    {
        Model        other;
        ModelObject* fresh = cube_object(other);
        fresh->volumes.front()->texture_displacement_layers = part->texture_displacement_layers;
        paint_facets(*fresh->volumes.front(), 0, 2);
        TextureState moved = before;
        CHECK(texture_changes_json(moved, {fresh}, nullptr).value("layers_lost", 0) == 2); // by identity it is another part
        rekey_texture_state(moved, *fresh);
        const json changes = texture_changes_json(moved, {fresh}, nullptr);
        CHECK_FALSE(changes.contains("layers_lost"));
        REQUIRE(changes.contains("paint_lost"));
        CHECK(changes["paint_lost"][0]["layers"] == json::array({2}));
    }
    SECTION("a rekey is skipped when the volume count changed")
    {
        Model        other;
        ModelObject* fresh = cube_object(other);
        fresh->volumes.front()->texture_displacement_layers = part->texture_displacement_layers;
        fresh->add_volume(TriangleMesh(its_make_cube(5., 5., 5.)), ModelVolumeType::MODEL_PART, false);
        TextureState moved = before;
        rekey_texture_state(moved, *fresh);
        CHECK(moved.parts[0].id == before.parts[0].id);
        CHECK(moved.parts[0].volume == part);
        CHECK(texture_changes_json(moved, {fresh}, nullptr).value("layers_lost", 0) == 2); // still another part
    }
    SECTION("a deleted part's paint is forgotten, its layers are not")
    {
        TextureState deleted = before;
        forget_texture_paint_of(deleted, 0);
        Model        other;
        ModelObject* fresh = cube_object(other);
        const json   changes = texture_changes_json(deleted, {fresh}, nullptr);
        CHECK_FALSE(changes.contains("paint_lost"));
        CHECK(changes["layers_lost"] == 2);
    }
    SECTION("a UV channel the edit dropped is told")
    {
        part->texture_uv.assign(part->mesh().its.indices.size() * 3, Vec2f::Zero());
        const TextureState with_uv = texture_state_of(*object);
        REQUIRE(with_uv.parts[0].has_uv);
        part->reset_texture_uv();
        const json changes = texture_changes_json(with_uv, {object}, nullptr);
        REQUIRE(changes.contains("uv_lost"));
        CHECK(changes["uv_lost"][0]["volume_id"] == 0);
    }
    SECTION("unwrap edits on a mesh that changed are told, on an unchanged one not")
    {
        TextureState edited = before;
        edited.parts[0].unwrap_edits = true;
        CHECK_FALSE(texture_changes_json(edited, {object}, nullptr).contains("unwrap_edits_stale"));
        edited.parts[0].facets += 4;
        const json changes = texture_changes_json(edited, {object}, nullptr);
        REQUIRE(changes.contains("unwrap_edits_stale"));
        CHECK(changes["unwrap_edits_stale"][0]["volume_id"] == 0);
    }
    SECTION("several objects get one summary line and one step naming them all")
    {
        json answer = json::object();
        const json one = {{"layers_lost", 1}};
        add_objects_texture_changes(answer, {{2, one}, {5, one}, {7, json::object()}}, "x");
        REQUIRE(answer["info_messages"].size() == 1);
        CHECK(answer["texture_changes"].size() == 2);
        CHECK(answer["texture_changes"][1]["object_id"] == 5);
        REQUIRE(answer["next_steps"].size() == 1);
        CHECK(answer["next_steps"][0]["arguments"]["object_id"] == 2);
        CHECK(answer["next_steps"][0]["why"].get<std::string>().find("objects 2 and 5") != std::string::npos);
    }
    SECTION("nothing lost adds nothing")
    {
        json answer = json::object();
        add_texture_changes(answer, texture_changes_json(before, {object}, nullptr), 0, "x");
        CHECK(answer.empty());
    }
}

TEST_CASE("paint on a part turned into a modifier is told as unused", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* first  = object->volumes.front();
    object->add_volume(TriangleMesh(its_make_cube(5., 5., 5.)), ModelVolumeType::MODEL_PART, false);
    paint_facets(*first, 1, 3);
    const TextureState before = texture_state_of(*object);
    first->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    const json changes = texture_changes_json(before, {object}, nullptr);
    CHECK_FALSE(changes.contains("paint_lost"));
    REQUIRE(changes.contains("paint_unused"));
    CHECK(changes["paint_unused"][0]["layers"] == json::array({1}));
}

TEST_CASE("a layer read back is accepted and changes nothing", "[McpTexture][orcamcp]")
{
    TextureDisplacementOptions options;
    TextureDisplacementLayer   layer = layer_in(1);
    layer.name                       = "Bark";
    layer.depth_mm                   = 0.8f;
    layer.offset                     = Vec2f(0.25f, 0.5f);
    layer.projection_method          = TextureProjectionMethod::ViewProjected;
    layer.view_project_projective    = true;
    layer.tile_enabled               = false;
    layer.view_project_right         = Vec3f(0, 1, 0);
    layer.view_project_up            = Vec3f(0, 0, 1);
    LayerTexture texture;
    texture.id = "builtin/Bark.png";

    // The volume is turned a quarter about Z, so the report is in plate axes, not the stored volume ones.
    Transform3d turned = Transform3d::Identity();
    turned.rotate(Eigen::AngleAxisd(M_PI / 2., Vec3d::UnitZ()));
    json echoed = layer_json(layer, 0, true, texture, true, {}, true, {layer}, options, turned);
    CHECK_THAT(echoed["view_axes"]["right"][0].get<double>(), WithinAbs(-1., 1e-6));
    CHECK_THAT(echoed["view_axes"]["right"][1].get<double>(), WithinAbs(0., 1e-6));
    echoed["action"] = "edit";

    std::string error;
    const auto  req = read_layer_request(echoed, error);
    INFO(error);
    REQUIRE(req.has_value());
    TextureDisplacementLayer after = layer;
    CHECK_FALSE(apply_layer_settings(after, req->settings));
    REQUIRE(req->settings.view_axes.has_value());
    const auto [right, up] = view_projector_axes(req->settings.view_axes->first, req->settings.view_axes->second, turned);
    CHECK((right - layer.view_project_right).norm() < 1e-5f);
    CHECK((up - layer.view_project_up).norm() < 1e-5f);

    StackFacts facts;
    facts.texture_has_color = false;
    CHECK_FALSE(layer_edit_refusal(1, {layer}, *req, &layer, after, facts).has_value());
}

TEST_CASE("an unwrap request is read for its shapes and bounds", "[McpTexture][orcamcp]")
{
    std::string error;
    const auto  read_unwrap = [&](const json& params) { return read_unwrap_request(params, error); };
    const auto  refused     = [&](const json& params) {
        REQUIRE_FALSE(read_unwrap(params).has_value());
        return error;
    };

    const auto req = read_unwrap({{"seam_angle_deg", 45}, {"auto_connect_islands", false},
                                  {"add_seams", {{7, 3}}}, {"remove_seams", {{1, 2}}},
                                  {"seam_paths", {{{"from", {0, 0, 0}}, {"to", {1, 2, 3}}}}}, {"cut_islands", {0, 2}}, {"unjoin", {1}},
                                  {"join", {{{"chart", 1}, {"to", 0}}, {{"chart", 2}}}},
                                  {"islands", {{{"chart", 0}, {"offset", {1, 2}}, {"rotation_deg", 90}, {"scale", 2}}}},
                                  {"uv_edits", {{{"vertex", 4}, {"uv", {0.5, 1.5}}}}}, {"clear_seams", true}, {"relayout", true}});
    REQUIRE(req.has_value());
    CHECK(*req->seam_angle_deg == 45.f);
    CHECK_FALSE(*req->auto_connect_islands);
    CHECK(req->add_seams == std::vector<std::pair<int, int>>{{3, 7}});
    CHECK(req->seam_paths[0].second == Vec3d(1, 2, 3));
    CHECK(req->cut_islands == std::vector<int>{0, 2});
    CHECK(req->join[0].second == 0);
    CHECK_FALSE(req->join[1].second.has_value());
    CHECK(req->islands[0].scale == 2.f);
    CHECK(req->uv_edits[0].second == Vec2f(0.5f, 1.5f));
    CHECK(req->clear_seams);
    CHECK(req->relayout);
    CHECK_FALSE(req->average_scale);

    CHECK(read_unwrap(json::object()).has_value());
    CHECK(refused({{"seam_angle_deg", 4}}).find("5 to 90") != std::string::npos);
    CHECK(refused({{"seam_angle_deg", 91}}).find("5 to 90") != std::string::npos);
    CHECK(refused({{"auto_connect_islands", 1}}).find("true or false") != std::string::npos);
    CHECK(refused({{"add_seams", {{1, 1}}}}).find("one vertex twice") != std::string::npos);
    CHECK(refused({{"add_seams", {{-1, 2}}}}).find("whole number") != std::string::npos);
    CHECK(refused({{"add_seams", {{1}}}}).find("pair") != std::string::npos);
    CHECK(refused({{"seam_paths", {{{"from", {0, 0, 0}}}}}}).find("from and to") != std::string::npos);
    CHECK(refused({{"cut_islands", {"a"}}}).find("cut_islands[0]") != std::string::npos);
    CHECK(refused({{"join", {{{"to", 1}}}}}).find("chart") != std::string::npos);
    CHECK(refused({{"islands", {{{"chart", 0}, {"scale", 0}}}}}).find("0.001 to 1000") != std::string::npos);
    CHECK(refused({{"islands", {{{"chart", 0}, {"size", 1}}}}}).find("no key \"size\"") != std::string::npos);
    CHECK(refused({{"uv_edits", {{{"vertex", 1}, {"uv", {1}}}}}}).find("2 numbers") != std::string::npos);
}

TEST_CASE("a layer and the stack's options are told", "[McpTexture][orcamcp]")
{
    TextureDisplacementOptions options;
    TextureDisplacementLayer   layer = layer_in(2);
    layer.name                       = "Bark";
    layer.path                       = "/res/Bark.png";
    layer.projection_method          = TextureProjectionMethod::ViewProjected;
    layer.view_project_projective    = true;
    MaskReport report;
    report.volume_id      = 1;
    report.name           = "cube";
    report.painted_facets = 4;
    report.area_ratio     = 1. / 3.;
    LayerTexture texture;
    texture.id     = "builtin/Bark.png";
    texture.source = "builtin";

    const json j = layer_json(layer, 0, true, texture, false, {report}, false, {layer}, options, Transform3d::Identity());
    CHECK(j["layer"] == 2);
    CHECK(j["base"] == true);
    CHECK(j["image"]["texture"] == "builtin/Bark.png");
    CHECK(j["projection"] == "view");
    CHECK(j["projection_frame_active"] == true);
    CHECK(j["view_axes"].contains("right"));
    CHECK(j["painted"] == true);
    CHECK_THAT(j["paint"][0]["coverage_percent"].get<double>(), WithinAbs(33.33, 1e-6));
    CHECK(j["color_capable"] == false);
    CHECK(j.contains("inactive_settings"));
    CHECK(j["warnings"].empty());

    LayerTexture project_only;
    CHECK(layer_json(layer, 0, true, project_only, std::nullopt, {}, false, {layer}, options, Transform3d::Identity())["image"]["source"] == "project");
    CHECK(layer_json(layer, 0, true, project_only, std::nullopt, {}, false, {layer}, options, Transform3d::Identity())["color_capable"].is_null());

    const json o = stack_options_json(options);
    CHECK(o["pipeline"] == "default");
    CHECK(o["resolution_mm"].is_null());
    CHECK(o["budget_k"].is_null());
    CHECK(o["smooth"]["enabled"] == false);
    CHECK(o["smooth"]["passes"] == 2);
    CHECK(o["smooth"]["ignore_outer_ring"] == true);
    CHECK_THAT(o["smooth"]["strength"].get<double>(), WithinAbs(30., 1e-6));
    CHECK(o["color_mix_count"] == 8);
    options.pipeline_v2        = false;
    options.v2_refine_mm       = 0.5f;
    options.v2_max_triangles_k = 0;
    const json c               = stack_options_json(options);
    CHECK(c["pipeline"] == "classic");
    CHECK(c["resolution_mm"] == 0.5);
    CHECK(c["budget_k"] == 0);

    SECTION("an unwrap")
    {
        PatchUnwrap unwrap;
        unwrap.chart_count    = 2;
        unwrap.chart_centroid = {Vec2f(1, 2), Vec2f(3, 4)};
        unwrap.uvs            = {Vec2f(0, 0), Vec2f(1, 1)};
        unwrap.source_vertex  = {5, 6};
        unwrap.vertex_chart   = {0, 1};
        layer.lscm_seam_edges = {{1, 2}};
        layer.islands         = {TextureIsland{}};
        layer.island_groups   = {7};
        const json u = unwrap_json(unwrap, layer, true);
        CHECK(u["chart_count"] == 2);
        CHECK(u["seams"] == json::array({json::array({1, 2})}));
        CHECK(u["islands"][0]["group"] == 7);
        CHECK(u["islands"][1]["group"] == 1); // its own group: its number
        CHECK(u["vertices"][1]["source_vertex"] == 6);
        CHECK_FALSE(unwrap_json(unwrap, layer, false).contains("vertices"));
    }
}

TEST_CASE("the library list names each texture by id and counts every source", "[McpTexture][orcamcp]")
{
    const std::vector<TextureLibraryEntry> lib = {entry("Bark", "/r/Bark.png", TextureSource::Shipped), entry("Mine", "/u/Mine.png", TextureSource::User),
                                                  entry("Mine 2", "/u/Mine 2.png", TextureSource::User),
                                                  entry("Grid", "/p/Grid.png", TextureSource::Plugin, "Pack A")};
    const std::vector<std::string>      ids    = texture_ids(lib);
    const std::vector<std::optional<bool>> colors = {false, true, std::nullopt, false};
    std::string                         error;

    const json all = library_list_json(lib, ids, colors, std::nullopt, error);
    CHECK(error.empty());
    CHECK(all["textures"].size() == 4);
    CHECK(all["textures"][0]["id"] == "builtin/Bark.png");
    CHECK(all["textures"][1]["color"] == true);
    CHECK(all["textures"][2]["color"].is_null());
    CHECK(all["textures"][3]["pack"] == "Pack A");
    CHECK(all["counts"] == json{{"builtin", 1}, {"user", 2}, {"pack", 1}});

    const json users = library_list_json(lib, ids, colors, std::string("user"), error);
    CHECK(users["textures"].size() == 2);
    CHECK(users["shown"] == 2);
    CHECK(users["counts"] == all["counts"]);

    library_list_json(lib, ids, colors, std::string("shipped"), error);
    CHECK(error.find("builtin, user or pack") != std::string::npos);
}

TEST_CASE("a stack is told in slot order with each part's paint", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    object->add_volume(TriangleMesh(its_make_cube(5., 5., 5.)), ModelVolumeType::MODEL_PART, false);
    ModelVolume* first  = object->volumes[0];
    ModelVolume* second = object->volumes[1];
    paint_facets(*second, 3, 6);

    std::vector<MaskInput> inputs;
    for (int i = 0; i < 2; ++i) {
        MaskInput input;
        input.volume_id = i;
        input.name      = "part " + std::to_string(i);
        input.mesh      = std::make_shared<TriangleMesh>(object->volumes[size_t(i)]->mesh());
        for (size_t slot = 0; slot < input.masks.size(); ++slot)
            input.masks[slot] = object->volumes[size_t(i)]->texture_displacement_facet(int(slot)).get_data();
        inputs.push_back(std::move(input));
    }
    (void) first;
    const auto reports = mask_reports(inputs);

    // Layers handed over out of order: slot 3 painted on the second part only, slot 1 painted nowhere.
    StackLayerFacts upper, lower;
    upper.layer = layer_in(3);
    lower.layer = layer_in(1);
    TextureDisplacementOptions options;

    SECTION("every part")
    {
        const StackAnswer answer = texture_stack_json({upper, lower}, reports, std::nullopt, false, options, Transform3d::Identity());
        const json&       layers = answer.json.at("layers");
        REQUIRE(layers.size() == 2);
        CHECK(layers[0]["layer"] == 1);
        CHECK(layers[0]["base"] == true);
        CHECK(layers[1]["layer"] == 3);
        CHECK(layers[1]["base"] == false);
        CHECK(layers[1]["position"] == 1);
        CHECK(layers[0]["painted"] == false);
        CHECK(layers[1]["painted"] == true);
        REQUIRE(layers[1]["paint"].size() == 2);
        CHECK(layers[1]["paint"][0]["painted_facets"] == 0);
        CHECK(layers[1]["paint"][1]["painted_facets"] == 6);
        CHECK(answer.json["needs_bake"] == true);
        CHECK(answer.json["free_layers"] == int(TEXTURE_DISPLACEMENT_MAX_LAYERS) - 2);
        CHECK(answer.json.contains("stack_options"));
        CHECK(answer.first_unpainted_layer == 1);
    }
    SECTION("one part's paint shows alone, but the bake and the next step read them all")
    {
        const StackAnswer answer = texture_stack_json({upper, lower}, reports, 0, false, options, Transform3d::Identity());
        const json&       paint  = answer.json["layers"][1]["paint"];
        REQUIRE(paint.size() == 1);
        CHECK(paint[0]["volume_id"] == 0);
        CHECK(answer.json["layers"][1]["painted"] == false);
        CHECK(answer.json["needs_bake"] == true);
    }
    SECTION("nothing painted needs no bake")
    {
        const StackAnswer answer = texture_stack_json({lower}, reports, std::nullopt, false, options, Transform3d::Identity());
        CHECK(answer.json["needs_bake"] == false);
        CHECK(answer.first_unpainted_layer == 1);
    }
    SECTION("no layers")
    {
        const StackAnswer answer = texture_stack_json({}, reports, std::nullopt, false, options, Transform3d::Identity());
        CHECK(answer.json["layers"].empty());
        CHECK(answer.json["free_layers"] == int(TEXTURE_DISPLACEMENT_MAX_LAYERS));
        CHECK_FALSE(answer.first_unpainted_layer.has_value());
    }
}

namespace {

// A cube whose six faces are charts of their own at the default seam angle, with the layer an unwrap edit starts from.
struct CubeUnwrap
{
    Model                    model;
    ModelObject*             object = nullptr;
    TextureDisplacementLayer layer;
    indexed_triangle_set     mesh;
    indexed_triangle_set     patch;
    PatchUnwrap              unwrap;

    CubeUnwrap()
    {
        object = cube_object(model);
        ModelVolume* part = object->volumes.front();
        paint_facets(*part, 0, 12);
        layer                   = layer_in(0);
        layer.projection_method = TextureProjectionMethod::LSCM;
        layer.auto_connect_islands = false;
        mesh   = part->mesh().its;
        patch  = unwrap_patch(part->mesh(), part->texture_displacement_facet(0).get_data(), Transform3d::Identity());
        unwrap = layer_unwrap(patch, layer);
    }

    std::optional<UnwrapEdit> edit(const TextureDisplacementLayer& from, const json& params, std::string& error) const
    {
        std::string read_error;
        const auto  request = read_unwrap_request(params, read_error);
        REQUIRE(request.has_value());
        return edit_unwrap(from, *request, mesh, patch, Transform3d::Identity(), [] { return false; }, error);
    }
    std::optional<UnwrapEdit> edit(const json& params, std::string& error) const { return edit(layer, params, error); }
    UnwrapEdit                ok(const TextureDisplacementLayer& from, const json& params) const
    {
        std::string error;
        auto        result = edit(from, params, error);
        INFO(error);
        REQUIRE(result.has_value());
        return *result;
    }
    UnwrapEdit ok(const json& params) const { return ok(layer, params); }
    std::string refused(const json& params) const
    {
        std::string error;
        REQUIRE_FALSE(edit(params, error).has_value());
        return error;
    }
};

} // namespace

TEST_CASE("an unwrap edit changes what it was asked to and names its undo step as the pane does", "[McpTexture][orcamcp]")
{
    CubeUnwrap cube;
    REQUIRE(cube.unwrap.chart_count == 6);

    SECTION("a call that asks for nothing changes nothing")
    {
        const UnwrapEdit none = cube.ok(json::object());
        CHECK_FALSE(none.changed);
        CHECK(none.snapshot.empty());
        CHECK(same_unwrap_state(none.layer, cube.layer));
    }
    SECTION("an island is placed by the fields given")
    {
        const UnwrapEdit moved = cube.ok({{"islands", {{{"chart", 1}, {"offset", {3, 4}}, {"scale", 2}}}}});
        CHECK(moved.changed);
        CHECK(moved.snapshot == "Move texture island");
        REQUIRE(moved.layer.islands.size() >= 2);
        CHECK(moved.layer.islands[1].offset == Vec2f(3, 4));
        CHECK(moved.layer.islands[1].scale == 2.f);
        CHECK(moved.layer.islands[1].rotation_deg == 0.f);
        CHECK_FALSE(moved.resolved);
        // The same placement again is no change.
        CHECK_FALSE(cube.ok(moved.layer, {{"islands", {{{"chart", 1}, {"offset", {3, 4}}}}}}).changed);
    }
    SECTION("a join places the chart edge to edge against its neighbour, as the pane's Join does")
    {
        const json u = unwrap_json(cube.unwrap, cube.layer, false);
        const int  neighbour = u["islands"][0]["neighbours"][0].get<int>();
        TextureIsland expected;
        REQUIRE(join_chart_placement(cube.unwrap, cube.layer.islands, 0, neighbour, expected));
        const UnwrapEdit joined = cube.ok({{"join", {{{"chart", 0}, {"to", neighbour}}}}});
        CHECK(joined.snapshot == "Join texture island");
        REQUIRE_FALSE(joined.layer.islands.empty());
        CHECK(joined.layer.islands[0].offset == expected.offset);
        CHECK(joined.layer.islands[0].rotation_deg == expected.rotation_deg);
        CHECK(island_group_of(joined.layer.island_groups, 0) == island_group_of(joined.layer.island_groups, neighbour));
        // Without `to`, the nearest neighbour the pane would pick.
        const auto target = join_target(cube.unwrap, cube.layer.islands, 0);
        REQUIRE(target.has_value());
        const UnwrapEdit nearest = cube.ok({{"join", {{{"chart", 0}}}}});
        CHECK(nearest.layer.islands[0].offset == target->second.offset);

        const UnwrapEdit apart = cube.ok(joined.layer, {{"unjoin", {0}}});
        CHECK(apart.snapshot == "Unjoin texture island");
        CHECK(apart.layer.islands[0].offset == Vec2f::Zero());
        CHECK(island_group_of(apart.layer.island_groups, 0) == 0);
    }
    SECTION("the scales are averaged")
    {
        TextureDisplacementLayer scaled = cube.layer;
        scaled.islands.resize(6);
        for (size_t i = 0; i < 6; ++i)
            scaled.islands[i].scale = float(1 + i % 3);
        const UnwrapEdit averaged = cube.ok(scaled, {{"average_scale", true}});
        CHECK(averaged.snapshot == "Average island scale");
        for (const TextureIsland& island : averaged.layer.islands)
            CHECK_THAT(island.scale, WithinAbs(2., 1e-5));
    }
    SECTION("a relayout lays the islands out as a net, and turning auto connect on does it at once")
    {
        const UnwrapEdit net = cube.ok({{"relayout", true}});
        CHECK(net.changed);
        CHECK(net.snapshot == "Texture displacement editing");
        CHECK(net.layer.islands.size() == 6);
        CHECK_FALSE(net.layer.auto_connect_islands);
        const UnwrapEdit connected = cube.ok({{"auto_connect_islands", true}});
        CHECK(connected.layer.auto_connect_islands);
        REQUIRE(connected.layer.islands.size() == net.layer.islands.size());
        for (size_t i = 0; i < net.layer.islands.size(); ++i) {
            CHECK(connected.layer.islands[i].offset == net.layer.islands[i].offset);
            CHECK(connected.layer.islands[i].rotation_deg == net.layer.islands[i].rotation_deg);
        }
    }
    SECTION("a seam path runs over mesh edges between the vertices nearest its ends")
    {
        const UnwrapEdit path = cube.ok({{"seam_paths", {{{"from", {0.1, 0.1, 0.1}}, {"to", {9.9, 9.9, 9.9}}}}}});
        CHECK(path.snapshot == "Mark texture seam path");
        CHECK(path.resolved);
        REQUIRE(path.snapped_paths.size() == 1);
        CHECK(cube.mesh.vertices[size_t(path.snapped_paths[0].first)].isApprox(Vec3f(0, 0, 0)));
        CHECK(cube.mesh.vertices[size_t(path.snapped_paths[0].second)].isApprox(Vec3f(10, 10, 10)));
        REQUIRE_FALSE(path.layer.lscm_seam_edges.empty());
        std::set<std::pair<int, int>> edges;
        for (const auto& tri : cube.mesh.indices)
            for (int k = 0; k < 3; ++k)
                edges.insert({std::min(tri[k], tri[(k + 1) % 3]), std::max(tri[k], tri[(k + 1) % 3])});
        for (const auto& seam : path.layer.lscm_seam_edges)
            CHECK(edges.count(seam) == 1);
    }
    SECTION("seams are added, removed and cleared; the unwrap is made anew")
    {
        const auto&               tri  = cube.mesh.indices[0];
        const std::pair<int, int> edge = {std::min(tri[0], tri[1]), std::max(tri[0], tri[1])};
        const UnwrapEdit marked = cube.ok({{"add_seams", {{edge.first, edge.second}}}});
        CHECK(marked.snapshot == "Mark texture seam");
        CHECK(marked.layer.lscm_seam_edges == std::vector<std::pair<int, int>>{edge});
        CHECK(marked.resolved);
        CHECK_FALSE(cube.ok(marked.layer, {{"add_seams", {{edge.first, edge.second}}}}).changed);
        CHECK(cube.ok(marked.layer, {{"remove_seams", {{edge.second, edge.first}}}}).layer.lscm_seam_edges.empty());
        const UnwrapEdit cleared = cube.ok(marked.layer, {{"clear_seams", true}});
        CHECK(cleared.snapshot == "Clear texture seams");
        CHECK(cleared.layer.lscm_seam_edges.empty());
    }
    SECTION("a chart is cut across")
    {
        const UnwrapEdit cut = cube.ok({{"cut_islands", {0}}});
        CHECK(cut.snapshot == "Cut texture island");
        CHECK_FALSE(cut.layer.lscm_seam_edges.empty());
        CHECK(cut.resolved);
        CHECK(cut.unwrap.chart_count != cube.unwrap.chart_count); // with a seam only seams cut: the seam angle no longer does
    }
    SECTION("a seam angle is a setting with no undo name, and makes the unwrap anew")
    {
        const UnwrapEdit angle = cube.ok({{"seam_angle_deg", 95 - 10}});
        CHECK(angle.layer.lscm_seam_angle_deg == 85.f);
        CHECK(angle.resolved);
        CHECK(angle.snapshot == "Texture displacement editing");
    }
    SECTION("vertex edits move an unwrapped vertex and are cleared")
    {
        const UnwrapEdit edited = cube.ok({{"uv_edits", {{{"vertex", 3}, {"uv", {5, 6}}}}}});
        CHECK(edited.snapshot == "Edit texture UV");
        REQUIRE(edited.layer.lscm_uv_overrides.size() == 1);
        CHECK(edited.layer.lscm_uv_overrides[0].first == lscm_uv_override_key(3));
        CHECK(edited.unwrap.uvs[3] == Vec2f(5, 6));
        CHECK_FALSE(cube.ok(edited.layer, {{"uv_edits", {{{"vertex", 3}, {"uv", {5, 6}}}}}}).changed);
        const UnwrapEdit cleared = cube.ok(edited.layer, {{"clear_uv_edits", true}});
        CHECK(cleared.snapshot == "Clear texture UV edits");
        CHECK(cleared.layer.lscm_uv_overrides.empty());
        CHECK(cleared.unwrap.uvs[3] != Vec2f(5, 6));
        // Clearing runs first, so a call that clears and edits ends with the new edit.
        const UnwrapEdit both = cube.ok(edited.layer, {{"clear_uv_edits", true}, {"uv_edits", {{{"vertex", 4}, {"uv", {1, 1}}}}}});
        REQUIRE(both.layer.lscm_uv_overrides.size() == 1);
        CHECK(both.layer.lscm_uv_overrides[0].first == lscm_uv_override_key(4));
    }
    SECTION("several kinds of edit share the generic undo name")
    {
        const UnwrapEdit both = cube.ok({{"islands", {{{"chart", 0}, {"offset", {1, 1}}}}}, {"uv_edits", {{{"vertex", 0}, {"uv", {1, 1}}}}}});
        CHECK(both.snapshot == "Texture displacement editing");
    }
}

TEST_CASE("an unwrap edit that cannot be done is refused before it changes anything", "[McpTexture][orcamcp]")
{
    CubeUnwrap cube;
    REQUIRE(cube.unwrap.chart_count == 6);
    const json u = unwrap_json(cube.unwrap, cube.layer, false);

    CHECK(cube.refused({{"islands", {{{"chart", 99}, {"scale", 2}}}}}).find("chart 99") != std::string::npos);
    CHECK(cube.refused({{"cut_islands", {6}}}).find("chart 6") != std::string::npos);
    CHECK(cube.refused({{"uv_edits", {{{"vertex", 100000}, {"uv", {0, 0}}}}}}).find("vertex 100000") != std::string::npos);
    CHECK(cube.refused({{"add_seams", {{0, 100}}}}).find("not a vertex") != std::string::npos);
    CHECK(cube.refused({{"seam_paths", {{{"from", {0, 0, 0}}, {"to", {0.1, 0, 0}}}}}}).find("apart") != std::string::npos);

    // Two charts that share no edge: the opposite faces of the cube.
    int apart = -1;
    for (int c = 1; c < 6; ++c) {
        bool neighbour = false;
        for (const json& n : u["islands"][0]["neighbours"])
            neighbour |= n.get<int>() == c;
        if (!neighbour)
            apart = c;
    }
    REQUIRE(apart > 0);
    CHECK(cube.refused({{"join", {{{"chart", 0}, {"to", apart}}}}}).find("share no mesh edge") != std::string::npos);
    CHECK(cube.refused({{"join", {{{"chart", 0}, {"to", 0}}}}}).find("share no mesh edge") != std::string::npos);

    // Two vertices of the mesh that no edge joins.
    std::set<std::pair<int, int>> edges;
    for (const auto& tri : cube.mesh.indices)
        for (int k = 0; k < 3; ++k)
            edges.insert({std::min(tri[k], tri[(k + 1) % 3]), std::max(tri[k], tri[(k + 1) % 3])});
    std::pair<int, int> not_edge{-1, -1};
    for (int a = 0; a < int(cube.mesh.vertices.size()) && not_edge.first < 0; ++a)
        for (int b = a + 1; b < int(cube.mesh.vertices.size()); ++b)
            if (!edges.count({a, b})) {
                not_edge = {a, b};
                break;
            }
    REQUIRE(not_edge.first >= 0);
    CHECK(cube.refused({{"add_seams", {{not_edge.first, not_edge.second}}}}).find("not joined by an edge") != std::string::npos);

    // Numbers name the unwrap before the call: a seam change in the same call renumbers it.
    CHECK(cube.refused({{"cut_islands", {0}}, {"islands", {{{"chart", 1}, {"scale", 2}}}}}).find("seam change first") != std::string::npos);
    CHECK(cube.refused({{"seam_angle_deg", 60}, {"uv_edits", {{{"vertex", 1}, {"uv", {0, 0}}}}}}).find("seam change first") != std::string::npos);

    // Nothing painted leaves nothing to unwrap.
    std::string error;
    std::string read_error;
    const auto  request = read_unwrap_request(json::object(), read_error);
    CHECK_FALSE(edit_unwrap(cube.layer, *request, cube.mesh, indexed_triangle_set{}, Transform3d::Identity(), [] { return false; }, error).has_value());
    CHECK(error.find("nothing to unwrap") != std::string::npos);
    // A stopped call changes nothing.
    CHECK_FALSE(edit_unwrap(cube.layer, *request, cube.mesh, cube.patch, Transform3d::Identity(), [] { return true; }, error).has_value());
    CHECK(error.find("stopped") != std::string::npos);
}

TEST_CASE("an unwrap read back is accepted and changes nothing", "[McpTexture][orcamcp]")
{
    CubeUnwrap cube;
    // A layer with a placement, a join, a seam and a vertex edit, as get_texture_unwrap would report it.
    TextureDisplacementLayer layer = cube.ok({{"relayout", true}}).layer;
    layer = cube.ok(layer, {{"islands", {{{"chart", 2}, {"offset", {1.5, 2.5}}, {"rotation_deg", 30}, {"scale", 1.5}}}}}).layer;
    layer = cube.ok(layer, {{"join", {{{"chart", 0}}}}}).layer;
    layer = cube.ok(layer, {{"uv_edits", {{{"vertex", 5}, {"uv", {2, 3}}}}}}).layer;
    const auto& tri       = cube.mesh.indices[0];
    layer.lscm_seam_edges = {{std::min(tri[0], tri[1]), std::max(tri[0], tri[1])}};

    const PatchUnwrap unwrap = layer_unwrap(cube.patch, layer);
    // The seam above is no seam of this unwrap's making; read the unwrap the layer's own seams give.
    const json reported = unwrap_json(unwrap, layer, true);
    REQUIRE(reported["islands"].size() == size_t(unwrap.chart_count));
    CHECK(reported["islands"][0].contains("area_mm2"));
    CHECK(reported["islands"][0].contains("bounding_box"));
    CHECK(reported["islands"][0].contains("neighbours"));
    CHECK(reported["uv_edits"].size() == 1);
    CHECK(reported["uv_edits"][0]["vertex"] == 5);

    json again = {{"islands", reported["islands"]}, {"uv_edits", reported["uv_edits"]}, {"add_seams", reported["seams"]},
                  {"seam_angle_deg", reported["seam_angle_deg"]}, {"auto_connect_islands", reported["auto_connect_islands"]}};
    const UnwrapEdit same = cube.ok(layer, again);
    CHECK_FALSE(same.changed);
    CHECK(same.snapshot.empty());
}

TEST_CASE("an unwrap lists at most max_unwrap_vertices vertices", "[McpTexture][orcamcp]")
{
    PatchUnwrap unwrap;
    unwrap.chart_count   = 1;
    const size_t count   = max_unwrap_vertices + 5;
    unwrap.uvs.assign(count, Vec2f::Zero());
    unwrap.source_vertex.assign(count, 0);
    unwrap.vertex_chart.assign(count, 0);
    unwrap.chart_centroid = {Vec2f::Zero()};
    const json capped = unwrap_json(unwrap, TextureDisplacementLayer{}, true);
    CHECK(capped["vertices"].size() == max_unwrap_vertices);
    CHECK(capped["truncated"] == true);
    CHECK(unwrap_json(unwrap, TextureDisplacementLayer{}, false)["islands"].size() == 1);
}

TEST_CASE("unjoining a group's lowest chart leaves the others grouped without it", "[McpTexture][orcamcp]")
{
    std::vector<int> groups = {0, 0, 2, 0};
    unjoin_island(groups, 0, 4);
    CHECK(island_group_of(groups, 0) == 0);
    CHECK(island_group_of(groups, 1) == 1);
    CHECK(island_group_of(groups, 3) == 1);
    CHECK(island_group_of(groups, 1) != island_group_of(groups, 0));

    groups = {0, 0, 2, 0};
    unjoin_island(groups, 3, 4); // a member that is not the name
    CHECK(island_group_of(groups, 3) == 3);
    CHECK(island_group_of(groups, 1) == island_group_of(groups, 0));

    groups = {};
    unjoin_island(groups, 1, 3); // not grouped at all
    CHECK(island_group_of(groups, 1) == 1);
    CHECK(island_group_of(groups, 0) == 0);

    CubeUnwrap cube;
    const json u = unwrap_json(cube.unwrap, cube.layer, false);
    const int  a = u["islands"][0]["neighbours"][0].get<int>();
    const int  b = u["islands"][0]["neighbours"][1].get<int>();
    TextureDisplacementLayer layer = cube.ok({{"join", {{{"chart", a}, {"to", 0}}}}}).layer;
    layer = cube.ok(layer, {{"join", {{{"chart", b}, {"to", 0}}}}}).layer;
    REQUIRE(island_group_of(layer.island_groups, a) == island_group_of(layer.island_groups, 0));
    const UnwrapEdit apart = cube.ok(layer, {{"unjoin", {0}}});
    CHECK(island_group_of(apart.layer.island_groups, a) != island_group_of(apart.layer.island_groups, 0));
    CHECK(island_group_of(apart.layer.island_groups, a) == island_group_of(apart.layer.island_groups, b));
}

TEST_CASE("moving one island of a join group moves the group, as dragging does", "[McpTexture][orcamcp]")
{
    CubeUnwrap cube;
    const json u = unwrap_json(cube.unwrap, cube.layer, false);
    const int  n = u["islands"][0]["neighbours"][0].get<int>();
    const TextureDisplacementLayer joined = cube.ok({{"join", {{{"chart", n}, {"to", 0}}}}}).layer;
    const Vec2f was_n = joined.islands[size_t(n)].offset;
    const Vec2f was_0 = joined.islands[0].offset;

    const UnwrapEdit moved = cube.ok(joined, {{"islands", {{{"chart", 0}, {"offset", {was_0.x() + 5, was_0.y() + 7}}}}}});
    CHECK(moved.layer.islands[0].offset == Vec2f(was_0.x() + 5, was_0.y() + 7));
    CHECK_THAT(moved.layer.islands[size_t(n)].offset.x(), WithinAbs(was_n.x() + 5, 1e-4));
    CHECK_THAT(moved.layer.islands[size_t(n)].offset.y(), WithinAbs(was_n.y() + 7, 1e-4));

    // A chart the call names keeps the placement it is given, and a rotation or scale moves no one else.
    const UnwrapEdit both = cube.ok(joined, {{"islands", {{{"chart", 0}, {"offset", {1, 1}}}, {{"chart", n}, {"offset", {9, 9}}}}}});
    CHECK(both.layer.islands[size_t(n)].offset == Vec2f(9, 9));
    const UnwrapEdit turned = cube.ok(joined, {{"islands", {{{"chart", 0}, {"rotation_deg", 45}}}}});
    CHECK(turned.layer.islands[size_t(n)].offset == was_n);
}

TEST_CASE("chart numbers are tied to an unwrap id", "[McpTexture][orcamcp]")
{
    CubeUnwrap cube;
    const json  u  = unwrap_json(cube.unwrap, cube.layer, false);
    const std::string id = u["unwrap_id"];
    REQUIRE(id.size() == 16);
    CHECK(cube.ok({{"unwrap_id", id}, {"islands", {{{"chart", 1}, {"scale", 2}}}}}).changed);
    const std::string stale = cube.refused({{"unwrap_id", "0000000000000000"}, {"islands", {{{"chart", 1}, {"scale", 2}}}}});
    CHECK(stale.find("read get_texture_unwrap again") != std::string::npos);
    CHECK(cube.refused({{"unwrap_id", "0000000000000000"}, {"cut_islands", {1}}}).find("unwrap_id") != std::string::npos);
    CHECK(cube.refused({{"unwrap_id", "0000000000000000"}, {"uv_edits", {{{"vertex", 1}, {"uv", {0, 0}}}}}}).find("unwrap_id") != std::string::npos);
    // Without it, today's behaviour; and a settings-only call has no numbers to check.
    CHECK(cube.ok({{"islands", {{{"chart", 1}, {"scale", 2}}}}}).changed);
    CHECK_FALSE(cube.ok({{"unwrap_id", "0000000000000000"}}).changed);

    // A placement leaves the id alone; a seam, or a vertex edit, changes it.
    const UnwrapEdit placed = cube.ok({{"islands", {{{"chart", 1}, {"scale", 2}}}}});
    CHECK(unwrap_json(placed.unwrap, placed.layer, false)["unwrap_id"] == id);
    const UnwrapEdit cut = cube.ok({{"cut_islands", {0}}});
    CHECK(unwrap_json(cut.unwrap, cut.layer, false)["unwrap_id"] != id);
    const UnwrapEdit edited = cube.ok({{"uv_edits", {{{"vertex", 1}, {"uv", {4, 4}}}}}});
    CHECK(unwrap_json(edited.unwrap, edited.layer, false)["unwrap_id"] != id);
}

TEST_CASE("a seam angle changed while seams stand renumbers nothing", "[McpTexture][orcamcp]")
{
    CubeUnwrap cube;
    const auto&               tri = cube.mesh.indices[0];
    TextureDisplacementLayer  seamed = cube.layer;
    seamed.lscm_seam_edges = {{std::min(tri[0], tri[1]), std::max(tri[0], tri[1])}};
    const UnwrapEdit angle = cube.ok(seamed, {{"seam_angle_deg", 80}});
    CHECK(angle.changed);
    CHECK_FALSE(angle.resolved);
    // So island edits may go with it.
    CHECK(cube.ok(seamed, {{"seam_angle_deg", 80}, {"islands", {{{"chart", 0}, {"scale", 2}}}}}).changed);
    // Without seams the angle does renumber.
    CHECK(cube.ok({{"seam_angle_deg", 80}}).resolved);
}

TEST_CASE("every neighbour a cube's charts report is a face sharing an edge", "[McpTexture][orcamcp]")
{
    CubeUnwrap cube;
    const json u = unwrap_json(cube.unwrap, cube.layer, false);
    for (const json& island : u["islands"]) {
        CHECK(island["neighbours"].size() == 4);
        for (const json& n : island["neighbours"])
            CHECK(n != island["chart"]);
    }
}

TEST_CASE("an unwrap lists at most max_unwrap_islands islands", "[McpTexture][orcamcp]")
{
    PatchUnwrap unwrap;
    unwrap.chart_count    = int(max_unwrap_islands) + 3;
    unwrap.chart_centroid.assign(size_t(unwrap.chart_count), Vec2f::Zero());
    const json capped = unwrap_json(unwrap, TextureDisplacementLayer{}, false);
    CHECK(capped["islands"].size() == max_unwrap_islands);
    CHECK(capped["islands_truncated"] == true);
}

TEST_CASE("a whole get_texture_unwrap answer is taken back by edit_unwrap and changes nothing", "[McpTexture][orcamcp]")
{
    CubeUnwrap cube;
    TextureDisplacementLayer layer = cube.ok({{"relayout", true}}).layer;
    const int neighbour = unwrap_json(cube.unwrap, cube.layer, false)["islands"][0]["neighbours"][0].get<int>();
    layer = cube.ok(layer, {{"join", {{{"chart", neighbour}, {"to", 0}}}}}).layer;
    const auto& tri = cube.mesh.indices[0];
    layer.lscm_seam_edges = {{std::min(tri[0], tri[1]), std::max(tri[0], tri[1])}};
    layer = cube.ok(layer, {{"seam_angle_deg", 50}}).layer;
    const PatchUnwrap unwrap = layer_unwrap(cube.patch, layer);
    const json reported = unwrap_json(unwrap, layer, true);

    // Every field of the answer the request also takes, the seams as add_seams.
    const json schema = OrcaMCPServer::registered_tools().at("set_texture_unwrap").input_schema;
    json       arguments = json::object();
    for (const auto& [key, value] : reported.items())
        if (schema.at("properties").contains(key))
            arguments[key] = value;
    arguments["add_seams"] = reported["seams"];
    REQUIRE(arguments.contains("islands"));
    REQUIRE(arguments.contains("unwrap_id"));
    const UnwrapEdit same = cube.ok(layer, arguments);
    CHECK_FALSE(same.changed);
    CHECK(same.snapshot.empty());
    CHECK(same.unwrap.chart_count == unwrap.chart_count);
}

TEST_CASE("only an imported texture can be deleted", "[McpTexture][orcamcp]")
{
    CHECK_FALSE(user_texture_delete_refusal(entry("Mine", "/u/Mine.png", TextureSource::User), "user/Mine.png").has_value());
    const auto builtin = user_texture_delete_refusal(entry("Bark", "/r/Bark.png", TextureSource::Shipped), "builtin/Bark.png");
    REQUIRE(builtin.has_value());
    CHECK(builtin->find("read-only") != std::string::npos);
    CHECK(builtin->find("builtin/Bark.png") != std::string::npos);
    CHECK(user_texture_delete_refusal(entry("Grid", "/p/Grid.png", TextureSource::Plugin, "A"), "pack/A/Grid.png").has_value());
}

TEST_CASE("a layer's position in slot order and the insertion index that moves it there", "[McpTexture][orcamcp]")
{
    const std::vector<TextureDisplacementLayer> stack = {layer_in(5), layer_in(1), layer_in(3)};
    CHECK(layer_position(stack, 1) == 0);
    CHECK(layer_position(stack, 3) == 1);
    CHECK(layer_position(stack, 5) == 2);
    CHECK_FALSE(layer_position(stack, 0).has_value());
    // Down the list the insertion index counts the layer's own place; up it does not.
    CHECK(move_insertion_index(0, 2) == 3);
    CHECK(move_insertion_index(2, 0) == 0);
    CHECK(move_insertion_index(1, 1) == 1);
}

TEST_CASE("a projection frame carries its own view", "[McpTexture][orcamcp]")
{
    const json frame = {{"view", "top"}, {"width_mm", 20}, {"height_mm", 20}};
    CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", frame}, {"view_direction", "front"}}).find("own view") != std::string::npos);
    CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", frame}, {"projection", "triplanar"}}).find("projection must be view") !=
          std::string::npos);
    std::string error;
    CHECK(read({{"action", "edit"}, {"layer", 0}, {"projection_frame", frame}, {"projection", "view"}}, error).has_value());
}

TEST_CASE("every bound of a layer setting is the registered schema's minimum and maximum", "[McpTexture][orcamcp]")
{
    const json schema = OrcaMCPServer::registered_tools().at("set_texture_displacement").input_schema;
    const json& properties = schema.at("properties");
    for (const SettingBound& bound : layer_setting_bounds()) {
        INFO(bound.key);
        const bool adjust = std::string(bound.key) == "brightness" || std::string(bound.key) == "contrast" || std::string(bound.key) == "sharpness" ||
                            std::string(bound.key) == "blur" || std::string(bound.key) == "hue_deg";
        const json& property = adjust ? properties.at("adjust_image").at("properties").at(bound.key) : properties.at(bound.key);
        CHECK_THAT(property.at("minimum").get<double>(), WithinAbs(bound.min, 1e-9));
        CHECK_THAT(property.at("maximum").get<double>(), WithinAbs(bound.max, 1e-9));
    }
}

TEST_CASE("a PNG's size is read from its header", "[McpTexture][orcamcp]")
{
    unsigned char head[33] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0x20, 0, 0, 0, 0x10, 0, 8, 0};
    const auto    size     = png_dimensions(head, sizeof head);
    REQUIRE(size.has_value());
    CHECK(size->first == 8192);
    CHECK(size->second == 4096);
    CHECK(size->first <= max_texture_side_px);
    head[1] = 'X';
    CHECK_FALSE(png_dimensions(head, sizeof head).has_value());
    CHECK_FALSE(png_dimensions(head, 10).has_value());
}

TEST_CASE("an RGB image of grey pixels is the grayscale texture the library would store", "[McpTexture][orcamcp]")
{
    DecodedHeightTexture grey;
    grey.width  = 2;
    grey.height = 1;
    grey.pixels = {100, 101};
    grey.rgb    = {100, 100, 100, 102, 102, 102};
    REQUIRE(grey.has_color());
    drop_grey_colour(grey);
    CHECK_FALSE(grey.has_color());
    CHECK(grey.pixels == std::vector<std::uint8_t>{100, 102});

    DecodedHeightTexture coloured = grey;
    coloured.pixels               = {100, 101};
    coloured.rgb                  = {100, 100, 100, 102, 103, 102};
    drop_grey_colour(coloured);
    CHECK(coloured.has_color());
    CHECK(coloured.pixels == std::vector<std::uint8_t>{100, 101});
}


// ---- The bake and the mesh preparation ----

TEST_CASE("a bake request reads its options and refuses what the panel cannot set", "[McpTexture][orcamcp]")
{
    std::string error;
    const auto  none = read_bake_request({{"object_id", 0}}, error);
    REQUIRE(none.has_value());
    CHECK_FALSE(none->classic.has_value());
    CHECK_FALSE(none->include_preview);

    const auto full = read_bake_request({{"object_id", 0}, {"pipeline", "classic"}, {"resolution_mm", 0.5}, {"budget_k", 300}, {"classic_budget_k", 900},
                                         {"displace_border", false}, {"smooth", {{"enabled", true}, {"strength", 40}, {"passes", 3}, {"ignore_outer_ring", false}}},
                                         {"color_mix", false}, {"color_mix_count", 4}, {"color_denoise", 1}, {"include_preview", true}},
                                        error);
    REQUIRE(full.has_value());
    CHECK(*full->classic);
    CHECK_THAT(*full->resolution_mm, WithinAbs(0.5, 1e-6));
    CHECK(*full->budget_k == 300);
    CHECK(classic_bake_budget_k(*full) == 900);
    CHECK(*full->smooth.passes == 3);
    CHECK(full->include_preview);

    const auto refused = [&](const json& params) {
        std::string why;
        CHECK_FALSE(read_bake_request(params, why).has_value());
        return why;
    };
    CHECK(refused({{"pipeline", "fast"}}).find("default or classic") != std::string::npos);
    CHECK(refused({{"resolution_mm", 5}}).find("0.02 to 2") != std::string::npos);
    CHECK(refused({{"resolution_mm", "fine"}}).find("must be a number") != std::string::npos);
    CHECK(refused({{"budget_k", 5000}}).find("0 to 4000") != std::string::npos);
    CHECK(refused({{"budget_k", 1.5}}).find("integer") != std::string::npos);
    CHECK(refused({{"classic_budget_k", 5}}).find("10 to 2000") != std::string::npos);
    CHECK(refused({{"pipeline", "default"}, {"classic_budget_k", 500}}).find("classic pipeline") != std::string::npos);
    CHECK(refused({{"smooth", {{"strength", 150}}}}).find("1 to 100") != std::string::npos);
    CHECK(refused({{"smooth", {{"width", 1}}}}).find("has no key \"width\"") != std::string::npos);
    CHECK(refused({{"color_mix_count", 40}}).find("1 to 32") != std::string::npos);
}

TEST_CASE("the stack options a response reports are accepted back as bake options", "[McpTexture][orcamcp]")
{
    TextureDisplacementOptions stored;
    stored.pipeline_v2        = false;
    stored.v2_refine_mm       = 0.4f;
    stored.v2_max_triangles_k = 0;
    stored.smooth_enabled     = true;
    for (const bool automatic : {false, true}) {
        if (automatic) {
            stored.v2_refine_mm       = 0.f;
            stored.v2_max_triangles_k = -1;
        }
        json arguments = stack_options_json(stored);
        arguments["object_id"] = 0;
        std::string error;
        const auto  request = read_bake_request(arguments, error);
        REQUIRE(request.has_value());
        const TextureDisplacementOptions used = bake_options(TextureDisplacementOptions{}, *request);
        CHECK_FALSE(bake_options_differ(used, stored));
    }
}

TEST_CASE("a bake pins the Standard options unless the call says otherwise", "[McpTexture][orcamcp]")
{
    TextureDisplacementOptions stored;
    stored.displace_border = false;
    stored.smooth_enabled  = true;
    stored.v2_refine_mm    = 0.7f;
    std::string error;
    const auto  request = read_bake_request({{"object_id", 0}}, error);
    REQUIRE(request.has_value());
    const TextureDisplacementOptions used = bake_options(stored, *request);
    CHECK(used.displace_border);
    CHECK_FALSE(used.smooth_enabled);
    CHECK_THAT(used.v2_refine_mm, WithinAbs(0.7, 1e-6)); // what the call leaves out stays

    const auto automatic = read_bake_request({{"object_id", 0}, {"resolution_mm", nullptr}, {"budget_k", -1}}, error);
    REQUIRE(automatic.has_value());
    const TextureDisplacementOptions auto_used = bake_options(stored, *automatic);
    CHECK_THAT(auto_used.v2_refine_mm, WithinAbs(0., 1e-9));
    CHECK(auto_used.v2_max_triangles_k == -1);
    CHECK(bake_options_differ(used, auto_used));
    TextureDisplacementOptions panel_state = used;
    panel_state.active_slot = 3;
    panel_state.panel_mode  = 1;
    CHECK_FALSE(bake_options_differ(used, panel_state));
}

TEST_CASE("every bound of the bake and the preparation is the registered schema's minimum and maximum", "[McpTexture][orcamcp]")
{
    const auto& tools = OrcaMCPServer::registered_tools();
    const json  bake = tools.at("bake_texture_displacement").input_schema.at("properties");
    for (const char* key : {"classic_budget_k", "color_mix_count", "color_denoise"}) {
        INFO(key);
        CHECK_THAT(bake.at(key).at("minimum").get<double>(), WithinAbs(bake_setting_bound(key)->min, 1e-9));
        CHECK_THAT(bake.at(key).at("maximum").get<double>(), WithinAbs(bake_setting_bound(key)->max, 1e-9));
    }
    for (const char* key : {"strength", "passes"}) {
        INFO(key);
        CHECK_THAT(bake.at("smooth").at("properties").at(key).at("minimum").get<double>(), WithinAbs(bake_setting_bound(key)->min, 1e-9));
        CHECK_THAT(bake.at("smooth").at("properties").at(key).at("maximum").get<double>(), WithinAbs(bake_setting_bound(key)->max, 1e-9));
    }
    const json prepare = tools.at("prepare_texture_mesh").input_schema.at("properties");
    for (const SettingBound& bound : prepare_setting_bounds()) {
        INFO(bound.key);
        CHECK_THAT(prepare.at(bound.key).at("minimum").get<double>(), WithinAbs(bound.min, 1e-9));
        CHECK_THAT(prepare.at(bound.key).at("maximum").get<double>(), WithinAbs(bound.max, 1e-9));
    }
}

TEST_CASE("a prepare request is held to its operation's arguments", "[McpTexture][orcamcp]")
{
    std::string error;
    const auto  remesh = read_prepare_request({{"object_id", 0}, {"operation", "remesh"}, {"target_edge_mm", 0.8}, {"keep_sharp_edges", false}}, error);
    REQUIRE(remesh.has_value());
    const TextureDisplacementPrepareParams remesh_params = remesh_prepare_params(*remesh, 2.f);
    CHECK_THAT(remesh_params.remesh_edge_mm, WithinAbs(0.8, 1e-6));
    CHECK_THAT(remesh_params.remesh_sharp_deg, WithinAbs(0., 1e-9)); // sharp edges not kept
    const auto defaults = read_prepare_request({{"object_id", 0}, {"operation", "remesh"}}, error);
    CHECK_THAT(remesh_prepare_params(*defaults, 2.f).remesh_edge_mm, WithinAbs(2., 1e-6));
    CHECK_THAT(remesh_prepare_params(*defaults, 2.f).remesh_sharp_deg, WithinAbs(40., 1e-6));

    const auto adaptive = read_prepare_request({{"object_id", 0}, {"operation", "subdivide"}, {"follow_texture", true}, {"budget_k", 100}}, error);
    REQUIRE(adaptive.has_value());
    CHECK_FALSE(adaptive->uniform());
    const TextureDisplacementPrepareParams sub = subdivide_prepare_params(*adaptive, 0.5f);
    CHECK_THAT(sub.subdiv_target_mm, WithinAbs(0.5, 1e-6));
    CHECK_THAT(sub.subdiv_border_mm, WithinAbs(0.4, 1e-6));
    CHECK_THAT(sub.subdiv_color_edge_mm, WithinAbs(0.3, 1e-6));
    CHECK(sub.subdiv_feature);
    CHECK(sub.subdiv_added_triangles == 100000);

    const auto uniform = read_prepare_request({{"object_id", 0}, {"operation", "subdivide"}, {"adaptive", false}, {"steps", 2}}, error);
    REQUIRE(uniform.has_value());
    CHECK(uniform->uniform());

    const auto refused = [&](const json& params) {
        std::string why;
        CHECK_FALSE(read_prepare_request(params, why).has_value());
        return why;
    };
    CHECK(refused({{"object_id", 0}}).find("operation is required") != std::string::npos);
    CHECK(refused({{"operation", "tidy"}}).find("remesh, subdivide or smooth") != std::string::npos);
    CHECK(refused({{"operation", "remesh"}, {"steps", 2}}).find("steps does not apply to remesh") != std::string::npos);
    CHECK(refused({{"operation", "subdivide"}, {"target_edge_mm", 1}}).find("does not apply to subdivide") != std::string::npos);
    CHECK(refused({{"operation", "subdivide"}, {"adaptive", false}, {"steps", 2}, {"max_edge_mm", 1}}).find("uniform subdivide") != std::string::npos);
    CHECK(refused({{"operation", "subdivide"}, {"adaptive", false}}).find("needs steps") != std::string::npos);
    CHECK(refused({{"operation", "subdivide"}, {"steps", 2}}).find("steps does not apply") != std::string::npos);
    CHECK(refused({{"operation", "remesh"}, {"target_edge_mm", 50}}).find("0.1 to 20") != std::string::npos);
    CHECK(refused({{"operation", "smooth"}, {"strength", 0}}).find("1 to 100") != std::string::npos);
    const auto smooth = read_prepare_request({{"operation", "smooth"}, {"strength", 50}, {"passes", 4}, {"ignore_outer_ring", false}}, error);
    REQUIRE(smooth.has_value());
    const TextureDisplacementOptions smoothed = smooth_options(TextureDisplacementOptions{}, smooth->smooth);
    CHECK_THAT(smoothed.smooth_strength, WithinAbs(0.5, 1e-6));
    CHECK(smoothed.smooth_iterations == 4);
    CHECK_FALSE(smoothed.smooth_skip_border);
}

TEST_CASE("the panel's seeds for the edge lengths come from the mesh's mean edge", "[McpTexture][orcamcp]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    // A cube's triangles have edges of 10, 10 and 14.14: the mean is 11.38.
    CHECK_THAT(default_remesh_edge_mm(cube), WithinAbs(11.38, 0.01));
    CHECK_THAT(default_subdivide_edge_mm(cube), WithinAbs(5.69, 0.01));
    CHECK_THAT(default_remesh_edge_mm(indexed_triangle_set{}), WithinAbs(1., 1e-9));
    const indexed_triangle_set tiny = its_make_cube(0.02, 0.02, 0.02);
    CHECK_THAT(default_remesh_edge_mm(tiny), WithinAbs(0.1, 1e-6));
}

TEST_CASE("a uniform subdivision is refused above the facet cap", "[McpTexture][orcamcp]")
{
    CHECK_FALSE(uniform_subdivision_refusal(100000, 1).has_value()); // 400 thousand
    const auto refusal = uniform_subdivision_refusal(100000, 2);     // 1.6 million
    REQUIRE(refusal.has_value());
    CHECK(refusal->find("adaptive") != std::string::npos);
}

TEST_CASE("the bake's answers: parts, budget and mixed slots", "[McpTexture][orcamcp]")
{
    BakePartReport part;
    part.volume_id = 1; part.name = "cube"; part.facets_before = 12; part.facets_after = 90000; part.baked = true; part.colored = true;
    const json row = bake_part_json(part);
    CHECK(row["facets_after"] == 90000);
    CHECK(row["colored"] == true);

    CHECK(bake_budget_json(0, 0).is_null());
    CHECK(bake_budget_json(900000, 750000)["kept"] == 750000);
    CHECK(bake_budget_warning(0, 0).empty());
    const std::string budget_text = bake_budget_warning(560640, 200768);
    CHECK(budget_text.find("200768") != std::string::npos);
    CHECK(budget_text.find("560640") != std::string::npos);
    CHECK(budget_text.find("prepare_texture_mesh") != std::string::npos);

    const json mixed = bake_mixed_filaments_json({1, 4}, 4);
    REQUIRE(mixed.size() == 2);
    CHECK(mixed[0]["slot"] == 2);
    CHECK(mixed[0]["created"] == false);
    CHECK(mixed[1]["slot"] == 5);
    CHECK(mixed[1]["created"] == true);

    CHECK(bake_undo_warning(1).find("delete_mixed_filament") != std::string::npos);
    CHECK(bake_timed_out_message(105., true).find("ORCAMCP_TIMEOUT") != std::string::npos);
    CHECK(bake_timed_out_message(105., true).find("classic_budget_k") != std::string::npos);
    CHECK(bake_timed_out_message(105., false).find("classic_budget_k") == std::string::npos);
    CHECK(bake_no_displacement_message().find("paint was kept") != std::string::npos);
}

TEST_CASE("a smooth is refused above the facet cap, and unused bake settings are reported", "[McpTexture][orcamcp]")
{
    CHECK_FALSE(smooth_refusal(SMOOTH_MAX_FACETS).has_value());
    CHECK(smooth_refusal(SMOOTH_MAX_FACETS + 1).has_value());

    std::string error;
    const auto  classic_with_default_settings = read_bake_request({{"pipeline", "classic"}, {"resolution_mm", 0.3}, {"budget_k", nullptr}}, error);
    REQUIRE(classic_with_default_settings.has_value());
    const json inactive = bake_inactive_settings(*classic_with_default_settings, true);
    CHECK(inactive.contains("resolution_mm"));
    CHECK(inactive.contains("budget_k"));
    CHECK_FALSE(inactive.contains("classic_budget_k"));
    CHECK(bake_inactive_settings(*classic_with_default_settings, false).empty());

    const auto budget = read_bake_request({{"classic_budget_k", 400}}, error);
    REQUIRE(budget.has_value());
    CHECK(bake_inactive_settings(*budget, false).contains("classic_budget_k"));
    CHECK(bake_inactive_settings(*budget, true).empty());
}

namespace {
// A height map of `period`-pixel stripes (half dark, half light), diagonal when `slope` is non-zero.
std::vector<uint8_t> stripe_image(int width, int height, int period, int slope)
{
    std::vector<uint8_t> px(size_t(width) * size_t(height));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            px[size_t(y) * size_t(width) + size_t(x)] = ((x + slope * y) % period) < period / 2 ? 30 : 220;
    return px;
}
} // namespace

TEST_CASE("a bake's resolution is judged against the texture's feature size on the part", "[McpTexture][orcamcp]")
{
    // Wide diagonal stripes, 32 px a stripe, 512x256 over a model UV that spans about 6 mm per 32 px: the feature is ~6 mm, so
    // 0.5 mm carries it (the false alarm of the texel-size rule, which asked for 0.05 mm).
    const float mm_per_px = 6.f / 32.f;
    const float stripes   = texture_feature_mm(stripe_image(512, 256, 64, 1), 512, 256, mm_per_px, mm_per_px);
    CHECK(stripes > 4.f);
    const ResolutionAdvice wide = resolution_advice_from(stripes, 0.5f);
    CHECK(wide.recommended_mm > 1.f);
    CHECK_FALSE(wide.aliasing);
    CHECK(resolution_alias_warning(wide).empty());

    // A knurl: 0.19 mm period at 6 mm per 512-px tile (~0.0117 mm a pixel): runs of ~8 px, half a period of 0.1 mm, baked at 0.7 mm.
    const float texel = 6.f / 512.f;
    const float knurl = texture_feature_mm(stripe_image(512, 512, 16, 0), 512, 512, texel, texel);
    CHECK(knurl == Catch::Approx(8 * texel).margin(0.02f));
    const ResolutionAdvice fine = resolution_advice_from(knurl, 0.7f);
    CHECK(fine.recommended_mm == Catch::Approx(0.05f)); // half of 0.09 mm, at the floor
    CHECK(fine.aliasing);
    CHECK(resolution_alias_warning(fine).find("0.7") != std::string::npos);
    CHECK(recommended_resolution_json(fine) == json(fine.recommended_mm));

    // The floor is on the recommendation only: a feature the floor would round up still warns above it.
    CHECK(resolution_advice_from(0.02f, 0.06f).aliasing);
    CHECK_FALSE(resolution_advice_from(0.02f, 0.05f).aliasing);

    // Nothing to size by: a flat image, a single edge.
    CHECK(texture_feature_mm(std::vector<uint8_t>(64 * 64, 100), 64, 64, 1.f, 1.f) == 0.f);
    const ResolutionAdvice none = resolution_advice_from(0.f, 5.f);
    CHECK(none.recommended_mm == 0.f);
    CHECK_FALSE(none.aliasing);
    CHECK(recommended_resolution_json(none).is_null());
}

TEST_CASE("a model's UV channel gives the millimetres a UV unit spans", "[McpTexture][orcamcp]")
{
    // A 10 x 10 mm square of two triangles mapped onto the unit UV square: 10 mm a unit; at half the UV size, 20 mm.
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
    mesh.indices  = {{0, 1, 2}, {0, 2, 3}};
    const std::vector<Vec2f> full = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
    CHECK(model_uv_mm_per_unit(mesh, Transform3d::Identity(), full) == Catch::Approx(10.f));
    std::vector<Vec2f> half = full;
    for (Vec2f& uv : half)
        uv *= 0.5f;
    CHECK(model_uv_mm_per_unit(mesh, Transform3d::Identity(), half) == Catch::Approx(20.f));
    CHECK(model_uv_mm_per_unit(mesh, Transform3d::Identity(), {}) == 0.f);
}
