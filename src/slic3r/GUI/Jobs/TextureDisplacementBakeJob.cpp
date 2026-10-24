#include "TextureDisplacementBakeJob.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <string>
#include <vector>
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/Point.hpp"
#include <exception>
#include <cstddef>
#include <cstdint>
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/TextureDisplacementObject.hpp"
#include <memory>
#include <sstream>

#include <cereal/archives/binary.hpp>
#include <cereal/types/utility.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <array>

namespace Slic3r::GUI {

TextureDisplacementBakeJob::TextureDisplacementBakeJob(TextureDisplacementBakeInput &&input, std::function<void()> on_finished)
    : m_input(std::move(input)), m_on_finished(std::move(on_finished))
{
}

void TextureDisplacementBakeJob::process(Ctl &ctl)
{
    const std::string status = _u8L("Baking texture displacement");
    ctl.update_status(1, status);

    // Only ever touches m_input (captured by value before this job was queued) and local state -
    // never the live Model - so this is safe to run concurrently with the UI thread.
    //
    // The progress hook matters for more than cosmetics: the framework's progress notification only
    // grows a close button once it reaches 100%, so a job that reports 0 and nothing else leaves an
    // uncloseable notification pinned on screen. It also carries the Cancel button's effect into the
    // bake, which on a subdivided mesh can run for several seconds.
    int last_reported = 1;
    m_plan = plan_texture_bake(m_input, [&ctl, &status, &last_reported](int overall) {
        if (ctl.was_canceled())
            return false;
        // The notification repaints (and wakes the idle loop) on every call, so only push a
        // message when the displayed integer percentage actually moves.
        if (overall > last_reported) {
            last_reported = overall;
            ctl.update_status(overall, status);
        }
        return true;
    });

    // Always finish at 100: this is what closes the notification. Reported even on cancel, where
    // build_texture_displacement() returns an empty mesh and finalize() commits nothing.
    ctl.update_status(100, status);
}

TextureBakePlan plan_texture_bake(const TextureDisplacementBakeInput &input, const std::function<bool(int)> &progress)
{
    TextureBakePlan plan;

    // Colour, when any layer asks for it, is computed in the same pass as the displacement: both need
    // the same per-layer projection and UV work, and doing it twice would double the expensive part.
    //
    // The quantizers are built once for the whole job rather than per part: each fills a colour lookup
    // cube that costs tens of milliseconds, and the palette is the object's, not the part's.
    TextureColorRequest  color_request;
    TextureColorRequest *color = nullptr;
    if (!input.color.empty()) {
        color_request.quantize = GLGizmoTextureDisplacement::make_palette_quantizer(input.color.palette);
        if (!input.color.palette_pure.empty())
            color_request.quantize_pure = GLGizmoTextureDisplacement::make_palette_quantizer(input.color.palette_pure);
        color_request.despeckle_passes = input.color.despeckle_passes;
        if (color_request.quantize)
            color = &color_request;
    }

    // Each part gets its own slice of the 1..100 range, in order, so one progress bar covers the whole
    // bake however many parts it displaces.
    const size_t count = input.volumes.size();
    plan.results.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const TextureDisplacementBakeVolume &volume = input.volumes[i];
        if (color != nullptr)
            color_request.out_triangle = &plan.results[i].triangle_color;
        plan.results[i].mesh = TriangleMesh(build_texture_displacement(
            volume.base_mesh, volume.layers, volume.facets_data, volume.options,
            [&progress, &plan, i, count](int percent) {
                if (plan.canceled || !progress(int((i * 100 + size_t(std::max(0, percent))) / count))) {
                    plan.canceled = true;
                    return false;
                }
                return true;
            },
            color, volume.volume_to_world, nullptr, &plan.results[i].stats,
            volume.model_uv.empty() ? nullptr : &volume.model_uv, &volume.texture_anchor, volume.resting_drop));
        if (plan.canceled)
            break;
    }
    return plan;
}

bool texture_bake_moved(const TextureDisplacementBakeInput &input, const TextureBakePlan &plan, size_t part)
{
    const TriangleMesh &result = plan.results[part].mesh;
    if (result.empty())
        return false;
    const indexed_triangle_set &out  = result.its;
    const indexed_triangle_set &base = input.volumes[part].base_mesh;
    bool unchanged = out.indices.size() == base.indices.size() && out.vertices.size() == base.vertices.size();
    for (size_t v = 0; unchanged && v < out.vertices.size(); ++v)
        unchanged = (out.vertices[v] - base.vertices[v]).cwiseAbs().maxCoeff() < 1e-5f;
    return !unchanged;
}

void TextureDisplacementBakeJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    struct OnExit
    {
        std::function<void()> fn;
        ~OnExit() { if (fn) fn(); }
    } on_exit{m_on_finished};

    if (canceled || eptr || m_plan.canceled)
        return;

    const TextureBakeOutcome outcome = apply_texture_bake(*wxGetApp().plater(), m_input, m_plan);

    // A bake that moved nothing - no layer could be sampled, or every sample was zero - must not be
    // committed: committing is what clears the baked layers' paint, so the user would see the painted
    // region simply vanish with no relief in its place and no idea why. Keep the paint and say so.
    if (outcome.committed.empty()) {
        show_error(nullptr, _u8L("The bake produced no displacement, so nothing was changed and the paint was kept. "
                                 "Check that the painted layer has a texture and a non-zero depth."));
        return;
    }

    // The refinement went finer than the budget could keep: the simplification had to take detail back
    // out to fit, so what was baked carries less of the texture than the resolution asked for. Said
    // here, with the numbers, because it is the only place that knows them - and not as an error
    // dialog: the result is a usable mesh, just not the one the settings described. Summed over the
    // parts that hit it, since the budget is spent per part and one message is enough.
    if (outcome.refined > 0) {
        // Thousands under a million, so a small budget is not reported as "0.1 M".
        const auto count = [](size_t n) {
            return n >= 1000000 ? Slic3r::format("%1$.1f M", double(n) / 1000000.) :
                                  Slic3r::format("%1% k", (n + 500) / 1000);
        };
        wxGetApp().notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
            Slic3r::format(_u8L("The triangle budget limited the detail: this resolution needs %1% triangles, "
                                "the budget kept %2%. Raise Budget or use a coarser Resolution for the full detail."),
                           count(outcome.refined), count(outcome.budget)));
    }
}

