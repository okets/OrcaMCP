// src/slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.cpp
#include "OrcaMCPTextureDisplacement.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPNextSteps.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include "slic3r/GUI/TextureDisplacementObject.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

using nlohmann::json;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string join_names(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& n : names)
        out += (out.empty() ? "" : ", ") + n;
    return out;
}

std::string number_text(double v)
{
    std::ostringstream os;
    os << v;
    return os.str();
}

template<class E> std::optional<E> named(const std::vector<std::string>& names, const std::string& name)
{
    const auto it = std::find(names.begin(), names.end(), lower(name));
    if (it == names.end())
        return std::nullopt;
    return E(int(it - names.begin()));
}

std::string file_name_of(const TextureLibraryEntry& entry)
{
    return std::filesystem::path(entry.path).filename().string();
}

} // namespace

// ---- Names ----

const std::vector<std::string>& projection_names()
{
    static const std::vector<std::string> names{"triplanar", "cylindrical", "spherical", "unwrap", "view", "model_uv"};
    return names;
}
std::optional<TextureProjectionMethod> projection_named(const std::string& name)
{
    return named<TextureProjectionMethod>(projection_names(), name);
}
std::string projection_name(TextureProjectionMethod m) { return projection_names().at(size_t(m)); }

const std::vector<std::string>& blend_names()
{
    static const std::vector<std::string> names{"add", "override", "subtract", "multiply", "divide"};
    return names;
}
namespace {
// The panel's combo order is not the enum's (Add, Subtract, Multiply, Divide, Override).
constexpr TextureBlendMode blend_modes[] = {TextureBlendMode::Add, TextureBlendMode::Override, TextureBlendMode::Subtract,
                                            TextureBlendMode::Multiply, TextureBlendMode::Divide};
} // namespace
std::optional<TextureBlendMode> blend_named(const std::string& name)
{
    const auto it = std::find(blend_names().begin(), blend_names().end(), lower(name));
    if (it == blend_names().end())
        return std::nullopt;
    return blend_modes[it - blend_names().begin()];
}
std::string blend_name(TextureBlendMode m)
{
    for (size_t i = 0; i < std::size(blend_modes); ++i)
        if (blend_modes[i] == m)
            return blend_names()[i];
    return blend_names().front();
}

const std::vector<std::string>& tile_method_names()
{
    static const std::vector<std::string> names{"repeat", "mirrored_repeat"};
    return names;
}
std::optional<TextureTileMethod> tile_method_named(const std::string& name) { return named<TextureTileMethod>(tile_method_names(), name); }
std::string tile_method_name(TextureTileMethod m) { return tile_method_names().at(size_t(m)); }

const std::vector<std::string>& view_direction_names()
{
    static const std::vector<std::string> names{"current", "iso", "top", "bottom", "front", "back", "left", "right"};
    return names;
}

const char* texture_source_name(TextureSource source)
{
    switch (source) {
    case TextureSource::Shipped: return "builtin";
    case TextureSource::User: return "user";
    case TextureSource::Plugin: return "pack";
    }
    return "builtin";
}

// ---- The library, by id ----

std::vector<std::string> texture_ids(const std::vector<TextureLibraryEntry>& entries)
{
    std::vector<std::string>   ids;
    std::map<std::string, int> seen;
    ids.reserve(entries.size());
    for (const TextureLibraryEntry& e : entries) {
        std::string id = texture_source_name(e.source);
        id += "/";
        if (e.source == TextureSource::Plugin)
            id += e.pack_name + "/";
        id += file_name_of(e);
        const int n = ++seen[id];
        ids.push_back(n == 1 ? id : id + " #" + std::to_string(n));
    }
    return ids;
}

TextureLookup find_texture(const std::vector<TextureLibraryEntry>& entries, const std::vector<std::string>& ids, const std::string& query)
{
    TextureLookup out;
    for (size_t i = 0; i < ids.size() && i < entries.size(); ++i)
        if (ids[i] == query) {
            out.index = int(i);
            return out;
        }
    std::vector<size_t> matches;
    const std::string   q = lower(query);
    for (size_t i = 0; i < ids.size() && i < entries.size(); ++i)
        if (lower(ids[i]) == q)
            matches.push_back(i);
    if (matches.empty())
        for (size_t i = 0; i < entries.size(); ++i)
            if (lower(entries[i].name) == q)
                matches.push_back(i);
    if (matches.size() == 1) {
        out.index = int(matches.front());
        return out;
    }
    if (matches.empty()) {
        out.error = "No texture \"" + query + "\": get_texture_library lists the texture ids";
        return out;
    }
    std::vector<std::string> candidates;
    for (size_t i : matches)
        candidates.push_back(ids[i]);
    out.error = "\"" + query + "\" names more than one texture: send one id of " + join_names(candidates);
    return out;
}

std::optional<int> library_index_of(const std::vector<TextureLibraryEntry>& entries, const TextureDisplacementLayer& layer)
{
    if (layer.path.empty())
        return std::nullopt;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].path == layer.path)
            return int(i);
    return std::nullopt;
}

std::optional<bool> png_has_color(const unsigned char* bytes, std::size_t size)
{
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (bytes == nullptr || size < 26 || !std::equal(signature, signature + 8, bytes) || std::string(bytes + 12, bytes + 16) != "IHDR")
        return std::nullopt;
    const unsigned bit_depth = bytes[24], color_type = bytes[25];
    if (bit_depth != 8)
        return std::nullopt;
    if (color_type == 0)
        return false;
    if (color_type == 2 || color_type == 6)
        return true;
    return std::nullopt;
}

std::optional<bool> png_color_from_file(const std::string& path)
{
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in)
        return std::nullopt;
    unsigned char head[33] = {};
    in.read(reinterpret_cast<char*>(head), sizeof head);
    return png_has_color(head, size_t(in.gcount()));
}

std::optional<std::pair<int, int>> png_dimensions(const unsigned char* bytes, std::size_t size)
{
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (bytes == nullptr || size < 24 || !std::equal(signature, signature + 8, bytes) || std::string(bytes + 12, bytes + 16) != "IHDR")
        return std::nullopt;
    const auto be32 = [&](size_t at) { return (std::uint32_t(bytes[at]) << 24) | (std::uint32_t(bytes[at + 1]) << 16) | (std::uint32_t(bytes[at + 2]) << 8) | bytes[at + 3]; };
    const std::uint32_t w = be32(16), h = be32(20);
    if (w == 0 || h == 0 || w > 0x7fffffffu || h > 0x7fffffffu)
        return std::nullopt;
    return std::make_pair(int(w), int(h));
}

std::optional<std::pair<int, int>> png_dimensions_from_file(const std::string& path)
{
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in)
        return std::nullopt;
    unsigned char head[33] = {};
    in.read(reinterpret_cast<char*>(head), sizeof head);
    return png_dimensions(head, size_t(in.gcount()));
}

void drop_grey_colour(DecodedHeightTexture& t)
{
    if (!t.has_color())
        return;
    for (size_t i = 0; i < t.pixels.size(); ++i)
        if (t.rgb[i * 3] != t.rgb[i * 3 + 1] || t.rgb[i * 3 + 1] != t.rgb[i * 3 + 2])
            return;
    for (size_t i = 0; i < t.pixels.size(); ++i)
        t.pixels[i] = t.rgb[i * 3];
    t.rgb.clear();
}

json library_entry_json(const TextureLibraryEntry& entry, const std::string& id, std::optional<bool> color)
{
    json out = {{"id", id}, {"name", entry.name}, {"source", texture_source_name(entry.source)}};
    if (entry.source == TextureSource::Plugin)
        out["pack"] = entry.pack_name;
    out["file"]  = file_name_of(entry);
    out["path"]  = entry.path;
    out["color"] = color ? json(*color) : json(nullptr);
    return out;
}

json library_list_json(const std::vector<TextureLibraryEntry>& entries, const std::vector<std::string>& ids,
                       const std::vector<std::optional<bool>>& colors, const std::optional<std::string>& source, std::string& error)
{
    error.clear();
    if (source && *source != "builtin" && *source != "user" && *source != "pack") {
        error = "source must be builtin, user or pack, not \"" + *source + "\"";
        return json();
    }
    json counts   = {{"builtin", 0}, {"user", 0}, {"pack", 0}};
    json textures = json::array();
    for (size_t i = 0; i < entries.size() && i < ids.size(); ++i) {
        const std::string name = texture_source_name(entries[i].source);
        counts[name]           = counts[name].get<int>() + 1;
        if (!source || *source == name)
            textures.push_back(library_entry_json(entries[i], ids[i], i < colors.size() ? colors[i] : std::nullopt));
    }
    return {{"textures", textures}, {"counts", counts}, {"shown", textures.size()}};
}

std::optional<std::string> user_texture_delete_refusal(const TextureLibraryEntry& entry, const std::string& id)
{
    if (entry.source == TextureSource::User)
        return std::nullopt;
    return "Only imported textures can be deleted; built-in and pack textures are read-only (" + id + " is " +
           (entry.source == TextureSource::Plugin ? "a pack texture" : "a built-in texture") + ")";
}

namespace {

void fnv(std::uint64_t& h, const std::uint8_t* data, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
}

} // namespace

TextureIdentity texture_identity(const DecodedHeightTexture& t)
{
    TextureIdentity id;
    id.width  = t.width;
    id.height = t.height;
    id.hash   = 1469598103934665603ull;
    const int dims[2] = {t.width, t.height};
    fnv(id.hash, reinterpret_cast<const std::uint8_t*>(dims), sizeof dims);
    fnv(id.hash, t.pixels.data(), t.pixels.size());
    // A colour image differs from its grayscale twin: the app decodes the one as colour (color: true is
    // allowed on it), the other not.
    const std::uint8_t has_color = t.has_color() ? 1 : 0;
    fnv(id.hash, &has_color, 1);
    if (t.has_color())
        fnv(id.hash, t.rgb.data(), t.rgb.size());
    return id;
}

bool same_texture(const DecodedHeightTexture& a, const DecodedHeightTexture& b)
{
    if (a.width != b.width || a.height != b.height || a.pixels != b.pixels)
        return false;
    return a.has_color() == b.has_color() && (!a.has_color() || a.rgb == b.rgb);
}

// ---- One call's layer edit ----

bool LayerSettings::empty() const
{
    return !depth_mm && !tile_size_mm && !rotation_deg && !midlevel && !smoothing && !edge_fade_amount && !offset && !invert &&
           !edge_fade && !tile && !color && !tile_method && !projection && !blend && !view_direction && !view_axes &&
           !projection_frame && !clear_projection_frame && !adjust_image && !color_mix && !color_mix_count && !color_denoise;
}

const std::vector<SettingBound>& layer_setting_bounds()
{
    static const std::vector<SettingBound> bounds = {
        {"depth_mm", 0.01, 10., "mm", "Depth"},
        {"tile_size_mm", 0.2, 200., "mm", "Tile size"},
        {"rotation_deg", 0., 360., "degrees", "Rotation"},
        {"midlevel", 0., 10., "", "Midlevel"},
        {"smoothing", 0., 1., "", "Smoothing"},
        {"edge_fade_amount", 0.02, 1., "", "Edge fade"},
        {"color_mix_count", 1., 32., "", "Mixed colors"},
        {"color_denoise", 0., 6., "", "Denoise"},
        {"brightness", -1., 1., "", "Brightness"},
        {"contrast", -1., 1., "", "Contrast"},
        {"sharpness", 0., 1., "", "Sharpness"},
        {"blur", 0., 1., "", "Blur"},
        {"hue_deg", -180., 180., "degrees", "Hue"},
    };
    return bounds;
}

const SettingBound* setting_bound(const std::string& key)
{
    for (const SettingBound& b : layer_setting_bounds())
        if (key == b.key)
            return &b;
    return nullptr;
}

