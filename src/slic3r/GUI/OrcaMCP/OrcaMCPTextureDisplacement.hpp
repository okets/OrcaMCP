// src/slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.hpp
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"

#include <nlohmann/json.hpp>

#include "OrcaMCPPaintModel.hpp"

// The decisions of the texture displacement tools, apart from the app: names, the library by id, one call's
// layer edit (read, refused, applied), the layer masks as reports, what a mesh edit dropped, the unwrap
// request and the answers' shapes. Owner lookups are the texture panel's (TextureDisplacementObject.hpp)
// and come in as arguments. No wx, no wxGetApp, no texture_library(); file I/O only where marked.
namespace Slic3r { namespace GUI { namespace OrcaMCP {

// ---- Names ----

// triplanar cylindrical spherical unwrap view model_uv (the enum's order).
const std::vector<std::string>&        projection_names();
std::optional<TextureProjectionMethod> projection_named(const std::string&);
std::string                            projection_name(TextureProjectionMethod);
// add override subtract multiply divide (the panel's combo order).
const std::vector<std::string>& blend_names();
std::optional<TextureBlendMode> blend_named(const std::string&);
std::string                     blend_name(TextureBlendMode);
// repeat mirrored_repeat
const std::vector<std::string>& tile_method_names();
std::optional<TextureTileMethod> tile_method_named(const std::string&);
std::string                      tile_method_name(TextureTileMethod);
// current iso top bottom front back left right
const std::vector<std::string>& view_direction_names();
// builtin | user | pack
const char* texture_source_name(TextureSource);

// ---- The library, by id ----

// "builtin/Bark.png", "user/My bricks.png", "pack/<pack name>/<file name>": the file name with its
// extension. Ids still equal get " #2", " #3" in library order.
std::vector<std::string> texture_ids(const std::vector<TextureLibraryEntry>&);

struct TextureLookup
{
    int         index = -1;
    std::string error;
};
// An id, or a stem (any case) exactly one entry has; ambiguous lists the ids; unknown points at
// get_texture_library.
TextureLookup find_texture(const std::vector<TextureLibraryEntry>&, const std::vector<std::string>& ids, const std::string& query);
// The entry a layer's image came from, by path.
std::optional<int> library_index_of(const std::vector<TextureLibraryEntry>&, const TextureDisplacementLayer&);

// A PNG's colour as decode_height_texture reads it, from its header: false for an 8-bit grayscale, true for
// the 8-bit RGB and RGBA it takes as colour, nullopt for anything it cannot decode (a jpg in the user folder,
// a palette, grayscale-with-alpha or 16-bit PNG).
std::optional<bool> png_has_color(const unsigned char* bytes, std::size_t size);
// A PNG's width and height from its header, nullopt for anything else.
std::optional<std::pair<int, int>> png_dimensions(const unsigned char* bytes, std::size_t size);
// I/O: the first 33 bytes of the file.
std::optional<std::pair<int, int>> png_dimensions_from_file(const std::string& path);
// An image stored as RGB whose every pixel is grey is a grayscale texture: the import stores it as one
// (the library's image_has_color), so a candidate is compared and checked as the file it would become.
// The height becomes the grey value and the colour channels go.
void drop_grey_colour(DecodedHeightTexture&);
// The most pixels a side of an image given to a layer may have: larger is refused, not decoded.
constexpr int max_texture_side_px = 8192;
std::optional<bool> png_color_from_file(const std::string& path);
// {id, name, source, pack?, file, path, color (true / false / null: not read)}
nlohmann::json library_entry_json(const TextureLibraryEntry&, const std::string& id, std::optional<bool> color);
// get_texture_library's list: {textures [library_entry_json], counts {builtin, user, pack} (of the whole library,
// whatever `source` filters), shown}. `colors` runs parallel to `entries`. `source` is builtin, user or pack, or
// nullopt for all; anything else is refused through `error`.
nlohmann::json library_list_json(const std::vector<TextureLibraryEntry>& entries, const std::vector<std::string>& ids,
                                 const std::vector<std::optional<bool>>& colors, const std::optional<std::string>& source,
                                 std::string& error);
// Why delete_user_texture refuses `entry` (id `id`): only an imported texture can go.
std::optional<std::string> user_texture_delete_refusal(const TextureLibraryEntry& entry, const std::string& id);

// Identity of an image for the import's reuse: width, height, the height pixels, and the colour pixels when
// some pixel is not grey. Two encodings of one picture (an RGB PNG of grey pixels and its grayscale PNG, a
// jpg and its PNG) are the same texture; a hash match is confirmed by comparing the decoded data.
struct TextureIdentity
{
    std::uint64_t hash   = 0;
    int           width  = 0;
    int           height = 0;
};
TextureIdentity texture_identity(const DecodedHeightTexture&);
bool            same_texture(const DecodedHeightTexture&, const DecodedHeightTexture&);

// ---- One call's layer edit ----

struct ImageAdjustRequest
{
    std::optional<float> brightness, contrast, sharpness, blur, hue_deg;
};
// An orthographic frame through which the texture is projected once.
struct ProjectionFrameRequest
{
    std::optional<std::string> view;       // a named view (view_direction_names), or right/up
    std::optional<Vec3d>       right, up;  // plate axes, unit after normalising, not parallel
    Vec3d                      center = Vec3d::Zero(); // plate mm
    double                     width_mm = 0, height_mm = 0; // > 0
};
// Every field optional: only what the call gives.
struct LayerSettings
{
    std::optional<float>                     depth_mm, tile_size_mm, rotation_deg, midlevel, smoothing, edge_fade_amount;
    std::optional<Vec2f>                     offset;
    std::optional<bool>                      invert, edge_fade, tile, color;
    std::optional<TextureTileMethod>         tile_method;
    std::optional<TextureProjectionMethod>   projection;
    std::optional<TextureBlendMode>          blend;
    std::optional<std::string>               view_direction;  // current | iso | top | bottom | front | back | left | right
    std::optional<std::pair<Vec3d, Vec3d>>  view_axes;       // explicit right/up in plate axes, instead of view_direction
    std::optional<ProjectionFrameRequest>    projection_frame;
    bool                                     clear_projection_frame = false; // projection_frame: false
    std::optional<ImageAdjustRequest>        adjust_image;
    std::optional<bool>                      color_mix;      // stack-wide from here
    std::optional<int>                       color_mix_count, color_denoise;
    bool empty() const;
};

enum class LayerAction { add, edit, remove, move };
struct LayerRequest
{
    LayerAction                action = LayerAction::edit;
    std::optional<int>         layer, to_position; // slot; move: 0-based position in slot order
    std::optional<std::string> texture, file_path;
    LayerSettings              settings;
};

// The bounds every number is held to, one table for read_layer_request and the schema's minimum/maximum.
// `key` is the argument's name (an adjust_image field by its own name).
struct SettingBound
{
    const char* key;
    double      min, max;
    const char* unit;        // "" when unitless
    const char* panel_label; // the panel's name for it
};
const std::vector<SettingBound>& layer_setting_bounds();
const SettingBound*              setting_bound(const std::string& key);

// Types, names, ranges and structure: action required; edit, remove and move need layer (0..7); add takes
// none (it takes the first free slot, as the panel does); remove takes nothing else; move takes to_position
// and nothing else; to_position only with move; texture and file_path not both; offset two finite numbers;
// view_direction and view_axes not both; a frame's width and height > 0, its axes not parallel and given as
// a view or as right and up; adjust_image with at least one field; projection_frame true refused (false
// clears). Returns nullopt with `error` set otherwise. `object_id` is not read.
std::optional<LayerRequest> read_layer_request(const nlohmann::json& params, std::string& error);

struct StackFacts
{
    bool owner_has_uv = false;
    int  physical_filaments = 1;
    bool texture_has_color = false;
    int  color_mix_count = -1; // the stack's value now, -1 when not known
};
// Refusals that need the stack: no free slot; no layer in that slot (lists the slots); colour on grayscale;
// model_uv without UV; color_mix_count above max(1, 32 - physical_filaments); view_direction / view_axes when
// the projection after the call is not view; move to_position out of range; adjust_image on a layer with no
// image. `after` is the layer with the request's settings applied (for add, the new layer); `before` is the
// layer as it is now (nullptr for add). Colour, model_uv and the mixed-colour count are refused only when they
// would change what the layer or the stack holds, so a value read back from a get is always accepted.
std::optional<std::string> layer_edit_refusal(int object_id, const std::vector<TextureDisplacementLayer>& stack, const LayerRequest&,
                                              const TextureDisplacementLayer* before, const TextureDisplacementLayer& after,
                                              const StackFacts&);
std::optional<int>         first_free_slot(const std::vector<TextureDisplacementLayer>&);
// The layer in `slot`'s 0-based position in slot order (how the panel lists them), nullopt without one.
std::optional<int> layer_position(const std::vector<TextureDisplacementLayer>&, int slot);
// The insertion index move_texture_layer takes (it counts the moved layer's own place) that puts a layer now at
// position `from` at position `to`.
int move_insertion_index(int from, int to);
// add_texture_layer's defaults: the slot, and ModelUV when the owner has a UV channel. No image.
TextureDisplacementLayer new_layer(int slot, bool owner_has_uv);
// The field mapping; true when something changed. The settings that need the model or the camera
// (view_direction, view_axes, projection_frame, adjust_image) are the caller's; clear_projection_frame is
// the panel's Clear: view_project_projective false and tile_enabled true.
bool apply_layer_settings(TextureDisplacementLayer&, const LayerSettings&);
bool apply_stack_settings(TextureDisplacementOptions&, const LayerSettings&);
TextureAdjustments adjustments_of(const ImageAdjustRequest&);
// capture_view_projection's arithmetic: the projector axes in the volume's frame from world right/up and the
// volume's transform (unit, or the x/y axis when the transform collapses one).
std::pair<Vec3f, Vec3f> view_projector_axes(const Vec3d& right_world, const Vec3d& up_world, const Transform3d& volume_to_world);
// An orthographic projection frame as the row-major 3x4 view_project_matrix in the volume's frame:
// uv = ((p - corner) . right / width, (p - corner) . down / height), row 2 = (0, 0, 0, 1), where corner is the
// frame's top left and v runs down the image, as apply_projection_frame's window does. `up` is made
// perpendicular to `right`. project_uv_projective reads it back.
std::array<float, 12> orthographic_frame_matrix(const Vec3d& right_world, const Vec3d& up_world, const Vec3d& center_world,
                                                double width_mm, double height_mm, const Transform3d& volume_to_world);
// {setting: why} for what the panel greys out but stores: tile_size_mm under model_uv, blend on the base
// layer, edge_fade_amount with fade off, tile_method with tile off, colour options with no layer colouring,
// colour on a texture without colours, placement under a projection frame, model_uv without UV.
nlohmann::json inactive_settings(const std::vector<TextureDisplacementLayer>& stack, const TextureDisplacementLayer&,
                                 const TextureDisplacementOptions&, bool owner_has_uv, bool texture_has_color);
// midlevel > 0 and depth > 1 mm: the panel's deep inward displacement warning.
std::vector<std::string> layer_warnings(const TextureDisplacementLayer&);

// ---- Masks ----

// A mask indexing a triangle the mesh no longer has: the panel treats it as unreadable.
bool mask_is_stale(const PaintData&, std::size_t facet_count);
struct MaskInput
{
    int                                 volume_id = -1;
    std::string                         name;
    std::shared_ptr<const TriangleMesh> mesh;
    std::array<PaintData, TEXTURE_DISPLACEMENT_MAX_LAYERS> masks;
};
struct MaskReport
{
    int         volume_id = -1;
    std::string name;
    bool        stale = false;
    int         painted_facets = 0;
    double      area_ratio = 0.;
};
// Any thread, state 1 only: one report per slot for each part.
// `slots`: report only those (the others stay unbuilt); null reports all eight.
std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>> mask_reports(const std::vector<MaskInput>&, const std::vector<int>* slots = nullptr);
// One part's masks and mesh, copied (volume_id: its position among the object's volumes). `slots`: copy only those
// masks (the others stay empty); null copies all eight.
MaskInput mask_input_of(const ModelVolume&, int volume_id, const std::vector<int>* slots = nullptr);
// Some mask of the part holds paint (a readable state 1): worth copying for a facet pick.
bool has_texture_paint(const ModelVolume&);
// The slots whose mask paints original facet `facet` of `input`'s mesh, wholly or in part: read from the facet's own leaf
// states (the mask must be readable and hold state 1; stale and all-unpainted masks are left out).
std::vector<int> texture_slots_painting_facet(const MaskInput& input, int facet);

// What `mode` and `texture_layer` ask get_object_paint and clear_object_paint for. mode absent: the four paint modes
// (texture masks are not print paint); a mode: that one; texture also names a slot, or without one `stack_slots` (a
// read) or all eight slots (`all_slots_default`, a clear). texture_layer without mode texture is refused.
struct PaintSelection
{
    std::vector<PaintMode> modes;         // the non-texture modes
    bool                   texture = false;
    std::vector<int>       texture_slots; // when texture
    bool                   named_mode = false;
    std::string            label;         // for an undo name: the mode, or "all"
};
bool resolve_paint_selection(const nlohmann::json& params, const std::vector<int>& stack_slots, bool all_slots_default,
                             PaintSelection& out, std::string& error);
// A texture mapping refused: a state above 1 (a mask has 0 unpainted and 1 painted), or one that sends every painted
// state to 0, which leaves split facets serialized (clear_object_paint erases). Empty when fine.
std::string texture_remap_refusal(const std::map<int, int>& mapping);
// The slots of an owner's layers, ascending; none for no owner.
std::vector<int> layer_slots_of(const ModelVolume* owner);
// [{layer, painted_facets, coverage_percent, stale?}] for `slots` from one part's reports; stale only when true.
nlohmann::json texture_paint_layers_json(const std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>& row, const std::vector<int>& slots);
// The same for slots just cleared: painted nowhere, nothing stale.
nlohmann::json texture_paint_layers_cleared_json(const std::vector<int>& slots);
// The texture state an object's description carries, cheap enough for every object of a scene (no selector is built):
// {texture_layers: the owner's layer count, needs_bake: needs_bake_fast, model_uv: some model part has a UV channel}.
nlohmann::json texture_object_fields(const ModelObject&);
// A volume row's: {model_uv: the part has a UV channel, texture_layers: its layer count when it owns the stack (else 0),
// texture_paint_layers: the slots whose mask is not empty}.
nlohmann::json texture_volume_fields(const ModelObject&, std::size_t volume_idx);
// Some layer of the stack is painted, readably, on some part.
bool needs_bake(const std::vector<TextureDisplacementLayer>&, const std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>>&);
// A readable mask (no triangle beyond the mesh's `facet_count`) with state 1 in it: what needs_bake counts as painted.
bool mask_has_paint(const PaintData&, std::size_t facet_count);
// One slot of the above, the cheap form: some model part holds a readable mask with state 1 in that slot.
bool slot_painted_fast(const ModelObject&, int slot);
// The cheap form for every-object reports: FacetsAnnotation::empty() per slot, no selector.
bool needs_bake_fast(const ModelObject&, const ModelVolume* owner);

// ---- What a mesh edit dropped ----

struct TextureState
{
    struct Part
    {
        ObjectID           id;
        const ModelVolume* volume = nullptr; // identity only, to find the part again after an edit: never dereferenced
        int                volume_id = -1;   // position among the object's volumes
        std::string        name;
        std::vector<int>   painted_slots;
        int                layers = 0;       // the part's own stack
        bool               stack_owner = false; // the object reads its layers from this part
        bool               model_part  = false;
        bool               has_uv      = false;
        bool               unwrap_edits = false; // some layer has seams, islands or UV overrides made on the mesh
        std::size_t        vertices = 0, facets = 0;
    };
    std::vector<Part> parts;
    std::size_t       volume_count = 0; // all the object's volumes, the parts above or not
};
TextureState texture_state_of(const ModelObject&);
// Comparing `before` with the objects after (one for most edits, several for a split), a part is found again by
// its volume pointer or its id. Keys, each only when there is something:
//   paint_lost [{volume_id, name, layers: [slots]}], layers_lost n (a part's layers gone with it or its stack),
//   stacks_unused [{volume_id, layers}] (a stack on a part that is not its object's owner; `owner_after` for the
//   first object), uv_lost [{volume_id, name}] (the part's UV channel, which model_uv layers map by),
//   unwrap_edits_stale [{volume_id, name}] (layers kept, with seams / islands / UV overrides made on a mesh that
//   changed: nothing checks them against the new mesh), paint_unused [{volume_id, name, layers}] (paint on a part
//   that is no longer a model part). volume_id is the part's place before. An empty object when nothing was lost.
nlohmann::json texture_changes_json(const TextureState& before, const std::vector<const ModelObject*>& after, const ModelVolume* owner_after);
// A deleted part's paint, UV and unwrap edits are not reported as lost (its layers still are, if it held the stack).
void forget_texture_paint_of(TextureState& state, int volume_id);

// For an edit that puts a new volume in each place (reload, replace): `state`'s parts take the identity of the volume
// now at their position, so texture_changes_json compares what each part holds. Does nothing when the object's volume
// count changed.
void rekey_texture_state(TextureState& state, const ModelObject& after);
// The info_messages lines for a texture_changes answer. `subject` names the object ("object 3"); `cause` says why
// ("the mesh changed, and texture paint is not carried across").
std::vector<std::string> texture_changes_messages(const nlohmann::json& changes, const std::string& subject, const std::string& cause);
// Does nothing for an empty `changes`; else adds texture_changes, appends the lines to info_messages (after the
// guard's report, which would replace them) and the next steps. `subject` defaults to "object <object_id>"; an
// edit whose object is gone names it ("object 3, before the split") while object_id is the one to act on.
void add_texture_changes(nlohmann::json& answer, const nlohmann::json& changes, int object_id, const std::string& cause,
                         const std::string& subject = std::string());
// The same for an edit of several objects: texture_changes is a list of {object_id, ...changes}. One affected object
// gets its lines; several get one summary line, the list telling what each lost, and one next step naming them all.
void add_objects_texture_changes(nlohmann::json& answer, const std::vector<std::pair<int, nlohmann::json>>& changes, const std::string& cause);

// ---- The unwrap request ----

struct IslandEdit
{
    int                  chart = 0;
    std::optional<Vec2f> offset;
    std::optional<float> rotation_deg, scale;
};
struct UnwrapRequest
{
    std::optional<std::string> unwrap_id;      // from get_texture_unwrap: refuse chart / vertex numbers when the unwrap is another
    std::optional<float> seam_angle_deg;       // 5..90 (SetSeamAngle's clamp, refused outside)
    std::optional<bool>  auto_connect_islands;
    std::vector<std::pair<int, int>>                 add_seams, remove_seams; // mesh vertex pairs
    std::vector<std::pair<Vec3d, Vec3d>>             seam_paths;              // plate mm, snapped to the nearest vertices
    std::vector<int>                                 cut_islands, unjoin;
    std::vector<std::pair<int, std::optional<int>>>  join;                    // chart, to (nullopt: the nearest neighbour)
    std::vector<IslandEdit>                          islands;
    std::vector<std::pair<int, Vec2f>>               uv_edits;                // unwrapped vertex, raw unwrap mm
    bool clear_seams = false, clear_uv_edits = false, average_scale = false, relayout = false;
};
// The shapes: add_seams / remove_seams [[v, v]], seam_paths [{from: [x,y,z], to: [x,y,z]}], cut_islands /
// unjoin [chart], join [{chart, to?}], islands [{chart, offset?, rotation_deg?, scale?}] (an entry of unwrap_json's
// islands is taken back: its centroid, group, area_mm2, bounding_box and neighbours are read and ignored),
// uv_edits [{vertex, uv: [u, v]}]; island scale 0.001..1000 (the panel's clamp). The padding between islands is no
// request: the panel and the bake always pack with none.
std::optional<UnwrapRequest> read_unwrap_request(const nlohmann::json& params, std::string& error);

// The most vertices and islands unwrap_json lists.
constexpr std::size_t max_unwrap_vertices = 20000;
constexpr std::size_t max_unwrap_islands  = 5000;
// What the chart and vertex numbers of `unwrap` are tied to: its charts, vertices and triangles and the layer's vertex edits.
// Changes when the paint, the mesh or the seams do; island placements and joins leave it.
std::string unwrap_id(const PatchUnwrap&, const TextureDisplacementLayer&);
// {chart_count, vertex_count, triangle_count, seam_angle_deg, auto_connect_islands, seams [[v, v]],
// islands [{chart, area_mm2, centroid, bounding_box {min, max}, offset, rotation_deg, scale, group, neighbours}],
// uv_edits [{vertex, uv}] (and mesh_vertex_uv_edits [{mesh_vertex, uv}] for edits stored by mesh vertex), vertices?
// [{vertex, source_vertex, chart, uv}] (the first max_unwrap_vertices; truncated), unwrap_id, islands_truncated (the first
// max_unwrap_islands are listed), note}. The unwrap is the layer's
// with its uv edits applied; coordinates are the unwrap's mm. Everything in it is taken back by read_unwrap_request.
nlohmann::json unwrap_json(const PatchUnwrap&, const TextureDisplacementLayer&, bool include_vertices);

// The painted patch of a layer's mask, in the frame the panel and the bake unwrap it in
// (texture_displacement_bake_frame of the owner's volume-to-world). Empty when nothing is painted.
indexed_triangle_set unwrap_patch(const TriangleMesh& owner_mesh, const PaintData& mask, const Transform3d& volume_to_world);
// The unwrap the panel shows for `layer`: its seams and angle, no padding, then its uv edits.
PatchUnwrap layer_unwrap(const indexed_triangle_set& patch, const TextureDisplacementLayer& layer);

// One call's edit of a layer's unwrap, worked out on a copy of the layer.
struct UnwrapEdit
{
    TextureDisplacementLayer layer;       // the layer after the call (the layer as it was when nothing changed)
    PatchUnwrap              unwrap;      // the unwrap after the call, uv edits applied
    bool                     changed  = false;
    bool                     resolved = false; // the seams or the angle changed: unwrapped anew, charts renumbered
    std::string              snapshot;    // the undo step's name, empty when nothing changed
    std::vector<std::string> notes;
    std::vector<std::pair<int, int>> snapped_paths; // per seam_paths entry, the mesh vertices its ends snapped to
};
// What the panel's UV pane does for each part of `request`, in its order: settings, seams (add, remove, clear, paths,
// cut), a new unwrap when seams or angle changed (then the connected net, when the layer lays islands out so), relayout,
// island placement, join, unjoin, average scale, clear uv edits, uv edits. Chart and vertex numbers name the unwrap as it
// is before the call, so a call that changes seams or the angle takes no island or uv edit. `mesh` is the owner's
// mesh in its own frame, `patch` its painted patch (unwrap_patch), `volume_to_plate` the owner's transform to plate mm.
// nullopt with `error` for a refusal (an unknown chart or vertex, a join of charts that share no edge, a seam that is no
// mesh edge, nothing painted) or when `cancelled` says so.
std::optional<UnwrapEdit> edit_unwrap(const TextureDisplacementLayer& before, const UnwrapRequest& request, const indexed_triangle_set& mesh,
                                      const indexed_triangle_set& patch, const Transform3d& volume_to_plate,
                                      const std::function<bool()>& cancelled, std::string& error);
// The fields of a layer an unwrap edit writes are equal.
bool same_unwrap_state(const TextureDisplacementLayer&, const TextureDisplacementLayer&);
// Writes those fields of `from` into `to`.
void copy_unwrap_state(const TextureDisplacementLayer& from, TextureDisplacementLayer& to);

// ---- Answers ----

// A layer's texture as the library knows it: the id, and the source (builtin, user, pack, or "project" for an
// image only the project holds).
struct LayerTexture
{
    std::optional<std::string> id;
    std::string                source = "project";
};
// {layer, position, base, image {texture (id or null), name, source, path}, color_capable, depth_mm,
// tile_size_mm, rotation_deg, offset, invert, midlevel, smoothing, edge_fade, edge_fade_amount, tile,
// tile_method, projection, blend, color, view_axes {right, up} in plate axes (view; set_texture_displacement's
// own shape), projection_frame_active (view with a frame; read_layer_request ignores it), unwrap {seam_angle_deg, auto_connect_islands, seams, placed_islands, uv_edits}
// (unwrap), paint [{volume_id, name, painted, stale, painted_facets, coverage_percent}], painted,
// inactive_settings, warnings}
nlohmann::json layer_json(const TextureDisplacementLayer& layer, int position, bool base, const LayerTexture& texture,
                          std::optional<bool> color_capable, const std::vector<MaskReport>& paint, bool owner_has_uv,
                          const std::vector<TextureDisplacementLayer>& stack, const TextureDisplacementOptions& options,
                          const Transform3d& volume_to_world);
// One layer of a stack as get_texture_displacement tells it: the layer and what the library and the image say of it.
struct StackLayerFacts
{
    TextureDisplacementLayer layer;
    LayerTexture             texture;
    std::optional<bool>      color_capable;
};
struct StackAnswer
{
    nlohmann::json     json;                       // {layers, free_layers, needs_bake, stack_options}
    std::optional<int> first_unpainted_layer;      // the lowest slot painted on no part (whatever `only_volume` shows)
};
// The whole stack, layers in slot order (the panel's), each as layer_json tells it with the paint of the model parts in
// `reports` (parallel to the parts, `volume_id` in each report): of `only_volume` alone when given. `needs_bake` and
// `first_unpainted_layer` always read every part.
StackAnswer texture_stack_json(std::vector<StackLayerFacts> layers, const std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>>& reports,
                               std::optional<int> only_volume, bool owner_has_uv, const TextureDisplacementOptions& options,
                               const Transform3d& volume_to_world);
// {pipeline (default | classic), displace_border, smooth {enabled, strength (percent), passes,
// ignore_outer_ring}, resolution_mm (null = auto), budget_k (null = auto), color_mix, color_mix_count,
// color_denoise}
nlohmann::json stack_options_json(const TextureDisplacementOptions&);

// ---- The bake and the mesh preparation ----

// The panel's ranges for the bake's and the preparation's numbers: one table for the readers and the schemas.
const std::vector<SettingBound>& bake_setting_bounds();
const SettingBound*              bake_setting_bound(const std::string& key);
const std::vector<SettingBound>& prepare_setting_bounds();
const SettingBound*              prepare_setting_bound(const std::string& key);

// The smoothing settings the bake and a smooth take (stack_options_json's `smooth`, accepted back whole).
struct SmoothRequest
{
    std::optional<bool>  enabled, ignore_outer_ring;
    std::optional<float> strength_percent; // 1..100, the panel's unit
    std::optional<int>   passes;           // 1..10
    bool empty() const { return !enabled && !ignore_outer_ring && !strength_percent && !passes; }
};
// bake_texture_displacement's options. The bake stores each one into the object's stack options, as the panel's
// controls store them. stack_options_json's output is accepted back: a null resolution_mm or budget_k is
// automatic (so are 0 and -1), and the stack-wide colour options are taken as they are.
struct BakeRequest
{
    std::optional<bool>  classic;                      // pipeline: classic (true) or default (false)
    std::optional<float> resolution_mm;                // 0.02..2
    bool                 resolution_auto = false;      // resolution_mm null or 0
    std::optional<int>   budget_k;                     // 0..4000, 0 = no simplification
    bool                 budget_auto = false;          // budget_k null or -1
    std::optional<int>   classic_budget_k;             // 10..2000, the classic bake's subdivision budget (not stored)
    std::optional<bool>  displace_border;
    SmoothRequest        smooth;
    std::optional<bool>  color_mix;
    std::optional<int>   color_mix_count, color_denoise;
    bool                 include_preview = false;
};
// Types, ranges, strict keys of `smooth`; `object_id` is not read. nullopt with `error` set otherwise.
std::optional<BakeRequest> read_bake_request(const nlohmann::json& params, std::string& error);
// The options the bake runs with: the stored ones with the request applied. What the request leaves out stays as it
// is, except displace_border (true) and smooth.enabled (false), which the Standard bake pins.
TextureDisplacementOptions bake_options(const TextureDisplacementOptions& stored, const BakeRequest&);
// Whether two option sets differ in anything the bake reads (the panel's active layer and mode are not).
bool bake_options_differ(const TextureDisplacementOptions&, const TextureDisplacementOptions&);
// The classic bake's subdivision budget, in thousands of triangles (the panel's default 750).
int classic_bake_budget_k(const BakeRequest&);

enum class PrepareOperation { remesh, subdivide, smooth };
// prepare_texture_mesh's arguments. Numbers left out take the panel's defaults, the two edge lengths from the mesh.
struct PrepareRequest
{
    PrepareOperation     operation = PrepareOperation::remesh;
    std::optional<float> target_edge_mm;                       // remesh, 0.1..20
    std::optional<bool>  keep_sharp_edges;                     // remesh, default true
    std::optional<float> sharp_angle_deg;                      // remesh, 5..90, default 40
    std::optional<bool>  adaptive;                             // subdivide, default true
    std::optional<int>   steps;                                // uniform subdivide, 1..5
    std::optional<float> max_edge_mm, detail_mm, min_edge_mm;  // adaptive subdivide
    std::optional<float> edge_detail_mm, color_detail_mm;      // adaptive subdivide, 0..5
    std::optional<bool>  follow_texture;                       // adaptive subdivide
    std::optional<int>   budget_k;                             // adaptive subdivide, 10..2000
    SmoothRequest        smooth;                               // smooth: the stored settings where left out
    bool                 uniform() const { return operation == PrepareOperation::subdivide && !adaptive.value_or(true); }
};
// operation is required; an argument of another operation (or an adaptive-only one of a uniform subdivide, steps of
// an adaptive one) is refused. smooth takes strength (percent), passes and ignore_outer_ring.
std::optional<PrepareRequest> read_prepare_request(const nlohmann::json& params, std::string& error);
// The libslic3r parameters of a remesh / an adaptive subdivide; `default_edge_mm` stands in for a left-out
// target_edge_mm / max_edge_mm (default_remesh_edge_mm, default_subdivide_edge_mm of the mesh).
TextureDisplacementPrepareParams remesh_prepare_params(const PrepareRequest&, float default_edge_mm);
TextureDisplacementPrepareParams subdivide_prepare_params(const PrepareRequest&, float default_edge_mm);
// The panel's seeds: the mesh's mean edge length (clamped to 0.1..20), and half of it (clamped to 0.001..20); 1 for no edge.
float default_remesh_edge_mm(const indexed_triangle_set&);
float default_subdivide_edge_mm(const indexed_triangle_set&);
// The stored options with a smooth's strength, passes and ignore_outer_ring applied.
TextureDisplacementOptions smooth_options(const TextureDisplacementOptions& stored, const SmoothRequest&);
// A uniform subdivision of `facets` triangles by `steps` is refused above this many triangles (each step quadruples).
// The limit is low because the paint of the four standard channels is remapped onto the new triangles on the app's main
// thread (the panel's Subdivide does the same), which takes seconds per million.
constexpr std::size_t UNIFORM_SUBDIVISION_MAX_FACETS = 1000000;
std::optional<std::string> uniform_subdivision_refusal(std::size_t facets, int steps);
// A smooth of a part of more than this many triangles is refused: it runs on the main thread (the vertices to relax come
// from the live volume), where the app cannot be held for long.
constexpr std::size_t SMOOTH_MAX_FACETS = 1000000;
std::optional<std::string> smooth_refusal(std::size_t facets);
// {setting: why} for the bake's arguments the pipeline does not use: classic_budget_k on the default pipeline,
// resolution_mm and budget_k on the classic one. Stored or reported, never applied.
nlohmann::json bake_inactive_settings(const BakeRequest&, bool classic);

// The words of the bake's and the preparation's answers, one place.
std::string bake_nothing_painted_message(int object_id);
std::string bake_no_displacement_message();
std::string bake_timed_out_message(double wait_s, bool classic);
std::string prepare_timed_out_message(double wait_s);
std::string bake_undo_warning(int slots_added);
std::string prepare_unchanged_message(const PrepareRequest&);
std::string prepare_paint_lost_message();

// One model part's share of a bake as the answer tells it.
struct BakePartReport
{
    int         volume_id     = -1; // the part's place among the object's volumes
    std::string name;
    std::size_t facets_before = 0, facets_after = 0;
    bool        baked = false;      // its mesh was rebuilt (false: the bake moved nothing, the part is as it was)
    bool        colored = false;    // the bake wrote colour paint on it
};
// {volume_id, name, facets_before, facets_after, baked, colored}
nlohmann::json bake_part_json(const BakePartReport&);
// {needed, kept} of the panel's budget notification (triangles), null when the budget did not cap the detail.
nlohmann::json bake_budget_json(std::size_t needed, std::size_t kept);
// The warning a budget-limited bake carries: kept / needed triangles, what that costs (detail thinned, bands where long input
// triangles were split) and what to do. Empty when the budget did not cap the detail (needed 0).
std::string bake_budget_warning(std::size_t needed, std::size_t kept);

// Whether a bake's resolution can carry the texture's detail. What aliases is the pattern's finest common feature on the part,
// not the pixel grid: `texture_feature_mm` measures the run lengths of the height map between crossings of its mean level, along
// rows and columns, and takes the length-weighted 10th percentile, in mm through the layer's mapping. The recommendation is
// half that feature, floored at 0.05 mm; the texture aliases when the resolution used is coarser than the recommendation.
struct ResolutionAdvice
{
    float recommended_mm = 0.f; // 0: no texture feature to size by
    float feature_mm     = 0.f;
    float used_mm        = 0.f;
    bool  aliasing       = false;
};
// The finest common feature of a height map (`pixels`, row-major, width x height) in mm, given the mm one pixel spans along a row
// (`mm_u`) and down a column (`mm_v`). 0 for an image with no crossing of its mean level (flat, or one edge only).
float texture_feature_mm(const std::vector<uint8_t>& pixels, int width, int height, float mm_u, float mm_v);
ResolutionAdvice resolution_advice_from(float feature_mm, float used_mm);
// One part as the advice reads it: its mesh, where it stands in world mm, and its UV channel (null: none; a ModelUV layer then
// maps by its tile size like the others).
struct AdvicePart
{
    const indexed_triangle_set* mesh = nullptr;
    Transform3d                 to_world = Transform3d::Identity();
    const std::vector<Vec2f>*   model_uv = nullptr;
};
// mm per unit of a UV channel: sqrt(surface area in world mm / UV area). 0 when the channel does not cover the mesh.
float model_uv_mm_per_unit(const indexed_triangle_set& mesh, const Transform3d& to_world, const std::vector<Vec2f>& uv);
// The advice for a stack on `parts`; `chosen_mm` is the stack's resolution_mm (0 or less: automatic, the coarsest part's).
ResolutionAdvice resolution_advice(const std::vector<TextureDisplacementLayer>& layers, const std::vector<AdvicePart>& parts, float chosen_mm);
// {recommended_resolution_mm}, null when there is no texture to size by.
nlohmann::json recommended_resolution_json(const ResolutionAdvice&);
// The warning that the texture will alias at the resolution used; empty when it does not.
std::string resolution_alias_warning(const ResolutionAdvice&);
// [{slot (1-based), created}] for the 0-based `slots` the bake named; created when the slot did not exist before
// (`filaments_before` slots then).
nlohmann::json bake_mixed_filaments_json(const std::vector<int>& slots, int filaments_before);


}}} // namespace Slic3r::GUI::OrcaMCP
