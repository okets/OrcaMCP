// src/slic3r/GUI/OrcaMCP/OrcaMCPTexturePreview.hpp
//
// render_plate_view's texture_preview, apart from the app: the texture panel's true preview (the bake without its
// simplification, after the classic bake's preparation for a classic part) from a captured bake input, drawn as colour
// runs (texture colours, then the part's own colour paint, then its colour), what makes a computed preview reusable
// (a content key), the cache that keeps them, the stamp that tells the scene is still what was captured, and the
// report the answer carries. Nothing here reads the Plater or wx; the capture, the draw and the call's flow are
// OrcaMCPPlateUtils.cpp's (unit-tested in tests/slic3rutils/test_texture_preview.cpp).
#ifndef slic3r_GUI_OrcaMCPTexturePreview_hpp_
#define slic3r_GUI_OrcaMCPTexturePreview_hpp_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementBakeJob.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementPrepareJob.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A part with more triangles than this, before or after the preview, is not previewed (too_large, with its count): the
// GL model of a flat-shaded mesh holds three vertices and normals per triangle (about 72 bytes), so 1 M triangles is
// already ~72 MB on the main thread's upload. The triangles drawn in one call are held to PreviewCache::max_triangles.
constexpr std::size_t max_part_triangles = 1000000;

// The triangle budget a render's plan is held to, in thousands: the part cap.
constexpr int max_part_triangles_k = int(max_part_triangles / 1000);

// The bake's input as the preview plans it. The texture panel's preview plans with no simplification
// (v2_max_triangles_k = 0), which a part of millions of triangles would make too large to draw after all the work;
// the preview plans with the render's cap as the triangle budget instead, as the bake's budget simplifies: a part under
// the cap is as the unbudgeted one (flat faces harvested), a part over it is simplified to it. `keep_budget` (the object
// form of texture_preview) keeps the input's own budget, held to the cap (0 and a larger one become the cap). A classic
// part is untouched.
TextureDisplacementBakeInput preview_input(TextureDisplacementBakeInput bake_input, bool keep_budget = false);

// The resolution_mm at which a refinement of `refined` triangles at `current_mm` would fit `cap` triangles (the count goes
// with the square of the edge), rounded up to 0.01 mm; 0 when no resolution in the bake's range (0.02 to 2 mm) does.
double suggested_resolution_mm(double current_mm, std::size_t refined, std::size_t cap);

// Two captures a preview would draw alike (the budget left out: the preview ignores it).
bool same_preview_input(const TextureDisplacementBakeInput& a, const TextureDisplacementBakeInput& b);

// What the preview is coloured with besides the texture: per captured part its colour paint (mmu_segmentation_facets,
// empty when none) and the project's extruder colours the painted states name.
struct PreviewColours
{
    std::vector<TriangleSelector::TriangleSplittingData> colour_paint;
    std::vector<ColorRGBA>                               extruders;
};

// A content key of everything a preview is drawn from, computed in one pass over the capture (no Model needed): equal
// keys draw the same picture. `classic_budget_k` is the classic preparation's budget (0 for the default pipeline).
struct PreviewKey
{
    std::uint64_t a = 0, b = 0;
    bool operator==(const PreviewKey& o) const { return a == o.a && b == o.b; }
    bool operator!=(const PreviewKey& o) const { return !(*this == o); }
};
// `keep_budget`: the capture holds a budget the preview keeps (preview_input).
PreviewKey preview_key(const TextureDisplacementBakeInput& input, const PreviewColours& colours, int classic_budget_k, bool keep_budget = false);

// One run of triangles of a previewed part drawn in one colour (`base`: the part's own colour, which the draw knows).
struct PreviewRun
{
    std::pair<std::size_t, std::size_t> range;   // into the part's index buffer, in elements
    ColorRGBA                           color;
    bool                                base = false;
};