TextureBakeOutcome apply_texture_bake(Plater &plater_ref, TextureDisplacementBakeInput &input, TextureBakePlan &plan)
{
    Plater *plater = &plater_ref;
    TextureBakeOutcome outcome;
    // A stopped plan has parts that were never displaced; committing the others would apply half a bake.
    if (plan.canceled)
        return outcome;

    // Judged per part, because on a multibody object only some parts may carry a layer that displaces;
    // the ones that did move are committed and the rest are left exactly as they were.
    std::vector<size_t> to_commit;
    for (size_t i = 0; i < plan.results.size(); ++i)
        if (texture_bake_moved(input, plan, i))
            to_commit.push_back(i);
        else
            outcome.unmoved.push_back(input.volumes[i].volume_id);
    if (to_commit.empty())
        return outcome;

    outcome.filaments_before = int(wxGetApp().preset_bundle->filament_presets.size());

    const auto filaments_for_palette = [&input, &plan, plater, &to_commit, &outcome]() {
        // palette_filaments() only cares *which* entries appear, so the union of the parts' entries
        // stands in for their concatenated per-triangle colours.
        std::vector<char>    seen(input.color.palette.size() + 1, 0);
        std::vector<uint8_t> used;
        for (const size_t i : to_commit)
            for (const uint8_t entry : plan.results[i].triangle_color)
                if (entry > 0 && size_t(entry) < seen.size() && !seen[size_t(entry)]) {
                    seen[size_t(entry)] = 1;
                    used.push_back(entry);
                }
        Sidebar &sidebar = plater->sidebar();
        std::vector<int> filament_of = GLGizmoTextureDisplacement::palette_filaments(
            input.color.palette, used, [&sidebar](const PrintableColor &mix) {
                return sidebar.ensure_mixed_filament({ unsigned(mix.a + 1), unsigned(mix.b + 1) },
                                                     { mix.a_percent(), 100 - mix.a_percent() });
            });
        // The mixed filament slots the bake named: the used entries beyond the single filaments.
        for (const uint8_t entry : used)
            // A mix that got no slot prints in its dominant component, a physical number below the
            // physical filament count: not a mixed slot.
            if (size_t(entry) > input.color.palette_pure.size() && size_t(entry) <= filament_of.size() &&
                filament_of[size_t(entry) - 1] >= int(input.color.palette_pure.size()) &&
                std::find(outcome.mixed_filaments.begin(), outcome.mixed_filaments.end(), filament_of[size_t(entry) - 1]) ==
                    outcome.mixed_filaments.end())
                outcome.mixed_filaments.push_back(filament_of[size_t(entry) - 1]);
        return filament_of;
    };

    const auto commit = [&input, &plan, plater, &to_commit, &filaments_for_palette, &outcome]() {
        const std::vector<int> filament_of = filaments_for_palette();
        ModelObject           *object      = nullptr;

        for (const size_t i : to_commit) {
            ModelVolume *volume = get_model_volume(input.volumes[i].volume_id, plater->model().objects);
            // The lookup doubles as a staleness check: anything that replaces a volume's mesh also gives it
            // a new id, so a part replaced since this bake started - by an undo, another bake, or a
            // boolean - simply fails to find its volume and is skipped.
            if (volume == nullptr)
                continue;
            const TextureDisplacementBakeVolume &captured       = input.volumes[i];
            const std::vector<uint8_t>          &triangle_color = plan.results[i].triangle_color;

            // Whether the bake replaced the triangles or only moved the vertices it was given. The classic
            // path is topology-preserving by contract, so every per-triangle annotation on the volume still
            // means what it did; the default pipeline refines, cuts and simplifies, so none of them do.
            // Both are checked: the flag is what the bake promised, the count is what it delivered. Only
            // the triangles matter - the annotations are per facet and reference no vertex index - so a
            // vertex count that changed on its own (the bake compacts unreferenced vertices away) is not a
            // reason to remap anything.
            const bool topology_changed = captured.options.pipeline_v2 ||
                                          plan.results[i].mesh.its.indices.size() != captured.base_mesh.indices.size();

            // Carry the volume's existing paint - the model's own colours above all - across that rebuild.
            // Without it a bake left the mmu_segmentation_facets bitstream pointing at a triangle set that
            // no longer existed, which is what scrambled the colours of an imported coloured model even
            // with colour switched off on every layer and nothing in the bake that was meant to touch them.
            //
            // transfer_facet_paint() rather than ModelVolume::restore_painting(): the latter's remap wants
            // a target triangle overlapping the source paint to within 0.01 mm, which the baked surface is
            // nowhere near - it has moved by the relief depth. Nothing to do when the topology is intact:
            // triangle i is still triangle i, so the paint is already exactly right and carrying it at all
            // could only lose detail.
            std::array<TriangleSelector::TriangleSplittingData, 4> before;
            if (topology_changed) {
                before[0] = volume->supported_facets.get_data();
                before[1] = volume->seam_facets.get_data();
                before[2] = volume->mmu_segmentation_facets.get_data();
                before[3] = volume->fuzzy_skin_facets.get_data();
            }

            volume->set_mesh(std::move(plan.results[i].mesh));
            volume->set_new_unique_id();
            volume->calculate_convex_hull();
            outcome.committed.push_back(volume->id());

            if (topology_changed) {
                const std::array<FacetsAnnotation *, 4> target{ &volume->supported_facets, &volume->seam_facets,
                                                                &volume->mmu_segmentation_facets,
                                                                &volume->fuzzy_skin_facets };
                for (size_t ch = 0; ch < target.size(); ++ch) {
                    const std::vector<uint8_t> state =
                        transfer_facet_paint(captured.base_mesh, before[ch], volume->mesh().its);
                    // Nothing was painted in this channel, or there is nothing to paint it onto: either
                    // way the annotation must not keep describing the mesh that is gone.
                    if (state.empty()) {
                        target[ch]->reset();
                        continue;
                    }
                    TriangleSelector selector(volume->mesh());
                    bool             any = false;
                    for (size_t t = 0; t < state.size(); ++t)
                        if (state[t] != 0) {
                            selector.set_facet(int(t), EnforcerBlockerType(state[t]));
                            any = true;
                        }
                    if (any)
                        target[ch]->set(selector);
                    else
                        target[ch]->reset();
                }
            }

            // Colour lands in mmu_segmentation_facets, merged *over* whatever is already painted there
            // rather than replacing it: a triangle the texture does not colour keeps its existing filament,
            // and one the user never painted at all stays at NONE, which already means "the volume's own
            // filament". That is what confines the effect to the painted area without having to invent a
            // colour for everything outside it. What was already painted is whatever the transfer above
            // left in the annotation, so this bake's colour lands on top of it rather than instead of it.
            if (!triangle_color.empty() && triangle_color.size() == volume->mesh().its.indices.size()) {
                TriangleSelector selector(volume->mesh());
                const TriangleSelector::TriangleSplittingData &existing = volume->mmu_segmentation_facets.get_data();
                if (!existing.bitstream.empty())
                    selector.deserialize(existing, false);
                for (size_t t = 0; t < triangle_color.size(); ++t) {
                    const size_t entry = size_t(triangle_color[t]);
                    if (entry > 0 && entry <= filament_of.size() && filament_of[entry - 1] >= 0)
                        selector.set_facet(int(t), EnforcerBlockerType(filament_of[entry - 1] + 1));
                }
                volume->mmu_segmentation_facets.set(selector);
            }

            // Clear the paint mask of every layer that was actually baked so a repeat bake (or the paint
            // overlay) doesn't act on triangles that no longer represent the same unbaked surface. The
            // texture layer definitions themselves are left untouched so the user can keep sculpting with
            // the same textures.
            //
            // A mask the bake did *not* consume only still means what it did if the topology is unchanged,
            // which is true of the classic path (it moves existing vertices) but not of the one-run
            // pipeline, which rebuilds and then simplifies the mesh. A mask left behind against the old
            // topology is exactly what makes the gizmo reload an empty selector over a non-empty mask and
            // then erase it on the next flush - see GLGizmoTextureDisplacement::update_from_model_object().
            for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
                const auto it = std::find_if(captured.layers.begin(), captured.layers.end(),
                                             [slot](const TextureDisplacementLayer &l) { return l.slot == slot; });
                if (topology_changed || (it != captured.layers.end() && !it->empty()))
                    volume->texture_displacement_facet(slot).reset();
            }

            // The model's own UV channel. Resampled onto a rebuilt mesh from the surface the bake started
            // from: a displaced vertex came from its foot point on that surface, which is the position its
            // uv belongs to - so no distance limit, the relief depth is exactly how far it has moved.
            if (!captured.model_uv.empty())
                volume->texture_uv = topology_changed ? transfer_uv_channel(captured.base_mesh, captured.model_uv,
                                                                            volume->mesh().its) :
                                                         captured.model_uv;

            if (object == nullptr)
                object = volume->get_object();
        }

        if (object == nullptr)
            return;
        outcome.object = object;

        if (ObjectList *obj_list = wxGetApp().obj_list()) {
            const ModelObjectPtrs &objs = plater->model().objects;
            auto it = std::find(objs.begin(), objs.end(), object);
            if (it != objs.end())
                obj_list->update_info_items(size_t(it - objs.begin()));
        }

        plater->changed_object(*object);
    };

    // Standard mode's Bake is remesh -> subdivide -> displace under a single snapshot, and this job runs
    // long after that snapshot's scope has closed. Adding one here would put an undo step *between* the
    // subdivision and the displacement: the first Undo would land on a mesh carrying every added
    // triangle and no relief at all, and pressing Bake again from there would subdivide that mesh a
    // second time. So the caller says who owns the undo step.
    if (input.take_snapshot) {
        Plater::TakeSnapshot snapshot(plater, _u8L("Bake texture displacement"), UndoRedo::SnapshotType::GizmoAction);
        commit();
    } else {
        commit();
    }
    outcome.filaments_after = int(wxGetApp().preset_bundle->filament_presets.size());

    // The budget is spent per part, so the parts that hit it are summed.
    for (const size_t i : to_commit)
        if (plan.results[i].stats.budget_limited) {
            outcome.refined += plan.results[i].stats.triangles_refined;
            outcome.budget  += plan.results[i].stats.triangles_budget;
        }
    return outcome;
}