namespace {

constexpr int MAX_SLOT = int(TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1;

struct Reader
{
    const json&  obj;
    std::string& error;

    bool has(const char* key) const { return obj.contains(key); }

    bool fail(const std::string& message)
    {
        if (error.empty())
            error = message;
        return false;
    }

    bool number(const char* key, double& out, const char* context = "")
    {
        const json& v = obj.at(key);
        if (!v.is_number() || !std::isfinite(v.get<double>()))
            return fail(std::string(context) + key + " must be a number");
        out = v.get<double>();
        return true;
    }

    // A number held to layer_setting_bounds().
    bool bounded(const char* key, std::optional<float>& out)
    {
        if (!has(key))
            return true;
        double v = 0;
        if (!number(key, v))
            return false;
        const SettingBound* b = setting_bound(key);
        if (b != nullptr && (v < b->min || v > b->max))
            return fail(std::string(key) + " must be from " + number_text(b->min) + " to " + number_text(b->max) +
                        (*b->unit ? std::string(" ") + b->unit : std::string()) + " (the panel's " + b->panel_label + "), not " +
                        number_text(v));
        out = float(v);
        return true;
    }

    bool bounded_int(const char* key, std::optional<int>& out)
    {
        if (!has(key))
            return true;
        if (!obj.at(key).is_number_integer())
            return fail(std::string(key) + " must be an integer");
        const long long v = obj.at(key).get<long long>();
        const SettingBound* b = setting_bound(key);
        if (b != nullptr && (double(v) < b->min || double(v) > b->max))
            return fail(std::string(key) + " must be from " + number_text(b->min) + " to " + number_text(b->max) + " (the panel's " +
                        b->panel_label + "), not " + std::to_string(v));
        out = int(v);
        return true;
    }

    bool boolean(const char* key, std::optional<bool>& out)
    {
        if (!has(key))
            return true;
        if (!obj.at(key).is_boolean())
            return fail(std::string(key) + " must be true or false");
        out = obj.at(key).get<bool>();
        return true;
    }

    bool string(const char* key, std::optional<std::string>& out)
    {
        if (!has(key))
            return true;
        if (!obj.at(key).is_string())
            return fail(std::string(key) + " must be a string");
        out = obj.at(key).get<std::string>();
        return true;
    }

    template<class E, class F> bool choice(const char* key, const std::vector<std::string>& names, F parse, std::optional<E>& out)
    {
        std::optional<std::string> text;
        if (!string(key, text))
            return false;
        if (!text)
            return true;
        const auto value = parse(*text);
        if (!value)
            return fail(std::string(key) + " must be one of " + join_names(names) + ", not \"" + *text + "\"");
        out = *value;
        return true;
    }
};

bool read_vec(const json& v, const std::string& what, size_t n, std::vector<double>& out, std::string& error)
{
    if (!v.is_array() || v.size() != n) {
        error = what + " must be an array of " + std::to_string(n) + " numbers";
        return false;
    }
    out.clear();
    for (const json& e : v) {
        if (!e.is_number() || !std::isfinite(e.get<double>())) {
            error = what + " must be an array of " + std::to_string(n) + " finite numbers";
            return false;
        }
        out.push_back(e.get<double>());
    }
    return true;
}

bool read_vec3(const json& v, const std::string& what, Vec3d& out, std::string& error)
{
    std::vector<double> c;
    if (!read_vec(v, what, 3, c, error))
        return false;
    out = Vec3d(c[0], c[1], c[2]);
    return true;
}

// Two unit axes that are neither zero nor parallel.
bool axes_usable(const Vec3d& right, const Vec3d& up)
{
    if (right.norm() < 1e-9 || up.norm() < 1e-9)
        return false;
    return right.normalized().cross(up.normalized()).norm() > 1e-6;
}

bool read_axes(const json& obj, const std::string& what, Vec3d& right, Vec3d& up, std::string& error)
{
    if (!obj.contains("right") || !obj.contains("up")) {
        error = what + " needs both right and up";
        return false;
    }
    if (!read_vec3(obj.at("right"), what + " right", right, error) || !read_vec3(obj.at("up"), what + " up", up, error))
        return false;
    if (!axes_usable(right, up)) {
        error = what + ": right and up must be non-zero and not parallel";
        return false;
    }
    return true;
}

bool read_frame(const json& v, ProjectionFrameRequest& frame, std::string& error)
{
    static const std::set<std::string> keys = {"view", "right", "up", "center", "width_mm", "height_mm"};
    if (!v.is_object()) {
        error = "projection_frame must be an object, or false to clear it";
        return false;
    }
    for (auto it = v.begin(); it != v.end(); ++it)
        if (!keys.count(it.key())) {
            error = "projection_frame has no key \"" + it.key() + "\". Its keys: view, right, up, center, width_mm, height_mm";
            return false;
        }
    Reader r{v, error};
    std::optional<std::string> view;
    if (!r.string("view", view))
        return false;
    const bool has_axes = v.contains("right") || v.contains("up");
    if (view && has_axes) {
        error = "projection_frame takes a view or right and up, not both";
        return false;
    }
    if (!view && !has_axes) {
        error = "projection_frame needs a view (a named view or current) or right and up";
        return false;
    }
    if (view) {
        const auto& names = view_direction_names();
        if (std::find(names.begin(), names.end(), lower(*view)) == names.end()) {
            error = "projection_frame view must be one of " + join_names(names) + ", not \"" + *view + "\"";
            return false;
        }
        frame.view = lower(*view);
    } else {
        Vec3d right, up;
        if (!read_axes(v, "projection_frame", right, up, error))
            return false;
        frame.right = right;
        frame.up    = up;
    }
    if (v.contains("center") && !read_vec3(v.at("center"), "projection_frame center", frame.center, error))
        return false;
    for (const char* key : {"width_mm", "height_mm"}) {
        if (!v.contains(key)) {
            error = std::string("projection_frame needs ") + key;
            return false;
        }
        double d = 0;
        if (!r.number(key, d, "projection_frame "))
            return false;
        if (!(d > 0.)) {
            error = std::string("projection_frame ") + key + " must be greater than 0 mm";
            return false;
        }
        (std::string(key) == "width_mm" ? frame.width_mm : frame.height_mm) = d;
    }
    return true;
}

bool read_adjust(const json& v, ImageAdjustRequest& adjust, std::string& error)
{
    static const std::set<std::string> keys = {"brightness", "contrast", "sharpness", "blur", "hue_deg"};
    if (!v.is_object()) {
        error = "adjust_image must be an object";
        return false;
    }
    for (auto it = v.begin(); it != v.end(); ++it)
        if (!keys.count(it.key())) {
            error = "adjust_image has no key \"" + it.key() + "\". Its keys: brightness, contrast, sharpness, blur, hue_deg";
            return false;
        }
    Reader r{v, error};
    if (!r.bounded("brightness", adjust.brightness) || !r.bounded("contrast", adjust.contrast) ||
        !r.bounded("sharpness", adjust.sharpness) || !r.bounded("blur", adjust.blur) || !r.bounded("hue_deg", adjust.hue_deg))
        return false;
    if (!adjust.brightness && !adjust.contrast && !adjust.sharpness && !adjust.blur && !adjust.hue_deg) {
        error = "adjust_image needs at least one of brightness, contrast, sharpness, blur, hue_deg";
        return false;
    }
    return true;
}

// Every argument that edits a layer or the stack: remove and move take none of them.
const char* const layer_edit_keys[] = {"texture", "file_path", "depth_mm", "tile_size_mm", "rotation_deg", "midlevel", "smoothing",
                                       "edge_fade_amount", "offset", "invert", "edge_fade", "tile", "color", "color_mix", "tile_method",
                                       "projection", "blend", "view_direction", "view_axes", "projection_frame", "adjust_image",
                                       "color_mix_count", "color_denoise"};

} // namespace

std::optional<LayerRequest> read_layer_request(const json& params, std::string& error)
{
    error.clear();
    if (!params.is_object()) {
        error = "arguments must be an object";
        return std::nullopt;
    }
    Reader       r{params, error};
    LayerRequest req;

    std::optional<std::string> action;
    if (!r.string("action", action))
        return std::nullopt;
    if (!action) {
        error = "action is required: add, edit, remove or move";
        return std::nullopt;
    }
    if (*action == "add") req.action = LayerAction::add;
    else if (*action == "edit") req.action = LayerAction::edit;
    else if (*action == "remove") req.action = LayerAction::remove;
    else if (*action == "move") req.action = LayerAction::move;
    else {
        error = "action must be add, edit, remove or move, not \"" + *action + "\"";
        return std::nullopt;
    }

    if (r.has("layer")) {
        if (!params.at("layer").is_number_integer()) {
            error = "layer must be an integer slot from 0 to " + std::to_string(MAX_SLOT);
            return std::nullopt;
        }
        const long long slot = params.at("layer").get<long long>();
        if (slot < 0 || slot > MAX_SLOT) {
            error = "layer must be a slot from 0 to " + std::to_string(MAX_SLOT) + ", not " + std::to_string(slot);
            return std::nullopt;
        }
        req.layer = int(slot);
    }
    if (r.has("to_position")) {
        if (!params.at("to_position").is_number_integer() || params.at("to_position").get<long long>() < 0) {
            error = "to_position must be an integer from 0 (the position in slot order)";
            return std::nullopt;
        }
        req.to_position = int(std::min<long long>(params.at("to_position").get<long long>(), 1000));
    }
    if (req.action == LayerAction::add && req.layer) {
        error = "add takes the first free slot, as the panel does: leave layer out";
        return std::nullopt;
    }
    if (req.action != LayerAction::add && !req.layer) {
        error = "action " + *action + " needs layer, the layer's slot (get_texture_displacement lists them)";
        return std::nullopt;
    }
    if (req.action == LayerAction::move && !req.to_position) {
        error = "action move needs to_position, the 0-based position in slot order";
        return std::nullopt;
    }
    if (req.action != LayerAction::move && req.to_position) {
        error = "to_position goes with action move only";
        return std::nullopt;
    }
    if (req.action == LayerAction::remove || req.action == LayerAction::move)
        for (const char* key : layer_edit_keys)
            if (r.has(key)) {
                error = "action " + *action + " takes no " + key;
                return std::nullopt;
            }

    LayerSettings& s = req.settings;
    if (!r.string("texture", req.texture) || !r.string("file_path", req.file_path))
        return std::nullopt;
    if (req.texture && req.file_path) {
        error = "texture and file_path are two ways to pick the image: send one";
        return std::nullopt;
    }
    if (!r.bounded("depth_mm", s.depth_mm) || !r.bounded("tile_size_mm", s.tile_size_mm) || !r.bounded("rotation_deg", s.rotation_deg) ||
        !r.bounded("midlevel", s.midlevel) || !r.bounded("smoothing", s.smoothing) || !r.bounded("edge_fade_amount", s.edge_fade_amount))
        return std::nullopt;
    if (r.has("offset")) {
        std::vector<double> c;
        if (!read_vec(params.at("offset"), "offset", 2, c, error))
            return std::nullopt;
        s.offset = Vec2f(float(c[0]), float(c[1]));
    }
    if (!r.boolean("invert", s.invert) || !r.boolean("edge_fade", s.edge_fade) || !r.boolean("tile", s.tile) ||
        !r.boolean("color", s.color) || !r.boolean("color_mix", s.color_mix))
        return std::nullopt;
    if (!r.choice("tile_method", tile_method_names(), tile_method_named, s.tile_method) ||
        !r.choice("projection", projection_names(), projection_named, s.projection) ||
        !r.choice("blend", blend_names(), blend_named, s.blend))
        return std::nullopt;

    if (!r.string("view_direction", s.view_direction))
        return std::nullopt;
    if (s.view_direction) {
        const auto& names = view_direction_names();
        s.view_direction  = lower(*s.view_direction);
        if (std::find(names.begin(), names.end(), *s.view_direction) == names.end()) {
            error = "view_direction must be one of " + join_names(names) + ", not \"" + *s.view_direction + "\"";
            return std::nullopt;
        }
    }
    if (r.has("view_axes")) {
        if (s.view_direction) {
            error = "view_direction and view_axes both say which way to project: send one";
            return std::nullopt;
        }
        const json& v = params.at("view_axes");
        if (!v.is_object()) {
            error = "view_axes must be an object {right, up}";
            return std::nullopt;
        }
        for (auto it = v.begin(); it != v.end(); ++it)
            if (it.key() != "right" && it.key() != "up") {
                error = "view_axes has no key \"" + it.key() + "\". Its keys: right, up";
                return std::nullopt;
            }
        Vec3d right, up;
        if (!read_axes(v, "view_axes", right, up, error))
            return std::nullopt;
        s.view_axes = std::make_pair(right, up);
    }
    if (r.has("projection_frame")) {
        const json& v = params.at("projection_frame");
        if (v.is_boolean()) {
            if (v.get<bool>()) {
                error = "projection_frame: true is not a frame; send {view or right/up, center, width_mm, height_mm}, or false to clear it";
                return std::nullopt;
            }
            s.clear_projection_frame = true;
        } else {
            if (s.view_direction || s.view_axes) {
                error = "projection_frame carries its own view: leave view_direction and view_axes out";
                return std::nullopt;
            }
            if (s.projection && *s.projection != TextureProjectionMethod::ViewProjected) {
                error = "projection_frame projects from a view: projection must be view, or left out";
                return std::nullopt;
            }
            ProjectionFrameRequest frame;
            if (!read_frame(v, frame, error))
                return std::nullopt;
            s.projection_frame = frame;
        }
    }
    if (r.has("adjust_image")) {
        ImageAdjustRequest adjust;
        if (!read_adjust(params.at("adjust_image"), adjust, error))
            return std::nullopt;
        s.adjust_image = adjust;
    }
    if (!r.bounded_int("color_mix_count", s.color_mix_count) || !r.bounded_int("color_denoise", s.color_denoise))
        return std::nullopt;
    return req;
}

std::optional<int> layer_position(const std::vector<TextureDisplacementLayer>& stack, int slot)
{
    std::vector<int> slots;
    for (const TextureDisplacementLayer& l : stack)
        slots.push_back(l.slot);
    std::sort(slots.begin(), slots.end());
    const auto it = std::find(slots.begin(), slots.end(), slot);
    if (it == slots.end())
        return std::nullopt;
    return int(it - slots.begin());
}

int move_insertion_index(int from, int to) { return to > from ? to + 1 : to; }

std::optional<int> first_free_slot(const std::vector<TextureDisplacementLayer>& stack)
{
    std::array<bool, TEXTURE_DISPLACEMENT_MAX_LAYERS> used{};
    for (const TextureDisplacementLayer& l : stack)
        if (l.slot >= 0 && size_t(l.slot) < used.size())
            used[size_t(l.slot)] = true;
    for (size_t i = 0; i < used.size(); ++i)
        if (!used[i])
            return int(i);
    return std::nullopt;
}

TextureDisplacementLayer new_layer(int slot, bool owner_has_uv)
{
    TextureDisplacementLayer layer;
    layer.slot = slot;
    if (owner_has_uv)
        layer.projection_method = TextureProjectionMethod::ModelUV;
    return layer;
}

std::optional<std::string> layer_edit_refusal(int object_id, const std::vector<TextureDisplacementLayer>& stack, const LayerRequest& req,
                                              const TextureDisplacementLayer* before, const TextureDisplacementLayer& after,
                                              const StackFacts& facts)
{
    const std::string object = "object " + std::to_string(object_id);
    if (req.action == LayerAction::add) {
        if (!first_free_slot(stack))
            return object + " already holds " + std::to_string(TEXTURE_DISPLACEMENT_MAX_LAYERS) +
                   " texture layers, the most there are: remove one with action remove first";
    } else {
        const bool found = std::any_of(stack.begin(), stack.end(), [&](const TextureDisplacementLayer& l) { return l.slot == *req.layer; });
        if (!found) {
            std::string slots;
            for (const TextureDisplacementLayer& l : stack)
                slots += (slots.empty() ? "" : ", ") + std::to_string(l.slot);
            return object + " has no texture layer in slot " + std::to_string(*req.layer) +
                   (slots.empty() ? ": it has no layers, add one with action add" : ": its layers are in slots " + slots);
        }
    }
    if (req.action == LayerAction::remove)
        return std::nullopt;
    if (req.action == LayerAction::move) {
        if (*req.to_position >= int(stack.size()))
            return "to_position " + std::to_string(*req.to_position) + " is past the last position, " + std::to_string(stack.size() - 1) +
                   " (positions count the layers in slot order from 0)";
        return std::nullopt;
    }
    const LayerSettings& s = req.settings;
    if (s.color.value_or(false) && !facts.texture_has_color && !(before != nullptr && before->color_enabled))
        return "The layer's texture has no colours, so it cannot colour the print: pick a colour texture (get_texture_library lists them) or leave color out";
    if (s.projection && *s.projection == TextureProjectionMethod::ModelUV && !facts.owner_has_uv &&
        !(before != nullptr && before->projection_method == TextureProjectionMethod::ModelUV))
        return object + "'s first part has no UV channel, so projection model_uv has nothing to map by (only a textured OBJ brings one)";
    if (s.color_mix_count && *s.color_mix_count != facts.color_mix_count) {
        const int most = std::max(1, 32 - facts.physical_filaments);
        if (*s.color_mix_count > most)
            return "color_mix_count " + std::to_string(*s.color_mix_count) + " is more than the " + std::to_string(most) +
                   " mixed colors the project has room for (32 filament slots less its " + std::to_string(facts.physical_filaments) +
                   " physical ones)";
    }
    if ((s.view_direction || s.view_axes) && after.projection_method != TextureProjectionMethod::ViewProjected)
        return "view_direction and view_axes aim a view projection: set projection view as well, or leave them out";
    if (s.adjust_image && after.empty())
        return "The layer has no image to adjust: give it a texture first";
    return std::nullopt;
}

namespace {

template<class T> bool assign(T& field, const std::optional<T>& value)
{
    if (!value || field == *value)
        return false;
    field = *value;
    return true;
}

} // namespace

bool apply_layer_settings(TextureDisplacementLayer& l, const LayerSettings& s)
{
    bool changed = false;
    if (s.clear_projection_frame && l.view_project_projective) {
        l.view_project_projective = false;
        l.tile_enabled            = true;
        changed                   = true;
    }
    changed |= assign(l.depth_mm, s.depth_mm);
    changed |= assign(l.tiling_scale, s.tile_size_mm);
    changed |= assign(l.rotation_deg, s.rotation_deg);
    changed |= assign(l.midlevel, s.midlevel);
    changed |= assign(l.smoothing, s.smoothing);
    changed |= assign(l.edge_smoothing_amount, s.edge_fade_amount);
    changed |= assign(l.offset, s.offset);
    changed |= assign(l.invert, s.invert);
    changed |= assign(l.edge_smoothing, s.edge_fade);
    changed |= assign(l.tile_enabled, s.tile);
    changed |= assign(l.color_enabled, s.color);
    changed |= assign(l.tile_method, s.tile_method);
    changed |= assign(l.projection_method, s.projection);
    changed |= assign(l.blend_mode, s.blend);
    return changed;
}

bool apply_stack_settings(TextureDisplacementOptions& o, const LayerSettings& s)
{
    bool changed = false;
    changed |= assign(o.color_mix_enabled, s.color_mix);
    changed |= assign(o.color_mix_count, s.color_mix_count);
    changed |= assign(o.color_despeckle, s.color_denoise);
    return changed;
}

TextureAdjustments adjustments_of(const ImageAdjustRequest& r)
{
    TextureAdjustments a;
    a.brightness = r.brightness.value_or(0.f);
    a.contrast   = r.contrast.value_or(0.f);
    a.sharpness  = r.sharpness.value_or(0.f);
    a.blur       = r.blur.value_or(0.f);
    a.hue_deg    = r.hue_deg.value_or(0.f);
    return a;
}

std::pair<Vec3f, Vec3f> view_projector_axes(const Vec3d& right_world, const Vec3d& up_world, const Transform3d& volume_to_world)
{
    const Matrix3d world_to_local = volume_to_world.matrix().block<3, 3>(0, 0).inverse();
    const Vec3d    right          = world_to_local * right_world;
    const Vec3d    up             = world_to_local * up_world;
    // Unit axes: the projected planar coordinate stays in mm, so the tile size keeps meaning mm.
    return {right.norm() > 1e-9 ? Vec3f(right.normalized().cast<float>()) : Vec3f::UnitX(),
            up.norm() > 1e-9 ? Vec3f(up.normalized().cast<float>()) : Vec3f::UnitY()};
}

std::array<float, 12> orthographic_frame_matrix(const Vec3d& right_world, const Vec3d& up_world, const Vec3d& center_world, double width_mm,
                                                double height_mm, const Transform3d& volume_to_world)
{
    const Vec3d right = right_world.normalized();
    const Vec3d up    = (up_world - right * right.dot(up_world)).normalized();
    const Vec3d down  = -up;
    const Vec3d corner = center_world - right * (width_mm * 0.5) + up * (height_mm * 0.5);

    const Matrix3d A = volume_to_world.matrix().block<3, 3>(0, 0);
    const Vec3d    t = volume_to_world.matrix().block<3, 1>(0, 3);
    const Vec3d    row_u = A.transpose() * right / width_mm, row_v = A.transpose() * down / height_mm;
    const double   off_u = right.dot(t - corner) / width_mm, off_v = down.dot(t - corner) / height_mm;

    std::array<float, 12> m{};
    for (int i = 0; i < 3; ++i) {
        m[size_t(i)]     = float(row_u[i]);
        m[size_t(4 + i)] = float(row_v[i]);
    }
    m[3]  = float(off_u);
    m[7]  = float(off_v);
    m[11] = 1.f;
    return m;
}

json inactive_settings(const std::vector<TextureDisplacementLayer>& stack, const TextureDisplacementLayer& layer,
                       const TextureDisplacementOptions& options, bool owner_has_uv, bool texture_has_color)
{
    (void) options;
    json out = json::object();
    const bool framed = layer.projection_method == TextureProjectionMethod::ViewProjected && layer.view_project_projective;
    if (layer.projection_method == TextureProjectionMethod::ModelUV) {
        if (!owner_has_uv)
            out["projection"] = "the first part has no UV channel, so the layer maps like triplanar";
        else
            out["tile_size_mm"] = "the model's own UV layout is the placement, so there is no tile size";
    }
    if (framed)
        for (const char* key : {"tile_size_mm", "rotation_deg", "offset"})
            out[key] = "a projection frame is the placement";
    const bool base = std::none_of(stack.begin(), stack.end(), [&](const TextureDisplacementLayer& l) { return l.slot < layer.slot; });
    if (base)
        out["blend"] = "the base layer has nothing beneath it to blend with";
    if (!layer.edge_smoothing)
        out["edge_fade_amount"] = "edge_fade is off";
    if (!layer.tile_enabled)
        out["tile_method"] = "tile is off";
    if (layer.color_enabled && !texture_has_color)
        out["color"] = "the texture has no colours";
    if (!std::any_of(stack.begin(), stack.end(), [](const TextureDisplacementLayer& l) { return l.color_enabled; }))
        for (const char* key : {"color_mix", "color_mix_count", "color_denoise"})
            out[key] = "no layer colours the print";
    return out;
}

std::vector<std::string> layer_warnings(const TextureDisplacementLayer& layer)
{
    std::vector<std::string> out;
    if (layer.midlevel > 0.f && layer.depth_mm > 1.f)
        out.push_back("Deep inward displacement may self-intersect.");
    return out;
}

// ---- Masks ----

bool mask_is_stale(const PaintData& data, std::size_t facet_count)
{
    // triangles_to_split is sorted by triangle_idx (FacetsAnnotation::get_triangle_as_string searches it), so the
    // smallest and the largest index decide; a scene read polls this for every layer of every part.
    if (data.triangles_to_split.empty())
        return false;
    return data.triangles_to_split.front().triangle_idx < 0 || size_t(data.triangles_to_split.back().triangle_idx) >= facet_count;
}

std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>> mask_reports(const std::vector<MaskInput>& inputs, const std::vector<int>* slots)
{
    std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>> out;
    out.reserve(inputs.size());
    for (const MaskInput& in : inputs) {
        std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS> row;
        for (size_t slot = 0; slot < row.size(); ++slot) {
            MaskReport& rep = row[slot];
            rep.volume_id   = in.volume_id;
            rep.name        = in.name;
            if (slots != nullptr && std::find(slots->begin(), slots->end(), int(slot)) == slots->end())
                continue;
            const PaintData& data = in.masks[slot];
            if (data.triangles_to_split.empty() || !in.mesh)
                continue;
            if (mask_is_stale(data, in.mesh->its.indices.size())) {
                rep.stale = true;
                continue;
            }
            for (const PaintedStateInfo& info : summarize_paint_data(*in.mesh, data))
                if (info.state == 1) {
                    rep.painted_facets = info.facet_count;
                    rep.area_ratio     = info.area_ratio;
                }
        }
        out.push_back(std::move(row));
    }
    return out;
}

MaskInput mask_input_of(const ModelVolume& v, int volume_id, const std::vector<int>* slots)
{
    MaskInput input;
    input.volume_id = volume_id;
    input.name      = v.name;
    input.mesh      = v.mesh_ptr();
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
        if (slots == nullptr || std::find(slots->begin(), slots->end(), slot) != slots->end())
            input.masks[size_t(slot)] = v.texture_displacement_facet(slot).get_data();
    return input;
}

bool has_texture_paint(const ModelVolume& v)
{
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
        if (mask_has_paint(v.texture_displacement_facet(slot).get_data(), v.mesh().its.indices.size()))
            return true;
    return false;
}

std::vector<int> texture_slots_painting_facet(const MaskInput& input, int facet)
{
    std::vector<int> slots;
    if (!input.mesh || facet < 0 || size_t(facet) >= input.mesh->its.indices.size())
        return slots;
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
        const PaintData& data = input.masks[size_t(slot)];
        if (!mask_has_paint(data, input.mesh->its.indices.size()))
            continue;
        TriangleSelector selector(*input.mesh);
        selector.deserialize(data, false);
        std::vector<int> source;
        selector.get_facets_strict(EnforcerBlockerType::ENFORCER, &source);
        if (std::find(source.begin(), source.end(), facet) != source.end())
            slots.push_back(slot);
    }
    return slots;
}