// One previewed part, ready to upload: its triangles sorted into colour runs.
struct PreviewPart
{
    int                  volume_id = -1;          // position among the object's volumes (get_object_info's volume_id)
    std::size_t          triangles_base = 0;      // the part's own triangles
    std::size_t          triangles = 0;           // the preview's (0: the layers moved nothing there)
    std::string          skipped;                 // "too_large": not drawn
    std::size_t          triangles_unsimplified = 0;   // the refinement's count before the budget simplified it (0: not known, a classic part)
    bool                 simplified_to_cap = false;    // the render's cap, not the stack's budget, is what simplified it
    double               suggested_resolution_mm = 0.; // the resolution_mm at which it would fit unsimplified (0: none does)
    indexed_triangle_set mesh;                    // empty when not drawn
    std::vector<PreviewRun> runs;
};

struct PreviewDraw
{
    std::vector<PreviewPart> parts;
    std::string              preparation;         // classic: "applied", "unchanged" or "paint_lost"; "" for the default pipeline
    std::size_t              triangles() const;   // those held (drawn parts)
    std::size_t              bytes() const;
};

// The preview of a capture. `prepare` (classic parts only) is the preparation the classic bake runs first, planned and
// applied to a copy of the input so the preview shows the surface the bake would make; a preparation that loses the
// paint is not applied (the bake refuses it) and reported as such. `volume_ids` are the parts' positions. Any thread;
// `progress` returns false to stop (the result is then `canceled`).
struct PreviewResult
{
    bool        canceled = false;
    PreviewDraw draw;
};
PreviewResult compute_preview(const TextureDisplacementBakeInput& captured, const TextureDisplacementPrepareInput* prepare,
                              const std::vector<int>& volume_ids, const PreviewColours& colours,
                              const std::function<bool(int)>& progress, bool keep_budget = false);

// The draw of a finished plan (what compute_preview ends with): colour runs by texture colour, else the part's colour
// paint carried onto the preview's triangles, else the part's colour. Any thread.
PreviewDraw make_preview_draw(TextureBakePlan&& plan, const TextureDisplacementBakeInput& original, const std::vector<int>& volume_ids,
                              const PreviewColours& colours, bool keep_budget = false);

// What a captured scene was, cheap to compare with the live one on the main thread (no mesh copy, no image compare
// beyond the pointer): per part its id, mesh, the eight masks' timestamps, layers, options and placement.
struct PreviewStamp
{
    struct Part
    {
        ObjectID                                     id;
        std::shared_ptr<const TriangleMesh>          mesh;
        std::array<ObjectBase::Timestamp, TEXTURE_DISPLACEMENT_MAX_LAYERS> masks{};
        Transform3d                                  to_world = Transform3d::Identity();
        std::vector<TextureDisplacementLayer>        layers;
        float                                        drop = 0.f;
        std::size_t                                  uv = 0;
    };
    std::vector<Part>          parts;
    Vec3d                      anchor = Vec3d::Zero();
    TextureDisplacementOptions options;
};
PreviewStamp capture_preview_stamp(const ModelObject& object);
// True when the object still holds exactly what the stamp saw.
bool preview_stamp_current(const ModelObject& object, const PreviewStamp& stamp);

// The previews kept between calls, by content key: one entry per object (a new preview of an object replaces its old
// one, so an edit to its layers, paint or mesh drops the entry), at most max_objects objects and max_triangles
// triangles in all; the least recently used goes first. A preview alone over the triangle cap is not kept. Entries
// hold the drawable result only, never the captured meshes or images. Thread safe: lookups run on the HTTP thread.
class PreviewCache
{
public:
    static constexpr std::size_t max_objects   = 4;
    static constexpr std::size_t max_triangles = 2000000;

    struct Entry
    {
        ObjectID                          object;
        PreviewKey                        key;
        std::shared_ptr<const PreviewDraw> draw;
        std::size_t                       triangles = 0;
        double                            seconds   = 0.;   // what computing it took
        std::uint64_t                     used      = 0;
    };