TextureDisplacementBakeVolume texture_displacement_bake_volume(const ModelVolume                           &volume,
                                                               const std::vector<TextureDisplacementLayer> &layers,
                                                               const TextureDisplacementOptions            &options,
                                                               const Vec3d                                 &texture_anchor,
                                                               float                                        resting_drop)
{
    TextureDisplacementBakeVolume out;
    out.volume_id       = volume.id();
    out.base_mesh       = volume.mesh().its;
    out.layers          = layers;
    out.options         = options;
    out.volume_to_world = texture_displacement_volume_to_world(volume);
    out.texture_anchor  = texture_anchor;
    out.resting_drop    = resting_drop;
    if (volume.has_texture_uv())
        out.model_uv = volume.texture_uv;
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        out.facets_data[size_t(i)] = volume.texture_displacement_facet(i).get_data();
    return out;
}

TextureDisplacementBakeInput capture_texture_bake(const ModelObject &object, const TextureColorSettings &color,
                                                  bool take_snapshot)
{
    TextureDisplacementBakeInput input;
    input.color         = color;
    input.take_snapshot = take_snapshot;

    const ModelVolume *owner = texture_stack_owner(object);
    if (owner == nullptr)
        return input;
    // Every painted part, not only the one holding the layer stack: the brush paints across all of
    // them, so baking one would leave the user's paint on the others untouched and their geometry flat.
    const std::vector<ModelVolume *> parts = painted_texture_parts(const_cast<ModelObject &>(object));
    const Vec3d                      anchor = texture_anchor(object);
    const std::vector<float>         drops  = resting_drops(object, parts);
    input.volumes.reserve(parts.size());
    for (size_t i = 0; i < parts.size(); ++i)
        input.volumes.push_back(texture_displacement_bake_volume(*parts[i], layers_for_part(object, *parts[i]),
                                                                 owner->texture_displacement_options, anchor, drops[i]));
    return input;
}