bool resolve_paint_selection(const nlohmann::json& params, const std::vector<int>& stack_slots, bool all_slots_default,
                             PaintSelection& out, std::string& error)
{
    PaintMode mode = PaintMode::Color;
    out            = PaintSelection{};
    out.named_mode = params.contains("mode");
    if (out.named_mode && !(params["mode"].is_string() && parse_paint_mode(params["mode"].get<std::string>(), mode))) {
        error = "Unknown mode; expected color, support, seam, fuzzy_skin or texture";
        return false;
    }
    if (!(out.named_mode && mode == PaintMode::Texture) && params.contains("texture_layer")) {
        error = "texture_layer belongs to mode texture: it names the layer whose paint this call reads or clears";
        return false;
    }
    out.label = out.named_mode ? paint_mode_name(mode) : "all";
    if (!out.named_mode) {
        out.modes = {PaintMode::Color, PaintMode::Support, PaintMode::Seam, PaintMode::FuzzySkin};
        return true;
    }
    if (mode != PaintMode::Texture) {
        out.modes = {mode};
        return true;
    }
    out.texture = true;
    if (params.contains("texture_layer")) {
        int slot = -1;
        if (!parse_integer_param(params["texture_layer"], slot) || slot < 0 || slot >= int(TEXTURE_DISPLACEMENT_MAX_LAYERS)) {
            error = "texture_layer must be a whole number from 0 to " + std::to_string(int(TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1);
            return false;
        }
        out.texture_slots = {slot};
    } else if (all_slots_default) {
        for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
            out.texture_slots.push_back(slot);
    } else {
        out.texture_slots = stack_slots;
    }
    return true;
}

std::string texture_remap_refusal(const std::map<int, int>& mapping)
{
    for (const auto& [from, to] : mapping)
        if (from > 1)
            return "a texture mask has two states, 0 unpainted and 1 painted, so " + std::to_string(from) + " cannot be renumbered";
    const auto painted = mapping.find(1);
    const auto bare    = mapping.find(0);
    if (painted != mapping.end() && painted->second == 0 && (bare == mapping.end() || bare->second == 0))
        return "mapping 1 to 0 without painting the bare facets leaves split facets in the mask instead of erasing it: "
               "clear_object_paint {mode: texture, texture_layer} clears a layer's paint; {\"0\": 1, \"1\": 0} inverts it";
    return {};
}

std::vector<int> layer_slots_of(const ModelVolume* owner)
{
    std::vector<int> slots;
    if (owner != nullptr)
        for (const TextureDisplacementLayer& layer : owner->texture_displacement_layers)
            slots.push_back(layer.slot);
    std::sort(slots.begin(), slots.end());
    return slots;
}

nlohmann::json texture_paint_layers_cleared_json(const std::vector<int>& slots)
{
    return texture_paint_layers_json(std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>{}, slots);
}

nlohmann::json texture_paint_layers_json(const std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>& row, const std::vector<int>& slots)
{
    nlohmann::json out = nlohmann::json::array();
    for (int slot : slots) {
        if (slot < 0 || size_t(slot) >= row.size())
            continue;
        const MaskReport& rep   = row[size_t(slot)];
        nlohmann::json    entry = {{"layer", slot}, {"painted_facets", rep.painted_facets}, {"coverage_percent", rep.area_ratio * 100.0}};
        if (rep.stale)
            entry["stale"] = true;
        out.push_back(std::move(entry));
    }
    return out;
}

nlohmann::json texture_object_fields(const ModelObject& object)
{
    const ModelVolume* owner = texture_stack_owner(object);
    const bool         uv    = std::any_of(object.volumes.begin(), object.volumes.end(),
                                           [](const ModelVolume* v) { return v->is_model_part() && v->has_texture_uv(); });
    return {{"texture_layers", owner != nullptr ? int(owner->texture_displacement_layers.size()) : 0},
            {"needs_bake", needs_bake_fast(object, owner)},
            {"model_uv", uv}};
}

nlohmann::json texture_volume_fields(const ModelObject& object, std::size_t volume_idx)
{
    const ModelVolume& volume = *object.volumes.at(volume_idx);
    nlohmann::json     slots  = nlohmann::json::array();
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
        if (mask_has_paint(volume.texture_displacement_facet(slot).get_data(), volume.mesh().its.indices.size()))
            slots.push_back(slot);
    return {{"model_uv", volume.has_texture_uv()},
            {"texture_layers", &volume == texture_stack_owner(object) ? int(volume.texture_displacement_layers.size()) : 0},
            {"texture_paint_layers", std::move(slots)}};
}

bool needs_bake(const std::vector<TextureDisplacementLayer>& stack, const std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>>& reports)
{
    for (const TextureDisplacementLayer& layer : stack) {
        if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
            continue;
        for (const auto& row : reports) {
            const MaskReport& rep = row[size_t(layer.slot)];
            if (!rep.stale && rep.painted_facets > 0)
                return true;
        }
    }
    return false;
}

bool mask_has_paint(const PaintData& data, std::size_t facet_count)
{
    return !data.triangles_to_split.empty() && !mask_is_stale(data, facet_count) && data.used_states.size() > 1 && data.used_states[1];
}

bool slot_painted_fast(const ModelObject& object, int slot)
{
    if (slot < 0 || size_t(slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
        return false;
    for (const ModelVolume* v : object.volumes)
        if (v->is_model_part()) {
            const PaintData& data = v->texture_displacement_facet(slot).get_data();
            if (mask_has_paint(data, v->mesh().its.indices.size()))
                return true;
        }
    return false;
}

bool needs_bake_fast(const ModelObject& object, const ModelVolume* owner)
{
    if (owner == nullptr)
        return false;
    for (const TextureDisplacementLayer& layer : owner->texture_displacement_layers)
        if (slot_painted_fast(object, layer.slot))
            return true;
    return false;
}

// ---- What a mesh edit dropped ----

TextureState texture_state_of(const ModelObject& object)
{
    TextureState state;
    state.volume_count = object.volumes.size();
    const ModelVolume* owner = texture_stack_owner(object);
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume* v = object.volumes[i];
        TextureState::Part part;
        part.id          = v->id();
        part.volume      = v;
        part.volume_id   = int(i);
        part.name        = v->name;
        part.layers      = int(v->texture_displacement_layers.size());
        part.stack_owner = v == owner;
        part.model_part  = v->is_model_part();
        part.has_uv      = v->has_texture_uv();
        part.vertices    = v->mesh().its.vertices.size();
        part.facets      = v->mesh().its.indices.size();
        for (const TextureDisplacementLayer& layer : v->texture_displacement_layers)
            if (!layer.lscm_seam_edges.empty() || !layer.islands.empty() || !layer.island_groups.empty() || !layer.lscm_uv_overrides.empty())
                part.unwrap_edits = true;
        for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
            if (mask_has_paint(v->texture_displacement_facet(slot).get_data(), part.facets))
                part.painted_slots.push_back(slot);
        // A volume of any type is told when it holds a stack or paint: the first part turned into a modifier
        // keeps its stack, which the object no longer reads.
        if (v->is_model_part() || part.layers > 0 || !part.painted_slots.empty())
            state.parts.push_back(std::move(part));
    }
    return state;
}

void forget_texture_paint_of(TextureState& state, int volume_id)
{
    for (TextureState::Part& part : state.parts)
        if (part.volume_id == volume_id) {
            part.painted_slots.clear();
            part.has_uv       = false;
            part.unwrap_edits = false;
        }
}

json texture_changes_json(const TextureState& before, const std::vector<const ModelObject*>& after, const ModelVolume* owner_after)
{
    std::vector<TextureState::Part> after_parts;
    std::vector<bool>               unused_after; // parallel: holds a stack its object ignores
    for (size_t o = 0; o < after.size(); ++o) {
        const TextureState state = texture_state_of(*after[o]);
        const ModelVolume* owner = (o == 0 && owner_after != nullptr) ? owner_after : texture_stack_owner(*after[o]);
        for (const TextureState::Part& p : state.parts) {
            after_parts.push_back(p);
            unused_after.push_back(p.layers > 0 && (owner == nullptr || owner->id() != p.id));
        }
    }
    const auto find_after = [&](const TextureState::Part& b) {
        return std::find_if(after_parts.begin(), after_parts.end(), [&](const TextureState::Part& a) { return a.id == b.id || (b.volume != nullptr && a.volume == b.volume); });
    };

    json lost = json::array(), uv_lost = json::array(), stale = json::array(), dead = json::array();
    int  layers_lost = 0;
    for (const TextureState::Part& b : before.parts) {
        const auto       it = find_after(b);
        const bool       found = it != after_parts.end();
        std::vector<int> slots;
        for (int slot : b.painted_slots)
            if (!found || std::find(it->painted_slots.begin(), it->painted_slots.end(), slot) == it->painted_slots.end())
                slots.push_back(slot);
        if (!slots.empty())
            lost.push_back({{"volume_id", b.volume_id}, {"name", b.name}, {"layers", slots}});
        layers_lost += found ? std::max(0, b.layers - it->layers) : b.layers;
        if (b.has_uv && (!found || !it->has_uv))
            uv_lost.push_back({{"volume_id", b.volume_id}, {"name", b.name}});
        if (found && b.unwrap_edits && it->layers > 0 && (b.vertices != it->vertices || b.facets != it->facets))
            stale.push_back({{"volume_id", b.volume_id}, {"name", b.name}});
        if (found && b.model_part && !it->model_part && !it->painted_slots.empty())
            dead.push_back({{"volume_id", b.volume_id}, {"name", b.name}, {"layers", it->painted_slots}});
    }

    // A stack the edit left on a part the object does not read it from (it was not so before).
    json unused = json::array();
    for (size_t i = 0; i < after_parts.size(); ++i) {
        if (!unused_after[i])
            continue;
        const auto was = std::find_if(before.parts.begin(), before.parts.end(), [&](const TextureState::Part& b) { return b.id == after_parts[i].id || (b.volume != nullptr && b.volume == after_parts[i].volume); });
        const bool unused_before = was != before.parts.end() && was->layers > 0 && !was->stack_owner;
        if (!unused_before)
            unused.push_back({{"volume_id", after_parts[i].volume_id}, {"layers", after_parts[i].layers}});
    }

    json out = json::object();
    if (!lost.empty())
        out["paint_lost"] = lost;
    if (layers_lost > 0)
        out["layers_lost"] = layers_lost;
    if (!unused.empty())
        out["stacks_unused"] = unused;
    if (!uv_lost.empty())
        out["uv_lost"] = uv_lost;
    if (!stale.empty())
        out["unwrap_edits_stale"] = stale;
    if (!dead.empty())
        out["paint_unused"] = dead;
    return out;
}

namespace {

std::string slot_list(const json& slots)
{
    std::vector<std::string> names;
    for (const json& s : slots)
        names.push_back(std::to_string(s.get<int>()));
    if (names.size() == 1)
        return names[0];
    std::string out;
    for (size_t i = 0; i < names.size(); ++i)
        out += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
    return out;
}

} // namespace

std::vector<std::string> texture_changes_messages(const json& changes, const std::string& subject, const std::string& cause)
{
    std::vector<std::string> said;
    for (const json& lost : changes.value("paint_lost", json::array())) {
        const json& slots = lost.at("layers");
        said.push_back(subject + " lost the paint of texture layer" + (slots.size() == 1 ? " " : "s ") + slot_list(slots) + " on part " +
                       std::to_string(lost.value("volume_id", 0)) + ": " + cause + "; that region has to be painted again (paint_object mode texture)");
    }
    if (changes.value("layers_lost", 0) > 0)
        said.push_back(subject + " lost " + std::to_string(changes.at("layers_lost").get<int>()) + " texture layer(s) with the part that held them: " +
                       cause + "; set_texture_displacement add makes them again");
    for (const json& unused : changes.value("stacks_unused", json::array()))
        said.push_back("part " + std::to_string(unused.value("volume_id", 0)) + " of " + subject + " holds " +
                       std::to_string(unused.value("layers", 0)) + " texture layer(s) the object does not read: only its first model part's count");
    for (const json& uv : changes.value("uv_lost", json::array()))
        said.push_back("part " + std::to_string(uv.value("volume_id", 0)) + " of " + subject + " lost its UV channel: " + cause +
                       "; a model_uv layer cannot map it (use another projection)");
    for (const json& stale : changes.value("unwrap_edits_stale", json::array()))
        said.push_back("part " + std::to_string(stale.value("volume_id", 0)) + " of " + subject + " keeps the unwrap edits (seams, islands, UV overrides) made on its old mesh: "
                       "nothing checks them against the new mesh, so they may cut or move the wrong places (get_texture_unwrap shows them)");
    for (const json& dead : changes.value("paint_unused", json::array()))
        said.push_back("part " + std::to_string(dead.value("volume_id", 0)) + " of " + subject + " is no longer a model part, so its texture paint (layer" +
                       (dead.at("layers").size() == 1 ? " " : "s ") + slot_list(dead.at("layers")) + ") is applied by no bake");
    for (const json& moved : changes.value("paint_reassigned", json::array()))
        said.push_back("part " + std::to_string(moved.value("volume_id", 0)) + " of " + subject + " holds texture paint of layer" +
                       (moved.at("layers").size() == 1 ? " " : "s ") + slot_list(moved.at("layers")) +
                       " made against another object's layers: " + cause + "; it is now read against this object's own layers");
    return said;
}

void add_texture_changes(json& answer, const json& changes, int object_id, const std::string& cause, const std::string& subject)
{
    if (changes.empty())
        return;
    answer["texture_changes"] = changes;
    json& said = answer["info_messages"];
    if (!said.is_array())
        said = json::array();
    for (const std::string& line : texture_changes_messages(changes, subject.empty() ? "object " + std::to_string(object_id) : subject, cause))
        said.push_back(line);
    append_next_steps(answer, texture_changes_next_steps(object_id, changes));
}

void add_objects_texture_changes(json& answer, const std::vector<std::pair<int, json>>& changes, const std::string& cause)
{
    std::vector<int> ids;
    json             list = json::array();
    for (const auto& [object_id, one] : changes) {
        if (one.empty())
            continue;
        ids.push_back(object_id);
        json entry         = one;
        entry["object_id"] = object_id;
        list.push_back(std::move(entry));
    }
    if (ids.empty())
        return;
    json& said = answer["info_messages"];
    if (!said.is_array())
        said = json::array();
    if (ids.size() == 1)
        for (const std::string& line : texture_changes_messages(list[0], "object " + std::to_string(ids[0]), cause))
            said.push_back(line);
    else
        said.push_back("objects " + listed_ids(ids) + " lost texture paint or layers, or keep texture data that no longer fits: " + cause + "; texture_changes tells what for each");
    answer["texture_changes"] = std::move(list);
    // One step per tool, built from every object's changes: arguments name the first object, why all of them.
    json merged = json::object();
    for (const auto& [object_id, one] : changes)
        for (const auto& [key, value] : one.items())
            if (key == "layers_lost")
                merged[key] = merged.value(key, 0) + value.get<int>();
            else {
                json& all = merged[key];
                if (!all.is_array())
                    all = json::array();
                for (const json& item : value)
                    all.push_back(item);
            }
    append_next_steps(answer, texture_changes_next_steps(ids, merged));
}

void rekey_texture_state(TextureState& state, const ModelObject& after)
{
    if (state.volume_count != after.volumes.size())
        return;
    for (TextureState::Part& part : state.parts)
        if (part.volume_id >= 0) {
            part.id     = after.volumes[size_t(part.volume_id)]->id();
            part.volume = after.volumes[size_t(part.volume_id)];
        }
}

// ---- The unwrap request ----

namespace {

bool read_index(const json& v, const std::string& what, int& out, std::string& error)
{
    if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > 100000000) {
        error = what + " must be a whole number from 0";
        return false;
    }
    out = int(v.get<long long>());
    return true;
}

bool read_index_list(const json& params, const char* key, std::vector<int>& out, std::string& error)
{
    if (!params.contains(key))
        return true;
    const json& v = params.at(key);
    if (!v.is_array()) {
        error = std::string(key) + " must be a list of whole numbers";
        return false;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        int n = 0;
        if (!read_index(v[i], std::string(key) + "[" + std::to_string(i) + "]", n, error))
            return false;
        out.push_back(n);
    }
    return true;
}

bool read_pairs(const json& params, const char* key, std::vector<std::pair<int, int>>& out, std::string& error)
{
    if (!params.contains(key))
        return true;
    const json& v = params.at(key);
    if (!v.is_array()) {
        error = std::string(key) + " must be a list of [vertex, vertex] pairs";
        return false;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        const std::string what = std::string(key) + "[" + std::to_string(i) + "]";
        int a = 0, b = 0;
        if (!v[i].is_array() || v[i].size() != 2 || !read_index(v[i][0], what, a, error) || !read_index(v[i][1], what, b, error)) {
            if (error.empty())
                error = what + " must be a [vertex, vertex] pair";
            return false;
        }
        if (a == b) {
            error = what + " names one vertex twice";
            return false;
        }
        out.emplace_back(std::min(a, b), std::max(a, b));
    }
    return true;
}

bool known_keys(const json& obj, const std::string& what, const std::set<std::string>& keys, std::string& error)
{
    if (!obj.is_object()) {
        error = what + " must be an object";
        return false;
    }
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (!keys.count(it.key())) {
            std::string list;
            for (const std::string& k : keys)
                list += (list.empty() ? "" : ", ") + k;
            error = what + " has no key \"" + it.key() + "\". Its keys: " + list;
            return false;
        }
    return true;
}

} // namespace

std::optional<UnwrapRequest> read_unwrap_request(const json& params, std::string& error)
{
    error.clear();
    if (!params.is_object()) {
        error = "arguments must be an object";
        return std::nullopt;
    }
    UnwrapRequest req;
    Reader        r{params, error};

    if (params.contains("unwrap_id")) {
        if (!params.at("unwrap_id").is_string()) {
            error = "unwrap_id must be the text get_texture_unwrap gave";
            return std::nullopt;
        }
        req.unwrap_id = params.at("unwrap_id").get<std::string>();
    }
    if (params.contains("seam_angle_deg")) {
        double v = 0;
        if (!r.number("seam_angle_deg", v))
            return std::nullopt;
        if (v < 5. || v > 90.) {
            error = "seam_angle_deg must be from 5 to 90 degrees (the panel's Seam angle), not " + number_text(v);
            return std::nullopt;
        }
        req.seam_angle_deg = float(v);
    }
    if (!r.boolean("auto_connect_islands", req.auto_connect_islands))
        return std::nullopt;
    for (const char* key : {"clear_seams", "clear_uv_edits", "average_scale", "relayout"}) {
        std::optional<bool> v;
        if (!r.boolean(key, v))
            return std::nullopt;
        if (v.value_or(false)) {
            const std::string k = key;
            (k == "clear_seams" ? req.clear_seams : k == "clear_uv_edits" ? req.clear_uv_edits : k == "average_scale" ? req.average_scale : req.relayout) = true;
        }
    }
    if (!read_pairs(params, "add_seams", req.add_seams, error) || !read_pairs(params, "remove_seams", req.remove_seams, error))
        return std::nullopt;
    if (params.contains("seam_paths")) {
        const json& v = params.at("seam_paths");
        if (!v.is_array()) {
            error = "seam_paths must be a list of {from: [x, y, z], to: [x, y, z]}";
            return std::nullopt;
        }
        for (size_t i = 0; i < v.size(); ++i) {
            const std::string what = "seam_paths[" + std::to_string(i) + "]";
            Vec3d from, to;
            if (!known_keys(v[i], what, {"from", "to"}, error) || !v[i].contains("from") || !v[i].contains("to")) {
                if (error.empty())
                    error = what + " needs from and to";
                return std::nullopt;
            }
            if (!read_vec3(v[i].at("from"), what + " from", from, error) || !read_vec3(v[i].at("to"), what + " to", to, error))
                return std::nullopt;
            req.seam_paths.emplace_back(from, to);
        }
    }
    if (!read_index_list(params, "cut_islands", req.cut_islands, error) || !read_index_list(params, "unjoin", req.unjoin, error))
        return std::nullopt;
    if (params.contains("join")) {
        const json& v = params.at("join");
        if (!v.is_array()) {
            error = "join must be a list of {chart, to?}";
            return std::nullopt;
        }
        for (size_t i = 0; i < v.size(); ++i) {
            const std::string what = "join[" + std::to_string(i) + "]";
            int chart = 0;
            if (!known_keys(v[i], what, {"chart", "to"}, error) || !v[i].contains("chart") || !read_index(v[i].at("chart"), what + " chart", chart, error)) {
                if (error.empty())
                    error = what + " needs chart";
                return std::nullopt;
            }
            std::optional<int> to;
            if (v[i].contains("to")) {
                int n = 0;
                if (!read_index(v[i].at("to"), what + " to", n, error))
                    return std::nullopt;
                to = n;
            }
            req.join.emplace_back(chart, to);
        }
    }
    if (params.contains("islands")) {
        const json& v = params.at("islands");
        if (!v.is_array()) {
            error = "islands must be a list of {chart, offset?, rotation_deg?, scale?}";
            return std::nullopt;
        }
        for (size_t i = 0; i < v.size(); ++i) {
            const std::string what = "islands[" + std::to_string(i) + "]";
            IslandEdit edit;
            if (!known_keys(v[i], what, {"chart", "offset", "rotation_deg", "scale", "centroid", "group", "area_mm2", "bounding_box", "neighbours"}, error) || !v[i].contains("chart") ||
                !read_index(v[i].at("chart"), what + " chart", edit.chart, error)) {
                if (error.empty())
                    error = what + " needs chart";
                return std::nullopt;
            }
            Reader ir{v[i], error};
            if (v[i].contains("offset")) {
                std::vector<double> c;
                if (!read_vec(v[i].at("offset"), what + " offset", 2, c, error))
                    return std::nullopt;
                edit.offset = Vec2f(float(c[0]), float(c[1]));
            }
            for (const char* key : {"rotation_deg", "scale"}) {
                if (!v[i].contains(key))
                    continue;
                double d = 0;
                if (!ir.number(key, d, (what + " ").c_str()))
                    return std::nullopt;
                if (std::string(key) == "scale") {
                    if (d < 0.001 || d > 1000.) {
                        error = what + " scale must be from 0.001 to 1000, not " + number_text(d);
                        return std::nullopt;
                    }
                    edit.scale = float(d);
                } else
                    edit.rotation_deg = float(d);
            }
            req.islands.push_back(edit);
        }
    }
    if (params.contains("uv_edits")) {
        const json& v = params.at("uv_edits");
        if (!v.is_array()) {
            error = "uv_edits must be a list of {vertex, uv: [u, v]}";
            return std::nullopt;
        }
        for (size_t i = 0; i < v.size(); ++i) {
            const std::string what = "uv_edits[" + std::to_string(i) + "]";
            int vertex = 0;
            if (!known_keys(v[i], what, {"vertex", "uv"}, error) || !v[i].contains("vertex") || !v[i].contains("uv") ||
                !read_index(v[i].at("vertex"), what + " vertex", vertex, error)) {
                if (error.empty())
                    error = what + " needs vertex and uv";
                return std::nullopt;
            }
            std::vector<double> c;
            if (!read_vec(v[i].at("uv"), what + " uv", 2, c, error))
                return std::nullopt;
            req.uv_edits.emplace_back(vertex, Vec2f(float(c[0]), float(c[1])));
        }
    }
    return req;
}

namespace {

struct ChartFacts
{
    std::vector<double> area;
    std::vector<Vec2f>  low, high;
    std::vector<std::set<int>> neighbours;
};

// Per chart: the area of its triangles in the unwrap's mm, the box of its vertices, and the charts sharing a mesh edge with it.
ChartFacts chart_facts(const PatchUnwrap& unwrap)
{
    const size_t count = size_t(std::max(unwrap.chart_count, 0));
    ChartFacts   out;
    out.area.assign(count, 0.);
    out.low.assign(count, Vec2f::Constant(std::numeric_limits<float>::max()));
    out.high.assign(count, Vec2f::Constant(std::numeric_limits<float>::lowest()));
    out.neighbours.resize(count);
    const auto chart_of = [&](int vertex) { return vertex >= 0 && size_t(vertex) < unwrap.vertex_chart.size() ? unwrap.vertex_chart[size_t(vertex)] : -1; };
    for (const stl_triangle_vertex_indices& tri : unwrap.indices) {
        const int chart = chart_of(tri[0]);
        if (chart < 0 || size_t(chart) >= count || tri[1] < 0 || tri[2] < 0 || size_t(tri[0]) >= unwrap.uvs.size() ||
            size_t(tri[1]) >= unwrap.uvs.size() || size_t(tri[2]) >= unwrap.uvs.size())
            continue;
        const Vec2f e1 = unwrap.uvs[size_t(tri[1])] - unwrap.uvs[size_t(tri[0])];
        const Vec2f e2 = unwrap.uvs[size_t(tri[2])] - unwrap.uvs[size_t(tri[0])];
        out.area[size_t(chart)] += 0.5 * std::abs(double(e1.x()) * double(e2.y()) - double(e1.y()) * double(e2.x()));
    }
    for (size_t i = 0; i < unwrap.uvs.size() && i < unwrap.vertex_chart.size(); ++i) {
        const int chart = unwrap.vertex_chart[i];
        if (chart < 0 || size_t(chart) >= count)
            continue;
        out.low[size_t(chart)]  = out.low[size_t(chart)].cwiseMin(unwrap.uvs[i]);
        out.high[size_t(chart)] = out.high[size_t(chart)].cwiseMax(unwrap.uvs[i]);
    }
    // Two charts are neighbours where both have the same mesh edge on their outline: the outline edges (in unwrapped
    // vertices) by the mesh edge they stand on, sorted, then equal neighbours with different charts linked.
    struct OutlineEdge
    {
        int a, b, chart;
        bool operator<(const OutlineEdge& o) const { return std::tie(a, b, chart) < std::tie(o.a, o.b, o.chart); }
    };
    std::vector<OutlineEdge> outline;
    outline.reserve(unwrap.boundary_edges.size());
    for (const auto& [u, v] : unwrap.boundary_edges) {
        if (u < 0 || v < 0 || size_t(u) >= unwrap.source_vertex.size() || size_t(v) >= unwrap.source_vertex.size() || size_t(u) >= unwrap.vertex_chart.size())
            continue;
        const int a = unwrap.source_vertex[size_t(u)], b = unwrap.source_vertex[size_t(v)];
        outline.push_back({std::min(a, b), std::max(a, b), unwrap.vertex_chart[size_t(u)]});
    }
    std::sort(outline.begin(), outline.end());
    for (size_t i = 0; i < outline.size();) {
        size_t j = i;
        while (j < outline.size() && outline[j].a == outline[i].a && outline[j].b == outline[i].b)
            ++j;
        for (size_t x = i; x < j; ++x)
            for (size_t y = i; y < j; ++y)
                if (outline[x].chart != outline[y].chart && outline[x].chart >= 0 && size_t(outline[x].chart) < count)
                    out.neighbours[size_t(outline[x].chart)].insert(outline[y].chart);
        i = j;
    }
    return out;
}

double rounded(double v) { return std::round(v * 1e4) / 1e4; }
json   point2(const Vec2f& v) { return {rounded(v.x()), rounded(v.y())}; }

} // namespace

std::string unwrap_id(const PatchUnwrap& unwrap, const TextureDisplacementLayer& layer)
{
    std::uint64_t h = 1469598103934665603ull;
    const auto    mix = [&h](const void* data, std::size_t bytes) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    mix(&unwrap.chart_count, sizeof(int));
    mix(unwrap.source_vertex.data(), unwrap.source_vertex.size() * sizeof(int));
    mix(unwrap.vertex_chart.data(), unwrap.vertex_chart.size() * sizeof(int));
    mix(unwrap.indices.data(), unwrap.indices.size() * sizeof(stl_triangle_vertex_indices));
    for (const auto& [key, uv] : layer.lscm_uv_overrides) {
        mix(&key, sizeof(key));
        mix(uv.data(), 2 * sizeof(float));
    }
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(h));
    return text;
}

