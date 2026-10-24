#ifndef slic3r_TextureDisplacementBakeJob_hpp_
#define slic3r_TextureDisplacementBakeJob_hpp_

#include <cstddef>
#include <exception>
#include <cstdint>
#include <functional>
#include "libslic3r/Point.hpp"
#include <vector>

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "Job.hpp"

namespace Slic3r {
class Model;
class ModelObject;
class ModelVolume;
}

namespace Slic3r::GUI {

class Plater;

// One model part's share of a bake: its own mesh, its own paint masks and UV channel, and the layer
// stack to displace it by. Captured by value on the main thread when the job is queued so the worker
// thread never touches the live Model concurrently with the UI (mirrors how EmbossJob's DataBase is
// captured before process() runs).
struct TextureDisplacementBakeVolume
{
    ObjectID                              volume_id;
    indexed_triangle_set                  base_mesh;
    std::vector<TextureDisplacementLayer> layers;
    TextureDisplacementFacetsData         facets_data;
    TextureDisplacementOptions            options;
    // Mesh coordinates -> world millimetres. Captured here with everything else so the worker never
    // reaches back into the live Model for it. See build_texture_displacement().
    Transform3d                           volume_to_world = Transform3d::Identity();
    // The part's own UV channel (ModelVolume::texture_uv), empty when it has none: what a ModelUV
    // layer maps through, and what the commit puts back on the baked mesh.
    std::vector<Vec2f>                    model_uv;
    // Where the part sits in its object: the world point the object's texture is anchored at, and how
    // far the object's lowest point lies below the part's own. See build_texture_displacement().
    Vec3d                                 texture_anchor = Vec3d::Zero();
    float                                 resting_drop   = 0.f;
};

struct TextureDisplacementBakeInput
{
    // Every model part the bake acts on, in the object's own volume order. An object made of several
    // parts carries one entry per part the paint actually reaches: the layer stack is the object's,
    // but each part is displaced through its own mesh, its own masks and its own UV channel. Baking
    // only the first part is what made texture displacement silently ignore every other body of a
    // multibody object.
    std::vector<TextureDisplacementBakeVolume> volumes;
    // Captured on the main thread, and shared by every part: the palette comes from the project's
    // filaments and from the layer stack, neither of which is per part. Empty unless some layer is
    // colouring, in which case the bake also writes each part's mmu_segmentation_facets - the same
    // per-triangle filament assignment the MMU paint gizmo writes - alongside the displaced geometry.
    TextureColorSettings                       color;
    // Whether this job pushes its own undo step when it commits. False when the caller has already
    // taken one that is meant to cover the displacement as well - Standard mode's Bake, which remeshes
    // and subdivides first and has to undo as a single action.
    bool                                       take_snapshot = true;
};

// What the bake produced for one part of TextureDisplacementBakeInput::volumes, at the same index.
struct TextureBakeVolumeResult
{
    TriangleMesh         mesh;
    // Per triangle of `mesh`: the palette entry to print it in, as its index + 1 (0 = leave alone).
    // Empty unless a layer asked for colour. See TextureColorRequest; the apply step turns these into
    // filaments.
    std::vector<uint8_t> triangle_color;
    // What the bake spent, for the message it leaves behind when the budget capped the detail.
    TextureBakeStats     stats;
};

struct TextureBakePlan
{
    std::vector<TextureBakeVolumeResult> results;
    bool                                 canceled = false;
};

// The bake in three steps, which the panel's Bake job and OrcaMCP's bake_texture_displacement both run
// (as FixModelByCgal.hpp does for the repair): capture on the main thread, plan on any thread, apply on
// the main thread.
//
// Main thread: the job's input for `object`, as the panel's Bake builds it - the painted parts, the
// object's layers for each, the owner's options, the texture anchor and each part's resting drop. Empty
// `volumes` when the object has no painted part.
TextureDisplacementBakeInput capture_texture_bake(const ModelObject &object, const TextureColorSettings &color,
                                                  bool take_snapshot);
// Any thread: the displacement of every part of `input`, one after another. `progress` is told the
// overall percentage and returns false to stop (the plan is then `canceled` and nothing may be applied).
TextureBakePlan plan_texture_bake(const TextureDisplacementBakeInput &input, const std::function<bool(int)> &progress);
// Main thread: the input still describes the model - every part found by id with the same mesh, masks,
// layers, options and placement.
bool texture_bake_applies_to(const Model &model, const TextureDisplacementBakeInput &input);
// Two layer stacks a bake would treat alike: the same layers as the undo stack stores them, and the same live image
// adjustments (which it leaves out).
bool same_texture_layers(const std::vector<TextureDisplacementLayer> &a, const std::vector<TextureDisplacementLayer> &b);
// Two captures of one part that a bake would treat alike: id, drop, anchor, placement, UV channel, mesh, paint, layers and
// options. `ignore_preview_budget` leaves out the v2 triangle budget, which the panel's preview (and OrcaMCP's texture
// preview) draws without.
bool same_bake_volume(const TextureDisplacementBakeVolume &a, const TextureDisplacementBakeVolume &b, bool ignore_preview_budget);
// A part's result is worth committing: it is not empty and some vertex moved more than 1e-5 mm. A bake
// that moved nothing must not be committed, since committing clears the baked layers' paint.
bool texture_bake_moved(const TextureDisplacementBakeInput &input, const TextureBakePlan &plan, std::size_t part);

struct TextureBakeOutcome
{
    std::vector<ObjectID> committed, unmoved;  // parts baked (their new ids) / parts the bake did not move, left as they were
    std::vector<int>      mixed_filaments;     // 0-based slots the colour bake named, each created or reused
    int                   filaments_before = 0, filaments_after = 0; // to tell created from reused
    std::size_t           refined = 0, budget = 0; // the budget notice's numbers, 0 when it did not cap
    ModelObject          *object = nullptr;
};
// Main thread: commits the plan's parts under the input's undo step (when `take_snapshot`). Shows no
// dialog and pushes no notification: the caller does.
TextureBakeOutcome apply_texture_bake(Plater &plater, TextureDisplacementBakeInput &input, TextureBakePlan &plan);

// Bakes an object's painted texture-displacement layers into real mesh geometry in the background,
// then commits the result on the main thread - mirrors EmbossJob's UpdateJob/update_volume()
// bake-and-commit pattern (see EmbossJob.cpp). Every part is displaced in the one job and committed
// under the one undo step, so a multibody object undoes as a single action.
class TextureDisplacementBakeJob : public Job
{
public:
    TextureDisplacementBakeJob(TextureDisplacementBakeInput &&input, std::function<void()> on_finished);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    TextureDisplacementBakeInput m_input;
    TextureBakePlan              m_plan;
    std::function<void()>        m_on_finished;
};

// Queues a single TextureDisplacementBakeJob for `input` on the app's UI job worker. `on_finished` is
// always called once the job settles (success, failure, or cancellation), so the caller can clear its own
// "bake in progress" UI state. Must be called from the main thread.
void queue_texture_displacement_bake(TextureDisplacementBakeInput &&input, std::function<void()> on_finished);

// `volume`'s share of a bake, read off the live Model. `layers`, `options`, `texture_anchor` and
// `resting_drop` are passed in rather than taken from `volume` because they belong to the object, not
// to the part: every part of a multibody object is displaced by the same stack, in the same texture
// frame and against the same plate (see GLGizmoTextureDisplacement::layers_for_part()).
TextureDisplacementBakeVolume texture_displacement_bake_volume(const ModelVolume                           &volume,
                                                               const std::vector<TextureDisplacementLayer> &layers,
                                                               const TextureDisplacementOptions            &options,
                                                               const Vec3d                                 &texture_anchor,
                                                               float                                        resting_drop);

} // namespace Slic3r::GUI

#endif // slic3r_TextureDisplacementBakeJob_hpp_
