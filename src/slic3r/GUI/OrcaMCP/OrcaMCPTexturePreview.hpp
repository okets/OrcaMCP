// src/slic3r/GUI/OrcaMCP/OrcaMCPTexturePreview.hpp
//
// render_plate_view's texture_preview, apart from the app: the texture panel's true preview (the bake without its
// simplification) from a captured bake input, what makes a computed preview reusable, the small cache that keeps them,
// and the report the answer carries. Nothing here reads the Model, the Plater or wx; the capture, the draw and the
// scene check are OrcaMCPPlateUtils.cpp's (unit-tested in tests/slic3rutils/test_texture_preview.cpp).
#ifndef slic3r_GUI_OrcaMCPTexturePreview_hpp_
#define slic3r_GUI_OrcaMCPTexturePreview_hpp_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/ObjectID.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementBakeJob.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// The panel's preview from the bake's input: the one difference GLGizmoTextureDisplacement::queue_preview_job makes
// (v2 pipeline: no simplification, v2_max_triangles_k = 0). The classic pipeline previews the mesh it is given, as
// the panel does. Everything else is untouched.
TextureDisplacementBakeInput preview_input(TextureDisplacementBakeInput bake_input);

// Two captures a preview would draw alike: the same parts with the same mesh, paint (the eight masks), layers, options
// (the budget left out: the preview ignores it), UV channel, placement, anchor and drop, and the same palette.
bool same_preview_input(const TextureDisplacementBakeInput& a, const TextureDisplacementBakeInput& b);

// The triangles a plan holds.
std::size_t preview_triangles(const TextureBakePlan& plan);

// The previews kept between calls: one per object (a new capture of an object replaces its old entry, so an edit to
// its layers, paint or mesh drops the entry), at most max_objects objects and max_triangles preview triangles in all;
// the least recently used goes first. A preview alone over the triangle cap is drawn but not kept. Used on the main
// thread only.
class PreviewCache
{
public:
    static constexpr std::size_t max_objects   = 4;
    static constexpr std::size_t max_triangles = 2000000;

    struct Entry
    {
        ObjectID                             object;
        TextureDisplacementBakeInput         input;
        std::shared_ptr<const TextureBakePlan> plan;
        std::size_t                          triangles = 0;
        double                               seconds   = 0.;   // what computing it took
        std::uint64_t                        used      = 0;
    };

    // The entry for `object` when it was made from the same capture; marks it used.
    std::shared_ptr<const Entry> find(const ObjectID& object, const TextureDisplacementBakeInput& input);
    // Keeps the plan; false when it is over the triangle cap on its own (nothing is kept, the object's old entry goes).
    bool put(const ObjectID& object, TextureDisplacementBakeInput input, std::shared_ptr<const TextureBakePlan> plan, double seconds);
    // Drops the entries of objects `exists` says are gone.
    void prune(const std::function<bool(const ObjectID&)>& exists);
    void clear() { m_entries.clear(); }

    std::size_t size() const { return m_entries.size(); }
    std::size_t triangles() const;

private:
    void evict_to_caps();

    std::vector<std::shared_ptr<Entry>> m_entries;
    std::uint64_t                       m_clock = 0;
};

// One object of a render's texture preview: what the first step captured, the plan the work made of it (null while it
// is not computed: the call's wait ran out) and how it was got.
struct PreviewObject
{
    int                                    object_id = -1;      // index into Model::objects when captured
    ObjectID                               identity;
    TextureDisplacementBakeInput           captured;            // the bake's input; the preview's is preview_input(captured)
    std::vector<int>                       volume_ids;          // each captured part's position among the object's volumes (get_object_info's volume_id)
    std::shared_ptr<const TextureBakePlan> plan;
    bool                                   cached     = false;
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
    std::vector<PreviewObject> objects;
    std::vector<PreviewSkip>   skipped;
    std::vector<int>           without_layers;              // objects on the plate with no texture layer
};

// The answer's texture_preview: {requested: true, previewed, complete, objects: [{object_id, parts: [{volume_id,
// triangles_base, triangles_preview}], cached, seconds, panel_open?, timed_out?, note}], skipped: [{object_id, reason,
// next_steps?}], no_texture_layers: [ids], cache: {objects, triangles, max_objects, max_triangles}, note?}.
// `complete` is false when an object timed out (it is drawn unbaked); `note` says so when nothing was previewed.
nlohmann::json preview_report_json(const PreviewReportInput& input, std::size_t cache_objects, std::size_t cache_triangles);

// The reason an object whose preview did not finish within the call's wait is listed with.
std::string preview_timed_out_message(int object_id, double wait_s);

}}} // namespace Slic3r::GUI::OrcaMCP

#endif