json unwrap_json(const PatchUnwrap& unwrap, const TextureDisplacementLayer& layer, bool include_vertices)
{
    json seams = json::array();
    for (const auto& s : layer.lscm_seam_edges)
        seams.push_back({s.first, s.second});
    const ChartFacts facts = chart_facts(unwrap);
    json             islands = json::array();
    const int shown_charts = int(std::min(size_t(std::max(unwrap.chart_count, 0)), max_unwrap_islands));
    for (int c = 0; c < shown_charts; ++c) {
        const TextureIsland isl = size_t(c) < layer.islands.size() ? layer.islands[size_t(c)] : TextureIsland{};
        const Vec2f centroid = size_t(c) < unwrap.chart_centroid.size() ? unwrap.chart_centroid[size_t(c)] : Vec2f::Zero();
        json        neighbours = json::array();
        for (const int n : facts.neighbours[size_t(c)])
            neighbours.push_back(n);
        const bool seen = facts.low[size_t(c)].x() <= facts.high[size_t(c)].x();
        islands.push_back({{"chart", c},
                           {"area_mm2", rounded(facts.area[size_t(c)])},
                           {"centroid", point2(centroid)},
                           {"bounding_box", seen ? json({{"min", point2(facts.low[size_t(c)])}, {"max", point2(facts.high[size_t(c)])}}) : json(nullptr)},
                           {"offset", {isl.offset.x(), isl.offset.y()}},
                           {"rotation_deg", isl.rotation_deg},
                           {"scale", isl.scale},
                           {"group", island_group_of(layer.island_groups, c)},
                           {"neighbours", neighbours}});
    }
    json uv_edits = json::array(), mesh_vertex_edits = json::array();
    for (const auto& [key, uv] : layer.lscm_uv_overrides) {
        if (key < 0)
            uv_edits.push_back({{"vertex", -(key + 1)}, {"uv", {uv.x(), uv.y()}}});
        else
            mesh_vertex_edits.push_back({{"mesh_vertex", key}, {"uv", {uv.x(), uv.y()}}});
    }
    json out = {{"chart_count", unwrap.chart_count},
                {"vertex_count", unwrap.uvs.size()},
                {"triangle_count", unwrap.indices.size()},
                {"seam_angle_deg", layer.lscm_seam_angle_deg},
                {"auto_connect_islands", layer.auto_connect_islands},
                {"seams", seams},
                {"islands", islands},
                {"uv_edits", uv_edits},
                {"unwrap_id", unwrap_id(unwrap, layer)},
                {"islands_truncated", shown_charts < unwrap.chart_count},
                {"note", "Chart and vertex numbers hold for this unwrap only: changing the seams or the seam angle numbers them anew."}};
    if (!mesh_vertex_edits.empty())
        out["mesh_vertex_uv_edits"] = mesh_vertex_edits;
    if (include_vertices) {
        json vertices = json::array();
        const size_t shown = std::min(unwrap.uvs.size(), max_unwrap_vertices);
        for (size_t i = 0; i < shown; ++i)
            vertices.push_back({{"vertex", i},
                                {"source_vertex", i < unwrap.source_vertex.size() ? unwrap.source_vertex[i] : -1},
                                {"chart", i < unwrap.vertex_chart.size() ? unwrap.vertex_chart[i] : -1},
                                {"uv", {unwrap.uvs[i].x(), unwrap.uvs[i].y()}}});
        out["vertices"]  = vertices;
        out["truncated"] = shown < unwrap.uvs.size();
    }
    return out;
}