namespace {
// Two values the undo stack would store alike: compared through the cereal archive that stack writes.
template<class T> std::string serialized(const T &value)
{
    std::ostringstream os(std::ios::binary);
    {
        cereal::BinaryOutputArchive archive(os);
        archive(value);
    }
    return os.str();
}

// Options as the bake reads them: where the panel was left (active layer, mode) is not one of them.
bool same_bake_options(TextureDisplacementOptions a, TextureDisplacementOptions b)
{
    a.active_slot = b.active_slot = 0;
    a.panel_mode  = b.panel_mode  = 0;
    return serialized(a) == serialized(b);
}
} // namespace

// The bake reads a layer's live image adjustments, which the archive leaves out.
bool same_texture_layers(const std::vector<TextureDisplacementLayer> &a, const std::vector<TextureDisplacementLayer> &b)
{
    if (a.size() != b.size() || serialized(a) != serialized(b))
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!(a[i].preview_adjust == b[i].preview_adjust))
            return false;
    return true;
}

bool same_bake_volume(const TextureDisplacementBakeVolume &live, const TextureDisplacementBakeVolume &captured, bool ignore_preview_budget)
{
    TextureDisplacementOptions live_options = live.options, captured_options = captured.options;
    if (ignore_preview_budget) // the panel's preview draws the bake without its triangle budget, so the budget is not what it shows
        live_options.v2_max_triangles_k = captured_options.v2_max_triangles_k = 0;
    return live.volume_id == captured.volume_id && live.resting_drop == captured.resting_drop &&
           live.texture_anchor == captured.texture_anchor &&
           live.volume_to_world.matrix() == captured.volume_to_world.matrix() && live.model_uv == captured.model_uv &&
           live.base_mesh.indices == captured.base_mesh.indices && live.base_mesh.vertices == captured.base_mesh.vertices &&
           live.facets_data == captured.facets_data && same_texture_layers(live.layers, captured.layers) &&
           same_bake_options(live_options, captured_options);
}

bool texture_bake_applies_to(const Model &model, const TextureDisplacementBakeInput &input)
{
    if (input.volumes.empty())
        return true;
    const ModelVolume *first = get_model_volume(input.volumes.front().volume_id, model.objects);
    if (first == nullptr || first->get_object() == nullptr)
        return false;

    // What a bake started now would capture: the same parts (one painted since, or turned into a modifier,
    // changes the list), the same drops (they depend on the unpainted parts too) and everything else.
    const TextureDisplacementBakeInput now = capture_texture_bake(*first->get_object(), input.color, input.take_snapshot);
    if (now.volumes.size() != input.volumes.size())
        return false;
    for (size_t i = 0; i < now.volumes.size(); ++i)
        if (!same_bake_volume(now.volumes[i], input.volumes[i], false))
            return false;
    return true;
}

void queue_texture_displacement_bake(TextureDisplacementBakeInput &&input, std::function<void()> on_finished)
{
    auto &worker = wxGetApp().plater()->get_ui_job_worker();
    queue_job(worker, std::make_unique<TextureDisplacementBakeJob>(std::move(input), std::move(on_finished)));
}

} // namespace Slic3r::GUI