    std::shared_ptr<const Entry> find(const ObjectID& object, const PreviewKey& key);
    // False when the draw is over the triangle cap on its own (nothing is kept, the object's old entry goes).
    bool put(const ObjectID& object, const PreviewKey& key, std::shared_ptr<const PreviewDraw> draw, double seconds);
    void prune(const std::function<bool(const ObjectID&)>& exists);
    void clear();

    std::size_t size() const;
    std::size_t triangles() const;
    std::size_t bytes() const;

private:
    void evict_to_caps();   // the lock held

    mutable std::mutex                  m_mutex;
    std::vector<std::shared_ptr<Entry>> m_entries;
    std::uint64_t                       m_clock = 0;
};

// The app's one cache. Dropped when the project is reset or its objects are deleted (Plater::priv::reset,
// delete_all_objects_from_model), so a closed project's previews are not kept.
PreviewCache& texture_preview_cache();
void          drop_texture_previews();

// One object of a render's texture preview.
struct PreviewObject
{
    int                                    object_id = -1;      // index into Model::objects when captured
    ObjectID                               identity;
    TextureDisplacementBakeInput           captured;            // freed once planned
    TextureDisplacementPrepareInput        prepare;             // classic parts: the preparation the bake runs first; freed once planned
    int                                    classic_budget_k = 0;
    bool                                   keep_budget = false; // texture_preview's object form: the capture's own budget is kept
    PreviewColours                         colours;
    std::vector<int>                       volume_ids;
    PreviewStamp                           stamp;
    PreviewKey                             key;
    std::shared_ptr<const PreviewDraw>     draw;                // null: not computed
    bool                                   cached     = false;
    bool                                   attempted  = false;  // the work started it (a draw that is still null ran out of the wait)
    std::string                            undrawn;             // computed but not drawn: "over_render_total" (the call's triangle cap), "draw_deadline"
    bool                                   panel_open = false;  // the texture panel is open on it: its strokes since the last flush are not in the model
    double                                 seconds    = 0.;
};

// An object the preview left out and why (only objects that hold texture layers; the others are listed by id).
struct PreviewSkip
{
    int            object_id = -1;
    std::string    reason;
    nlohmann::json next_steps = nlohmann::json::array();   // next_step_json entries
};

struct PreviewReportInput
{
    int                        plate_index = 0;
    double                     wait_s      = 0.;            // the call's wait, for the timed-out message
    nlohmann::json             requested   = true;          // what texture_preview asked for, in the shape a request takes it
    bool                       narrowed    = false;         // the views fit objects, so only those were previewed
    std::vector<int>           outside_fit;                 // textured objects the views' fit left out (drawn unbaked)
    std::vector<PreviewObject> objects;
    std::vector<PreviewSkip>   skipped;
    std::vector<int>           without_layers;              // objects on the plate with no texture layer
};

// The answer's texture_preview: {requested, previewed, complete, objects: [{object_id, parts: [{volume_id,
// triangles_base, triangles_preview, skipped?}], cached, seconds, preparation?, panel_open?, timed_out?, not_reached?,
// note?}], skipped, no_texture_layers, outside_fit?, cache: {objects, triangles, bytes, max_objects, max_triangles}, note?}.
// `complete` is false when an object timed out or was never reached (it is drawn unbaked).
nlohmann::json preview_report_json(const PreviewReportInput& input, const PreviewCache& cache);

// The note an object whose preview did not finish within the call's wait is listed with; `narrowed` says the views
// already fit objects, so fitting would not help.
std::string preview_timed_out_message(int object_id, double wait_s, bool narrowed);
std::string preview_not_reached_message(int object_id, bool narrowed);
// The note a drawn part carries when the cap shaped its preview (empty when it did not): simplified_to_cap, or too_large.
std::string preview_part_note(const PreviewPart& part);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