// ---- Editing the unwrap ----

indexed_triangle_set unwrap_patch(const TriangleMesh& owner_mesh, const PaintData& mask, const Transform3d& volume_to_world)
{
    indexed_triangle_set patch = extract_painted_patch(owner_mesh.its, mask);
    if (patch.indices.empty())
        return patch;
    const Transform3d to_world = texture_displacement_bake_frame(volume_to_world);
    if (!to_world.matrix().isApprox(Transform3d::Identity().matrix()))
        for (Vec3f& v : patch.vertices)
            v = (to_world * v.cast<double>()).cast<float>();
    return patch;
}

PatchUnwrap layer_unwrap(const indexed_triangle_set& patch, const TextureDisplacementLayer& layer)
{
    PatchUnwrap unwrap = compute_patch_unwrap(patch, layer.lscm_seam_angle_deg, 0.f, layer.lscm_seam_edges);
    apply_lscm_uv_overrides(unwrap, layer.lscm_uv_overrides);
    return unwrap;
}

namespace {

bool islands_equal(const std::vector<TextureIsland>& a, const std::vector<TextureIsland>& b)
{
    const TextureIsland fresh;
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const TextureIsland& x = i < a.size() ? a[i] : fresh;
        const TextureIsland& y = i < b.size() ? b[i] : fresh;
        if (x.offset != y.offset || x.rotation_deg != y.rotation_deg || x.scale != y.scale)
            return false;
    }
    return true;
}

bool groups_equal(const std::vector<int>& a, const std::vector<int>& b)
{
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
        if (island_group_of(a, int(i)) != island_group_of(b, int(i)))
            return false;
    return true;
}

std::vector<std::pair<int, int>> sorted_seams(std::vector<std::pair<int, int>> seams)
{
    std::sort(seams.begin(), seams.end());
    seams.erase(std::unique(seams.begin(), seams.end()), seams.end());
    return seams;
}

void add_seam(std::vector<std::pair<int, int>>& seams, const std::pair<int, int>& edge)
{
    if (std::find(seams.begin(), seams.end(), edge) == seams.end())
        seams.push_back(edge);
}

// The undo step the pane names for a call that makes one kind of edit; none for a setting, which the pane takes no step for.
std::string snapshot_of(const std::set<std::string>& kinds)
{
    static const std::map<std::string, std::string> names = {
        {"seam", "Mark texture seam"},         {"seam_path", "Mark texture seam path"}, {"clear_seams", "Clear texture seams"},
        {"cut", "Cut texture island"},         {"join", "Join texture island"},         {"unjoin", "Unjoin texture island"},
        {"average_scale", "Average island scale"}, {"clear_uv_edits", "Clear texture UV edits"}, {"islands", "Move texture island"},
        {"uv_edits", "Edit texture UV"}};
    if (kinds.size() == 1)
        if (const auto it = names.find(*kinds.begin()); it != names.end())
            return it->second;
    return "Texture displacement editing";
}

std::string chart_error(const std::string& what, int chart, int count)
{
    return what + ": chart " + std::to_string(chart) + " is not in this unwrap, which has " + std::to_string(count) +
           " (0 to " + std::to_string(std::max(count - 1, 0)) + "); get_texture_unwrap lists them";
}

} // namespace

bool same_unwrap_state(const TextureDisplacementLayer& a, const TextureDisplacementLayer& b)
{
    return sorted_seams(a.lscm_seam_edges) == sorted_seams(b.lscm_seam_edges) && a.lscm_seam_angle_deg == b.lscm_seam_angle_deg &&
           a.auto_connect_islands == b.auto_connect_islands && islands_equal(a.islands, b.islands) && groups_equal(a.island_groups, b.island_groups) &&
           a.lscm_uv_overrides == b.lscm_uv_overrides;
}

void copy_unwrap_state(const TextureDisplacementLayer& from, TextureDisplacementLayer& to)
{
    to.lscm_seam_edges     = from.lscm_seam_edges;
    to.lscm_seam_angle_deg = from.lscm_seam_angle_deg;
    to.auto_connect_islands = from.auto_connect_islands;
    to.islands             = from.islands;
    to.island_groups       = from.island_groups;
    to.lscm_uv_overrides   = from.lscm_uv_overrides;
}

std::optional<UnwrapEdit> edit_unwrap(const TextureDisplacementLayer& before, const UnwrapRequest& req, const indexed_triangle_set& mesh,
                                      const indexed_triangle_set& patch, const Transform3d& volume_to_plate,
                                      const std::function<bool()>& cancelled, std::string& error)
{
    error.clear();
    const auto stop = [&]() {
        if (!cancelled())
            return false;
        error = "The unwrap was stopped (the app is quitting or the call ran out of time); nothing was changed";
        return true;
    };

    const bool numbered = !req.join.empty() || !req.unjoin.empty() || !req.islands.empty() || !req.uv_edits.empty();

    UnwrapEdit                out;
    out.layer = before;
    TextureDisplacementLayer& layer = out.layer;
    PatchUnwrap               unwrap = layer_unwrap(patch, layer);
    if (unwrap.empty()) {
        error = "The layer's paint on the owner part leaves nothing to unwrap";
        return std::nullopt;
    }
    if (stop())
        return std::nullopt;
    const int         charts = unwrap.chart_count;
    if (req.unwrap_id && (numbered || !req.cut_islands.empty()) && *req.unwrap_id != unwrap_id(unwrap, layer)) {
        error = "unwrap_id " + *req.unwrap_id + " is not this unwrap's (" + unwrap_id(unwrap, layer) + "): the paint, the mesh or the seams changed since "
                "the chart and vertex numbers were read, so nothing was changed; read get_texture_unwrap again";
        return std::nullopt;
    }
    std::set<std::string> kinds;

    // Settings.
    bool resolve = false, relayout = req.relayout;
    bool angle_changed = false;
    if (req.seam_angle_deg && *req.seam_angle_deg != layer.lscm_seam_angle_deg) {
        layer.lscm_seam_angle_deg = *req.seam_angle_deg;
        angle_changed             = true;
        kinds.insert("settings");
    }
    if (req.auto_connect_islands && *req.auto_connect_islands != layer.auto_connect_islands) {
        layer.auto_connect_islands = *req.auto_connect_islands;
        relayout |= layer.auto_connect_islands; // turning it on lays the islands out at once, as the pane does
        kinds.insert("settings");
    }

    // Seams.
    std::vector<std::pair<int, int>>& seams = layer.lscm_seam_edges;
    const auto                        seams_before = sorted_seams(seams);
    const auto                        seam_step = [&](const char* kind, const std::vector<std::pair<int, int>>& was) {
        if (sorted_seams(seams) != was)
            kinds.insert(kind);
    };
    if (req.clear_seams) {
        const auto was = sorted_seams(seams);
        seams.clear();
        seam_step("clear_seams", was);
    }
    if (!req.add_seams.empty() || !req.remove_seams.empty()) {
        const auto was = sorted_seams(seams);
        std::set<std::pair<int, int>> mesh_edges;
        for (const stl_triangle_vertex_indices& tri : mesh.indices)
            for (int k = 0; k < 3; ++k)
                mesh_edges.insert({std::min(tri[k], tri[(k + 1) % 3]), std::max(tri[k], tri[(k + 1) % 3])});
        for (size_t i = 0; i < req.add_seams.size(); ++i) {
            const auto& edge = req.add_seams[i];
            if (size_t(edge.second) >= mesh.vertices.size()) {
                error = "add_seams[" + std::to_string(i) + "]: vertex " + std::to_string(edge.second) + " is not a vertex of the owner's mesh, which has " +
                        std::to_string(mesh.vertices.size());
                return std::nullopt;
            }
            if (!mesh_edges.count(edge)) {
                error = "add_seams[" + std::to_string(i) + "]: vertices " + std::to_string(edge.first) + " and " + std::to_string(edge.second) +
                        " are not joined by an edge of the owner's mesh; seam_paths marks a path between any two points";
                return std::nullopt;
            }
            add_seam(seams, edge);
        }
        for (const auto& edge : req.remove_seams)
            seams.erase(std::remove(seams.begin(), seams.end(), edge), seams.end());
        seam_step("seam", was);
    }
    if (!req.seam_paths.empty()) {
        const auto was = sorted_seams(seams);
        const auto nearest = [&](const Vec3d& point) {
            int    best  = -1;
            double best_d = std::numeric_limits<double>::max();
            for (size_t v = 0; v < mesh.vertices.size(); ++v) {
                const double d = (volume_to_plate * mesh.vertices[v].cast<double>() - point).squaredNorm();
                if (d < best_d) {
                    best_d = d;
                    best   = int(v);
                }
            }
            return best;
        };
        for (size_t i = 0; i < req.seam_paths.size(); ++i) {
            const int from = nearest(req.seam_paths[i].first), to = nearest(req.seam_paths[i].second);
            out.snapped_paths.emplace_back(from, to);
            if (from < 0 || from == to) {
                error = "seam_paths[" + std::to_string(i) + "]: both ends are nearest to vertex " + std::to_string(from) + "; give two points apart";
                return std::nullopt;
            }
            const std::vector<std::pair<int, int>> path = seam_path_edges(mesh, from, to);
            if (path.empty()) {
                error = "seam_paths[" + std::to_string(i) + "]: no path along the owner's mesh edges joins vertices " + std::to_string(from) + " and " +
                        std::to_string(to);
                return std::nullopt;
            }
            for (const auto& edge : path)
                add_seam(seams, edge);
        }
        seam_step("seam_path", was);
    }
    if (!req.cut_islands.empty()) {
        const auto was = sorted_seams(seams);
        for (const int chart : req.cut_islands) {
            if (chart >= charts) {
                error = chart_error("cut_islands", chart, charts);
                return std::nullopt;
            }
            seams = cut_island_seams(mesh, unwrap, chart, seams);
        }
        seam_step("cut", was);
    }
    // The library drops the angle once any seam exists, so then a new angle numbers nothing anew.
    resolve      = angle_changed && seams.empty();
    out.resolved = resolve || sorted_seams(seams) != seams_before;
    if (out.resolved && numbered) {
        error = "Chart and vertex numbers (join, unjoin, islands, uv_edits) name the unwrap as it is before the call, and this call changes the seams or "
                "the seam angle, which numbers it anew: send the seam change first, read get_texture_unwrap, then the island edits";
        return std::nullopt;
    }
    if (out.resolved) {
        unwrap = layer_unwrap(patch, layer);
        if (unwrap.empty()) {
            error = "With those seams the layer's paint leaves nothing to unwrap";
            return std::nullopt;
        }
        if (stop())
            return std::nullopt;
        relayout |= layer.auto_connect_islands; // the pane's Unwrap lays a new unwrap out when the layer asks for it
    }
    const int new_charts = unwrap.chart_count; // equal to `charts` unless resolved
    const auto fits = [&](int chart) { return chart >= 0 && chart < new_charts; };
    if (layer.islands.size() < size_t(new_charts)) // grown, never shrunk, as the pane does
        layer.islands.resize(size_t(new_charts));

    // Layout.
    if (relayout) {
        const std::vector<TextureIsland> net = compute_connected_net(unwrap);
        if (net.size() == size_t(new_charts)) {
            if (layer.islands.size() < net.size())
                layer.islands.resize(net.size());
            for (size_t i = 0; i < net.size(); ++i)
                layer.islands[i] = net[i];
        }
        if (req.relayout && !islands_equal(layer.islands, before.islands))
            kinds.insert("relayout");
        else if (req.auto_connect_islands && !out.resolved && !islands_equal(layer.islands, before.islands))
            kinds.insert("settings");
    }
    if (!req.islands.empty()) {
        const auto was = layer.islands;
        std::set<int> listed;
        for (const IslandEdit& edit : req.islands)
            listed.insert(edit.chart);
        for (const IslandEdit& edit : req.islands) {
            if (!fits(edit.chart)) {
                error = chart_error("islands", edit.chart, new_charts);
                return std::nullopt;
            }
            TextureIsland& island = layer.islands[size_t(edit.chart)];
            if (edit.offset) {
                // As the pane's drag does: a pure move carries the chart's whole join group, the charts the call does not name.
                if (!edit.rotation_deg && !edit.scale) {
                    const Vec2f delta = *edit.offset - island.offset;
                    const int   group = island_group_of(layer.island_groups, edit.chart);
                    for (int c = 0; c < new_charts; ++c)
                        if (c != edit.chart && !listed.count(c) && island_group_of(layer.island_groups, c) == group)
                            layer.islands[size_t(c)].offset += delta;
                }
                island.offset = *edit.offset;
            }
            if (edit.rotation_deg)
                island.rotation_deg = *edit.rotation_deg;
            if (edit.scale)
                island.scale = *edit.scale;
        }
        if (!islands_equal(was, layer.islands))
            kinds.insert("islands");
    }
    for (const auto& [chart, to] : req.join) {
        if (!fits(chart) || (to && !fits(*to))) {
            error = chart_error("join", !fits(chart) ? chart : *to, new_charts);
            return std::nullopt;
        }
        std::optional<std::pair<int, TextureIsland>> target;
        if (to) {
            TextureIsland placed;
            if (*to == chart || !join_chart_placement(unwrap, layer.islands, chart, *to, placed)) {
                error = "join: charts " + std::to_string(chart) + " and " + std::to_string(*to) +
                        " share no mesh edge, so one cannot be placed against the other (get_texture_unwrap lists each chart's neighbours)";
                return std::nullopt;
            }
            target = std::make_pair(*to, placed);
        } else {
            target = join_target(unwrap, layer.islands, chart);
            if (!target) {
                error = "join: chart " + std::to_string(chart) + " has no neighbour it shares an edge with";
                return std::nullopt;
            }
        }
        const auto islands_was = layer.islands;
        const auto groups_was  = layer.island_groups;
        layer.islands[size_t(chart)] = target->second;
        join_island_groups(layer.island_groups, chart, target->first, new_charts);
        if (!islands_equal(islands_was, layer.islands) || !groups_equal(groups_was, layer.island_groups))
            kinds.insert("join");
    }
    for (const int chart : req.unjoin) {
        if (!fits(chart)) {
            error = chart_error("unjoin", chart, new_charts);
            return std::nullopt;
        }
        const auto islands_was = layer.islands;
        const auto groups_was  = layer.island_groups;
        layer.islands[size_t(chart)] = TextureIsland{};
        unjoin_island(layer.island_groups, chart, new_charts);
        if (!islands_equal(islands_was, layer.islands) || !groups_equal(groups_was, layer.island_groups))
            kinds.insert("unjoin");
    }
    if (req.average_scale) {
        const auto was = layer.islands;
        average_island_scales(layer.islands);
        if (!islands_equal(was, layer.islands))
            kinds.insert("average_scale");
    }

    // Vertex edits.
    if (req.clear_uv_edits && !layer.lscm_uv_overrides.empty()) {
        layer.lscm_uv_overrides.clear();
        unwrap = layer_unwrap(patch, layer);
        kinds.insert("clear_uv_edits");
    }
    if (!req.uv_edits.empty()) {
        const auto was = layer.lscm_uv_overrides;
        for (const auto& [vertex, uv] : req.uv_edits) {
            if (size_t(vertex) >= unwrap.uvs.size()) {
                error = "uv_edits: vertex " + std::to_string(vertex) + " is not in this unwrap, which has " + std::to_string(unwrap.uvs.size()) +
                        " (get_texture_unwrap with include_vertices lists them)";
                return std::nullopt;
            }
            unwrap.uvs[size_t(vertex)] = uv;
            const int key = lscm_uv_override_key(vertex);
            auto      it  = std::find_if(layer.lscm_uv_overrides.begin(), layer.lscm_uv_overrides.end(),
                                         [key](const std::pair<int, Vec2f>& p) { return p.first == key; });
            if (it != layer.lscm_uv_overrides.end())
                it->second = uv;
            else
                layer.lscm_uv_overrides.emplace_back(key, uv);
        }
        if (layer.lscm_uv_overrides != was)
            kinds.insert("uv_edits");
    }

    out.changed = !same_unwrap_state(before, layer);
    if (!out.changed) {
        out.layer = before;
        out.resolved = false;
    } else {
        out.snapshot = snapshot_of(kinds);
        const bool joined = std::any_of(before.island_groups.begin(), before.island_groups.end(), [](int g) { return g >= 0; });
        if (out.resolved && (joined || !before.lscm_uv_overrides.empty()))
            out.notes.push_back("The seams or the seam angle changed, so the charts are numbered anew: island joins and vertex edits made before still "
                                "name the old numbers (unjoin and clear_uv_edits drop them)");
    }
    out.unwrap = std::move(unwrap);
    return out;
}

// ---- Answers ----

json layer_json(const TextureDisplacementLayer& layer, int position, bool base, const LayerTexture& texture, std::optional<bool> color_capable,
                const std::vector<MaskReport>& paint, bool owner_has_uv, const std::vector<TextureDisplacementLayer>& stack,
                const TextureDisplacementOptions& options, const Transform3d& volume_to_world)
{
    json out = {{"layer", layer.slot},
                {"position", position},
                {"base", base},
                {"image",
                 {{"texture", texture.id ? json(*texture.id) : json(nullptr)},
                  {"name", layer.name},
                  {"source", texture.source},
                  {"path", layer.path}}},
                {"color_capable", color_capable ? json(*color_capable) : json(nullptr)},
                {"depth_mm", layer.depth_mm},
                {"tile_size_mm", layer.tiling_scale},
                {"rotation_deg", layer.rotation_deg},
                {"offset", {layer.offset.x(), layer.offset.y()}},
                {"invert", layer.invert},
                {"midlevel", layer.midlevel},
                {"smoothing", layer.smoothing},
                {"edge_fade", layer.edge_smoothing},
                {"edge_fade_amount", layer.edge_smoothing_amount},
                {"tile", layer.tile_enabled},
                {"tile_method", tile_method_name(layer.tile_method)},
                {"projection", projection_name(layer.projection_method)},
                {"blend", blend_name(layer.blend_mode)},
                {"color", layer.color_enabled}};
    if (layer.projection_method == TextureProjectionMethod::ViewProjected) {
        // In plate axes, the shape set_texture_displacement's view_axes takes: the stored axes are the volume's.
        const Matrix3d to_world = volume_to_world.matrix().block<3, 3>(0, 0);
        const Vec3d    right = (to_world * layer.view_project_right.cast<double>()).normalized();
        const Vec3d    up    = (to_world * layer.view_project_up.cast<double>()).normalized();
        out["view_axes"] = {{"right", {right.x(), right.y(), right.z()}}, {"up", {up.x(), up.y(), up.z()}}};
        if (layer.view_project_projective)
            out["projection_frame_active"] = true;
    }
    if (layer.projection_method == TextureProjectionMethod::LSCM)
        out["unwrap"] = {{"seam_angle_deg", layer.lscm_seam_angle_deg},
                         {"auto_connect_islands", layer.auto_connect_islands},
                         {"seams", layer.lscm_seam_edges.size()},
                         {"placed_islands", layer.islands.size()},
                         {"uv_edits", layer.lscm_uv_overrides.size()}};
    json masks   = json::array();
    bool painted = false;
    for (const MaskReport& m : paint) {
        const bool any = m.painted_facets > 0;
        painted |= any;
        masks.push_back({{"volume_id", m.volume_id},
                         {"name", m.name},
                         {"painted", any},
                         {"stale", m.stale},
                         {"painted_facets", m.painted_facets},
                         {"coverage_percent", std::round(m.area_ratio * 10000.) / 100.}});
    }
    out["paint"]             = masks;
    out["painted"]           = painted;
    out["inactive_settings"] = inactive_settings(stack, layer, options, owner_has_uv, color_capable.value_or(false));
    out["warnings"]          = layer_warnings(layer);
    return out;
}

StackAnswer texture_stack_json(std::vector<StackLayerFacts> layers, const std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>>& reports,
                               std::optional<int> only_volume, bool owner_has_uv, const TextureDisplacementOptions& options,
                               const Transform3d& volume_to_world)
{
    std::sort(layers.begin(), layers.end(), [](const StackLayerFacts& a, const StackLayerFacts& b) { return a.layer.slot < b.layer.slot; });
    std::vector<TextureDisplacementLayer> stack;
    for (const StackLayerFacts& facts : layers)
        stack.push_back(facts.layer);

    StackAnswer answer;
    json        rows = json::array();
    for (size_t position = 0; position < layers.size(); ++position) {
        const StackLayerFacts&  facts = layers[position];
        const size_t            slot  = size_t(std::clamp(facts.layer.slot, 0, int(TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1));
        std::vector<MaskReport> shown;
        bool                    painted_anywhere = false;
        for (const auto& row : reports) {
            painted_anywhere |= row[slot].painted_facets > 0;
            if (!only_volume || row[slot].volume_id == *only_volume)
                shown.push_back(row[slot]);
        }
        if (!painted_anywhere && !answer.first_unpainted_layer)
            answer.first_unpainted_layer = facts.layer.slot;
        rows.push_back(layer_json(facts.layer, int(position), position == 0, facts.texture, facts.color_capable, shown, owner_has_uv, stack, options,
                                  volume_to_world));
    }
    answer.json = {{"layers", rows},
                   {"free_layers", int(TEXTURE_DISPLACEMENT_MAX_LAYERS) - int(layers.size())},
                   {"needs_bake", needs_bake(stack, reports)},
                   {"stack_options", stack_options_json(options)}};
    return answer;
}

json stack_options_json(const TextureDisplacementOptions& o)
{
    return {{"pipeline", o.pipeline_v2 ? "default" : "classic"},
            {"displace_border", o.displace_border},
            {"smooth",
             {{"enabled", o.smooth_enabled},
              {"strength", std::round(o.smooth_strength * 1000.f) / 10.f},
              {"passes", o.smooth_iterations},
              {"ignore_outer_ring", o.smooth_skip_border}}},
            {"resolution_mm", o.v2_refine_mm > 0.f ? json(o.v2_refine_mm) : json(nullptr)},
            {"budget_k", o.v2_max_triangles_k >= 0 ? json(o.v2_max_triangles_k) : json(nullptr)},
            {"color_mix", o.color_mix_enabled},
            {"color_mix_count", o.color_mix_count},
            {"color_denoise", o.color_despeckle}};
}

// ---- The bake and the mesh preparation ----

const std::vector<SettingBound>& bake_setting_bounds()
{
    static const std::vector<SettingBound> bounds = {
        {"resolution_mm", 0.02, 2., "mm", "Resolution"},
        {"budget_k", 0., 4000., "", "Budget"},
        {"classic_budget_k", 10., 2000., "", "Triangles"},
        {"strength", 1., 100., "percent", "Strength"},
        {"passes", 1., 10., "", "Passes"},
        {"color_mix_count", 1., 32., "", "Mixed colors"},
        {"color_denoise", 0., 6., "", "Denoise"},
    };
    return bounds;
}

const SettingBound* bake_setting_bound(const std::string& key)
{
    for (const SettingBound& b : bake_setting_bounds())
        if (key == b.key)
            return &b;
    return nullptr;
}

const std::vector<SettingBound>& prepare_setting_bounds()
{
    static const std::vector<SettingBound> bounds = {
        {"target_edge_mm", 0.1, 20., "mm", "Target edge"},
        {"sharp_angle_deg", 5., 90., "degrees", "Keep sharp edges' angle"},
        {"steps", 1., 5., "", "Steps"},
        {"max_edge_mm", 0.001, 20., "mm", "Max edge"},
        {"detail_mm", 0.001, 1., "mm", "Detail"},
        {"min_edge_mm", 0.001, 20., "mm", "Min edge"},
        {"edge_detail_mm", 0., 5., "mm", "Edge detail"},
        {"color_detail_mm", 0., 5., "mm", "Color detail"},
        {"budget_k", 10., 2000., "", "Triangles"},
        {"strength", 1., 100., "percent", "Strength"},
        {"passes", 1., 10., "", "Passes"},
    };
    return bounds;
}

const SettingBound* prepare_setting_bound(const std::string& key)
{
    for (const SettingBound& b : prepare_setting_bounds())
        if (key == b.key)
            return &b;
    return nullptr;
}

namespace {

std::string range_text(const SettingBound& b)
{
    return "from " + number_text(b.min) + " to " + number_text(b.max) + (*b.unit ? std::string(" ") + b.unit : std::string()) + " (the panel's " +
           b.panel_label + ")";
}

// A number held to `b`, as a float.
bool bounded_number(Reader& r, const char* key, const SettingBound& b, std::optional<float>& out)
{
    if (!r.has(key))
        return true;
    double v = 0;
    if (!r.number(key, v))
        return false;
    if (v < b.min || v > b.max)
        return r.fail(std::string(key) + " must be " + range_text(b) + ", not " + number_text(v));
    out = float(v);
    return true;
}

bool bounded_whole(Reader& r, const char* key, const SettingBound& b, std::optional<int>& out)
{
    if (!r.has(key))
        return true;
    if (!r.obj.at(key).is_number_integer())
        return r.fail(std::string(key) + " must be an integer");
    const long long v = r.obj.at(key).get<long long>();
    if (double(v) < b.min || double(v) > b.max)
        return r.fail(std::string(key) + " must be " + range_text(b) + ", not " + std::to_string(v));
    out = int(v);
    return true;
}

// The smoothing settings in `obj` (a bake's `smooth` object, or a smooth's own arguments): strength, passes, ignore_outer_ring.
bool read_smooth_numbers(Reader& r, SmoothRequest& out)
{
    return bounded_number(r, "strength", *bake_setting_bound("strength"), out.strength_percent) &&
           bounded_whole(r, "passes", *bake_setting_bound("passes"), out.passes) && r.boolean("ignore_outer_ring", out.ignore_outer_ring);
}

} // namespace

std::optional<BakeRequest> read_bake_request(const json& params, std::string& error)
{
    error.clear();
    if (!params.is_object()) {
        error = "arguments must be an object";
        return std::nullopt;
    }
    BakeRequest req;
    Reader      r{params, error};

    std::optional<std::string> pipeline;
    if (!r.string("pipeline", pipeline))
        return std::nullopt;
    if (pipeline) {
        if (lower(*pipeline) != "default" && lower(*pipeline) != "classic") {
            error = "pipeline must be default or classic, not \"" + *pipeline + "\"";
            return std::nullopt;
        }
        req.classic = lower(*pipeline) == "classic";
    }
    if (r.has("resolution_mm")) {
        const json& v = params.at("resolution_mm");
        if (v.is_null()) {
            req.resolution_auto = true;
        } else if (!v.is_number() || !std::isfinite(v.get<double>())) {
            error = "resolution_mm must be a number, 0 or null (automatic)";
            return std::nullopt;
        } else if (v.get<double>() == 0.) {
            req.resolution_auto = true;
        } else {
            const SettingBound& b = *bake_setting_bound("resolution_mm");
            if (v.get<double>() < b.min || v.get<double>() > b.max) {
                error = "resolution_mm must be " + range_text(b) + ", or 0 or null for automatic, not " + number_text(v.get<double>());
                return std::nullopt;
            }
            req.resolution_mm = float(v.get<double>());
        }
    }
    if (r.has("budget_k")) {
        const json& v = params.at("budget_k");
        if (v.is_null()) {
            req.budget_auto = true;
        } else if (!v.is_number_integer()) {
            error = "budget_k must be an integer, -1 or null (automatic)";
            return std::nullopt;
        } else if (v.get<long long>() == -1) {
            req.budget_auto = true;
        } else {
            const SettingBound& b = *bake_setting_bound("budget_k");
            if (double(v.get<long long>()) < b.min || double(v.get<long long>()) > b.max) {
                error = "budget_k must be " + range_text(b) + ", or -1 or null for automatic, not " + std::to_string(v.get<long long>());
                return std::nullopt;
            }
            req.budget_k = int(v.get<long long>());
        }
    }
    if (!bounded_whole(r, "classic_budget_k", *bake_setting_bound("classic_budget_k"), req.classic_budget_k) ||
        !r.boolean("displace_border", req.displace_border) || !r.boolean("color_mix", req.color_mix) ||
        !bounded_whole(r, "color_mix_count", *bake_setting_bound("color_mix_count"), req.color_mix_count) ||
        !bounded_whole(r, "color_denoise", *bake_setting_bound("color_denoise"), req.color_denoise))
        return std::nullopt;
    if (req.classic_budget_k && req.classic && !*req.classic) {
        error = "classic_budget_k is the classic pipeline's subdivision budget: leave it out for the default pipeline, whose budget is budget_k";
        return std::nullopt;
    }
    if (r.has("smooth")) {
        const json& v = params.at("smooth");
        if (!known_keys(v, "smooth", {"enabled", "strength", "passes", "ignore_outer_ring"}, error))
            return std::nullopt;
        Reader sr{v, error};
        if (!sr.boolean("enabled", req.smooth.enabled) || !read_smooth_numbers(sr, req.smooth))
            return std::nullopt;
    }
    std::optional<bool> preview;
    if (!r.boolean("include_preview", preview))
        return std::nullopt;
    req.include_preview = preview.value_or(false);
    return req;
}

TextureDisplacementOptions bake_options(const TextureDisplacementOptions& stored, const BakeRequest& req)
{
    TextureDisplacementOptions o = stored;
    if (req.classic)
        o.pipeline_v2 = !*req.classic;
    if (req.resolution_auto)
        o.v2_refine_mm = 0.f;
    else if (req.resolution_mm)
        o.v2_refine_mm = *req.resolution_mm;
    if (req.budget_auto)
        o.v2_max_triangles_k = -1;
    else if (req.budget_k)
        o.v2_max_triangles_k = *req.budget_k;
    // The Standard bake's pins, unless the call says otherwise.
    o.displace_border = req.displace_border.value_or(true);
    o.smooth_enabled  = req.smooth.enabled.value_or(false);
    if (req.smooth.strength_percent)
        o.smooth_strength = std::clamp(*req.smooth.strength_percent / 100.f, 0.01f, 1.f);
    if (req.smooth.passes)
        o.smooth_iterations = *req.smooth.passes;
    if (req.smooth.ignore_outer_ring)
        o.smooth_skip_border = *req.smooth.ignore_outer_ring;
    if (req.color_mix)
        o.color_mix_enabled = *req.color_mix;
    if (req.color_mix_count)
        o.color_mix_count = *req.color_mix_count;
    if (req.color_denoise)
        o.color_despeckle = *req.color_denoise;
    return o;
}

bool bake_options_differ(const TextureDisplacementOptions& a, const TextureDisplacementOptions& b)
{
    return a.displace_border != b.displace_border || a.smooth_enabled != b.smooth_enabled || a.smooth_strength != b.smooth_strength ||
           a.smooth_iterations != b.smooth_iterations || a.smooth_skip_border != b.smooth_skip_border || a.pipeline_v2 != b.pipeline_v2 ||
           a.v2_refine_mm != b.v2_refine_mm || a.v2_max_triangles_k != b.v2_max_triangles_k || a.color_mix_enabled != b.color_mix_enabled ||
           a.color_mix_count != b.color_mix_count || a.color_despeckle != b.color_despeckle;
}

int classic_bake_budget_k(const BakeRequest& req) { return req.classic_budget_k.value_or(750); }

namespace {

// The arguments each operation takes, besides object_id and operation.
std::set<std::string> prepare_keys(const PrepareRequest& req)
{
    switch (req.operation) {
    case PrepareOperation::remesh: return {"target_edge_mm", "keep_sharp_edges", "sharp_angle_deg"};
    case PrepareOperation::smooth: return {"strength", "passes", "ignore_outer_ring"};
    case PrepareOperation::subdivide:
        if (req.uniform())
            return {"adaptive", "steps"};
        return {"adaptive", "max_edge_mm", "detail_mm", "min_edge_mm", "edge_detail_mm", "color_detail_mm", "follow_texture", "budget_k"};
    }
    return {};
}

std::string operation_name(PrepareOperation op)
{
    return op == PrepareOperation::remesh ? "remesh" : op == PrepareOperation::subdivide ? "subdivide" : "smooth";
}

} // namespace

std::optional<PrepareRequest> read_prepare_request(const json& params, std::string& error)
{
    error.clear();
    if (!params.is_object()) {
        error = "arguments must be an object";
        return std::nullopt;
    }
    PrepareRequest req;
    Reader         r{params, error};

    std::optional<std::string> operation;
    if (!r.string("operation", operation))
        return std::nullopt;
    if (!operation) {
        error = "operation is required: remesh, subdivide or smooth";
        return std::nullopt;
    }
    const std::string name = lower(*operation);
    if (name == "remesh")
        req.operation = PrepareOperation::remesh;
    else if (name == "subdivide")
        req.operation = PrepareOperation::subdivide;
    else if (name == "smooth")
        req.operation = PrepareOperation::smooth;
    else {
        error = "operation must be remesh, subdivide or smooth, not \"" + *operation + "\"";
        return std::nullopt;
    }
    if (!r.boolean("adaptive", req.adaptive))
        return std::nullopt;

    // An argument of another operation would be ignored: refused instead.
    static const std::set<std::string> all_keys = {"target_edge_mm", "keep_sharp_edges", "sharp_angle_deg", "adaptive", "steps", "max_edge_mm",
                                                   "detail_mm", "min_edge_mm", "edge_detail_mm", "color_detail_mm", "follow_texture", "budget_k",
                                                   "strength", "passes", "ignore_outer_ring"};
    const std::set<std::string> mine = prepare_keys(req);
    for (const std::string& key : all_keys)
        if (r.has(key.c_str()) && !mine.count(key)) {
            std::string list;
            for (const std::string& k : mine)
                list += (list.empty() ? "" : ", ") + k;
            error = key + " does not apply to " + (req.uniform() ? "a uniform subdivide (adaptive: false)" : operation_name(req.operation)) +
                    ", which takes " + list;
            return std::nullopt;
        }

    const auto b = [](const char* key) -> const SettingBound& { return *prepare_setting_bound(key); };
    if (!bounded_number(r, "target_edge_mm", b("target_edge_mm"), req.target_edge_mm) || !r.boolean("keep_sharp_edges", req.keep_sharp_edges) ||
        !bounded_number(r, "sharp_angle_deg", b("sharp_angle_deg"), req.sharp_angle_deg) ||
        !bounded_whole(r, "steps", b("steps"), req.steps) || !bounded_number(r, "max_edge_mm", b("max_edge_mm"), req.max_edge_mm) ||
        !bounded_number(r, "detail_mm", b("detail_mm"), req.detail_mm) || !bounded_number(r, "min_edge_mm", b("min_edge_mm"), req.min_edge_mm) ||
        !bounded_number(r, "edge_detail_mm", b("edge_detail_mm"), req.edge_detail_mm) ||
        !bounded_number(r, "color_detail_mm", b("color_detail_mm"), req.color_detail_mm) || !r.boolean("follow_texture", req.follow_texture) ||
        !bounded_whole(r, "budget_k", b("budget_k"), req.budget_k) || !read_smooth_numbers(r, req.smooth))
        return std::nullopt;
    if (req.uniform() && !req.steps) {
        error = "a uniform subdivide (adaptive: false) needs steps, 1 to 5";
        return std::nullopt;
    }
    return req;
}

TextureDisplacementPrepareParams remesh_prepare_params(const PrepareRequest& req, float default_edge_mm)
{
    TextureDisplacementPrepareParams p;
    p.remesh_edge_mm   = req.target_edge_mm.value_or(default_edge_mm);
    p.remesh_sharp_deg = req.keep_sharp_edges.value_or(true) ? req.sharp_angle_deg.value_or(40.f) : 0.f;
    return p;
}

TextureDisplacementPrepareParams subdivide_prepare_params(const PrepareRequest& req, float default_edge_mm)
{
    TextureDisplacementPrepareParams p;
    p.subdiv_target_mm       = req.max_edge_mm.value_or(default_edge_mm);
    p.subdiv_detail_mm       = req.detail_mm.value_or(0.05f);
    p.subdiv_min_edge_mm     = req.min_edge_mm.value_or(0.1f);
    p.subdiv_border_mm       = req.edge_detail_mm.value_or(0.4f);
    p.subdiv_color_edge_mm   = req.color_detail_mm.value_or(0.3f);
    p.subdiv_feature         = req.follow_texture.value_or(false);
    p.subdiv_added_triangles = req.budget_k.value_or(750) * 1000;
    return p;
}

namespace {
float mean_edge_mm(const indexed_triangle_set& its)
{
    double sum = 0.;
    size_t cnt = 0;
    for (const stl_triangle_vertex_indices& tri : its.indices)
        for (int i = 0; i < 3; ++i) {
            sum += double((its.vertices[size_t(tri[i])] - its.vertices[size_t(tri[(i + 1) % 3])]).norm());
            ++cnt;
        }
    return cnt > 0 ? float(sum / double(cnt)) : 0.f;
}
} // namespace

float default_remesh_edge_mm(const indexed_triangle_set& its)
{
    const float mean = mean_edge_mm(its);
    return mean > 0.f ? std::clamp(mean, 0.1f, 20.f) : 1.f;
}

float default_subdivide_edge_mm(const indexed_triangle_set& its)
{
    const float mean = mean_edge_mm(its);
    return mean > 0.f ? std::clamp(mean * 0.5f, 0.001f, 20.f) : 1.f;
}

TextureDisplacementOptions smooth_options(const TextureDisplacementOptions& stored, const SmoothRequest& req)
{
    TextureDisplacementOptions o = stored;
    if (req.strength_percent)
        o.smooth_strength = std::clamp(*req.strength_percent / 100.f, 0.01f, 1.f);
    if (req.passes)
        o.smooth_iterations = *req.passes;
    if (req.ignore_outer_ring)
        o.smooth_skip_border = *req.ignore_outer_ring;
    return o;
}

std::optional<std::string> uniform_subdivision_refusal(std::size_t facets, int steps)
{
    double result = double(facets);
    for (int i = 0; i < steps; ++i)
        result *= 4.;
    if (result <= double(UNIFORM_SUBDIVISION_MAX_FACETS))
        return std::nullopt;
    return "Subdividing " + std::to_string(facets) + " triangles " + std::to_string(steps) + (steps == 1 ? " step" : " steps") +
           " would make " + number_text(std::round(result)) + " (each step quadruples the count), more than the " +
           std::to_string(UNIFORM_SUBDIVISION_MAX_FACETS) + " one call allows: use fewer steps, or adaptive subdivision, which refines the painted area only";
}

std::optional<std::string> smooth_refusal(std::size_t facets)
{
    if (facets <= SMOOTH_MAX_FACETS)
        return std::nullopt;
    return "The part has " + std::to_string(facets) + " triangles, more than the " + std::to_string(SMOOTH_MAX_FACETS) +
           " a smooth is run on: it runs on the app's main thread, where it would hold the app too long";
}

json bake_inactive_settings(const BakeRequest& req, bool classic)
{
    json out = json::object();
    if (classic) {
        if (req.resolution_mm || req.resolution_auto)
            out["resolution_mm"] = "the classic pipeline's refinement is classic_budget_k's, so the default pipeline's resolution is stored but not used";
        if (req.budget_k || req.budget_auto)
            out["budget_k"] = "the classic pipeline's budget is classic_budget_k, so the default pipeline's budget is stored but not used";
    } else if (req.classic_budget_k) {
        out["classic_budget_k"] = "the default pipeline refines to resolution_mm and budget_k; classic_budget_k is the classic pipeline's";
    }
    return out;
}

std::string bake_nothing_painted_message(int object_id)
{
    return "Object " + std::to_string(object_id) + " has no texture paint, so there is nothing to bake: paint_object mode texture paints a layer";
}

std::string bake_no_displacement_message()
{
    return "The bake produced no displacement, so nothing was changed and the paint was kept. Check that the painted layer has a texture and a non-zero depth.";
}

std::string bake_timed_out_message(double wait_s, bool classic)
{
    return "The bake did not finish within this call's wait (" + number_text(std::round(wait_s)) + " s), so nothing was changed. Raise ORCAMCP_TIMEOUT, or " +
           (classic ? "lower classic_budget_k, or " : "") +
           "ask for less detail: a larger resolution_mm or a smaller budget_k";
}

std::string prepare_timed_out_message(double wait_s)
{
    return "The preparation did not finish within this call's wait (" + number_text(std::round(wait_s)) +
           " s), so nothing was changed. Raise ORCAMCP_TIMEOUT, or ask for less: a larger edge length or a smaller budget_k";
}

std::string bake_undo_warning(int slots_added)
{
    return "The bake added " + std::to_string(slots_added) + " mixed filament slot" + (slots_added == 1 ? "" : "s") +
           ", which an undo does not remove (filament slots are not undone): delete_mixed_filament removes one";
}

std::string prepare_unchanged_message(const PrepareRequest& req)
{
    switch (req.operation) {
    case PrepareOperation::remesh:
        return "Remeshing did not change the model. It may be non-manifold (open edges or edges shared by more than two triangles), or the target "
               "edge length may already be met.";
    case PrepareOperation::subdivide:
        return "Nothing to subdivide - the painted area already meets the target edge length and detail tolerance, or the triangle budget is already used up.";
    case PrepareOperation::smooth:
        return "Nothing to smooth.";
    }
    return {};
}

std::string prepare_paint_lost_message()
{
    return "The painted area could not be carried onto the remeshed model, so nothing was changed";
}

json bake_part_json(const BakePartReport& p)
{
    return {{"volume_id", p.volume_id}, {"name", p.name}, {"facets_before", p.facets_before}, {"facets_after", p.facets_after},
            {"baked", p.baked}, {"colored", p.colored}};
}

json bake_budget_json(std::size_t needed, std::size_t kept)
{
    if (needed == 0)
        return nullptr;
    return {{"needed", needed}, {"kept", kept}};
}

std::string bake_budget_warning(std::size_t needed, std::size_t kept)
{
    if (needed == 0)
        return {};
    return "The triangle budget cut the detail: " + std::to_string(kept) + " triangles kept of " + std::to_string(needed) +
           " needed. Detail is thinned, and the relief can come out noisy or moire, with a band of lower vertex density where a long thin "
           "input triangle was split (the library undersamples and decimates long triangles). Refine the long triangles first "
           "(prepare_texture_mesh) or bake again with a larger budget_k.";
}

float texture_feature_mm(const std::vector<uint8_t>& pixels, int width, int height, float mm_u, float mm_v)
{
    if (width < 4 || height < 4 || pixels.size() < size_t(width) * size_t(height) || mm_u <= 0.f || mm_v <= 0.f)
        return 0.f;
    double sum = 0.;
    for (size_t i = 0; i < size_t(width) * size_t(height); ++i)
        sum += pixels[i];
    const double mean = sum / double(size_t(width) * size_t(height));
    std::vector<std::pair<float, float>> runs; // length in mm, weight
    const auto scan = [&](int lines, int length, int line_stride, int pixel_stride, float mm, int max_lines) {
        const int step = std::max(1, lines / max_lines);
        for (int line = 0; line < lines; line += step) {
            const uint8_t* row = pixels.data() + size_t(line) * size_t(line_stride);
            int            start = 0;
            bool           above = row[0] > mean;
            for (int i = 1; i <= length; ++i) {
                const bool end = i == length;
                if (!end && (row[size_t(i) * size_t(pixel_stride)] > mean) == above)
                    continue;
                // Runs cut by the image's edge are only part of a feature: left out.
                if (start > 0 && !end)
                    runs.emplace_back(float(i - start) * mm, float(i - start) * mm);
                start = i;
                if (!end)
                    above = !above;
            }
        }
    };
    scan(height, width, width, 1, mm_u, 256);
    scan(width, height, 1, width, mm_v, 256);
    if (runs.empty())
        return 0.f;
    std::sort(runs.begin(), runs.end());
    double total = 0.;
    for (const auto& r : runs)
        total += r.second;
    double seen = 0.;
    for (const auto& r : runs) {
        seen += r.second;
        if (seen >= 0.1 * total)
            return r.first;
    }
    return runs.back().first;
}

ResolutionAdvice resolution_advice_from(float feature_mm, float used_mm)
{
    ResolutionAdvice out;
    out.used_mm = used_mm;
    if (feature_mm <= 0.f)
        return out;
    out.feature_mm     = feature_mm;
    out.recommended_mm = std::max(0.05f, std::round(0.5f * feature_mm * 100.f) / 100.f);
    out.aliasing       = used_mm > out.recommended_mm;
    return out;
}

float model_uv_mm_per_unit(const indexed_triangle_set& mesh, const Transform3d& to_world, const std::vector<Vec2f>& uv)
{
    if (uv.size() != mesh.indices.size() * 3)
        return 0.f;
    double surface = 0., uv_area = 0.;
    for (size_t t = 0; t < mesh.indices.size(); ++t) {
        const stl_triangle_vertex_indices& f = mesh.indices[t];
        const Vec3d a = to_world * mesh.vertices[size_t(f[0])].cast<double>(), b = to_world * mesh.vertices[size_t(f[1])].cast<double>(),
                    c = to_world * mesh.vertices[size_t(f[2])].cast<double>();
        surface += 0.5 * (b - a).cross(c - a).norm();
        const Vec2d u0 = uv[t * 3].cast<double>(), u1 = uv[t * 3 + 1].cast<double>(), u2 = uv[t * 3 + 2].cast<double>();
        uv_area += 0.5 * std::abs((u1 - u0).x() * (u2 - u0).y() - (u1 - u0).y() * (u2 - u0).x());
    }
    return (surface > 0. && uv_area > 1e-12) ? float(std::sqrt(surface / uv_area)) : 0.f;
}

ResolutionAdvice resolution_advice(const std::vector<TextureDisplacementLayer>& layers, const std::vector<AdvicePart>& parts, float chosen_mm)
{
    float automatic = 0.f, feature = 0.f;
    for (const AdvicePart& part : parts)
        if (part.mesh != nullptr)
            automatic = std::max(automatic, Slic3r::recommend_v2_resolution(*part.mesh, {}, part.to_world).edge_mm);
    for (const TextureDisplacementLayer& layer : layers) {
        if (layer.empty() || layer.tiling_scale <= 0.f)
            continue;
        const Slic3r::DecodedHeightTexture& tex = Slic3r::decode_height_texture(layer);
        if (tex.width <= 0 || tex.height <= 0)
            continue;
        const float tiled_u = layer.tiling_scale / float(tex.width);
        const auto  measure = [&](float mm_u, float mm_v) {
            const float f = texture_feature_mm(tex.pixels, tex.width, tex.height, mm_u, mm_v);
            if (f > 0.f && (feature <= 0.f || f < feature))
                feature = f;
        };
        bool by_uv = false;
        if (layer.projection_method == Slic3r::TextureProjectionMethod::ModelUV)
            for (const AdvicePart& part : parts) {
                const float unit = part.mesh != nullptr && part.model_uv != nullptr ? model_uv_mm_per_unit(*part.mesh, part.to_world, *part.model_uv) : 0.f;
                if (unit > 0.f) {
                    measure(unit / float(tex.width), unit / float(tex.height));
                    by_uv = true;
                }
            }
        if (!by_uv)
            measure(tiled_u, tiled_u);
    }
    return resolution_advice_from(feature, chosen_mm > 0.f ? chosen_mm : automatic);
}

json recommended_resolution_json(const ResolutionAdvice& a)
{
    return a.recommended_mm > 0.f ? json(a.recommended_mm) : json(nullptr);
}

std::string resolution_alias_warning(const ResolutionAdvice& a)
{
    if (!a.aliasing)
        return {};
    auto text = [](float v) { std::string t = std::to_string(v); t.erase(t.find_last_not_of('0') + 1); if (!t.empty() && t.back() == '.') t.pop_back(); return t; };
    return "The texture will alias at the resolution used (" + text(a.used_mm) + " mm): its finest common feature is about " + text(a.feature_mm) +
           " mm on the part, so its detail needs about " + text(a.recommended_mm) + " mm. Set resolution_mm to " + text(a.recommended_mm) + " (more triangles: raise budget_k with it) or accept a coarser relief.";
}

json bake_mixed_filaments_json(const std::vector<int>& slots, int filaments_before)
{
    json out = json::array();
    for (const int slot : slots)
        out.push_back({{"slot", slot + 1}, {"created", slot >= filaments_before}});
    return out;
}

}}} // namespace Slic3r::GUI::OrcaMCP
