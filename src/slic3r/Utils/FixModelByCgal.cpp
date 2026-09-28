#include "FixModelByCgal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/log/trivial.hpp>

#include "libslic3r/Geometry.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Thread.hpp"
#include "../GUI/I18N.hpp"

// Orca: This file provides utilities for repairing 3D model meshes using the CGAL library, handling mesh splitting, merging, and boolean operations.

namespace Slic3r {

namespace {

// Orca: Helper functions for analyzing mesh properties and transformations.

bool is_not_3dimensional_part(const TriangleMesh &mesh)
{
    // Orca: Determines if a mesh is degenerate or represents a non-3dimensional part by checking volume and bounding box dimensions.
    if (mesh.its.indices.empty())
        return true;

    indexed_triangle_set tmp = mesh.its;
    its_remove_degenerate_faces(tmp, true);
    if (tmp.indices.empty())
        return true;

    const BoundingBoxf3 bbox = mesh.bounding_box();
    const Vec3d size = bbox.size();
    const double min_dim = std::min(size.x(), std::min(size.y(), size.z()));
    const double max_dim = std::max(size.x(), std::max(size.y(), size.z()));
    if (min_dim <= EPSILON)
        return true;

    const double volume = std::abs(its_volume(mesh.its));
    const double bbox_volume = size.x() * size.y() * size.z();
    if (volume <= EPSILON)
        return true;

    const double min_relative_thickness = 1e-6;
    const double min_volume_ratio = 1e-6;
    if (min_dim / max_dim <= min_relative_thickness)
        return true;
    if (bbox_volume > 0.0 && volume / bbox_volume <= min_volume_ratio)
        return true;

    return false;
}

// OrcaMCP: the msgids of the refusals that change nothing, translated where they are shown.
const char *const k_changed_meanwhile = L("The object changed while it was being repaired.");
const char *const k_leaves_nothing    = L("Repairing would leave the object nothing to print: every part is flat or empty.");

// OrcaMCP: the parts the repair works on for one volume. ModelVolume::split gives each part of a
// splittable volume a volume of its own (TriangleMesh::split, empty parts skipped) and centres it
// there (ModelVolume::center_geometry_after_creation); the repair reads that centred mesh, so the
// plan works on the same one, bit for bit. A volume that does not split is worked on whole.
struct VolumeParts
{
    std::vector<TriangleMesh> parts;
    bool                      split = false;
};

void centre_like_a_split_part(TriangleMesh &part)
{
    const Vec3d shift = part.bounding_box().center();
    if (!shift.isApprox(Vec3d::Zero())) {
        part.translate(-(float) shift(0), -(float) shift(1), -(float) shift(2));
        part.set_init_shift(shift);
    }
}

VolumeParts parts_of(const CgalRepairVolume &volume)
{
    VolumeParts out;
    if (volume.splittable)
        out.parts = volume.mesh->split();
    if (out.parts.size() <= 1) {
        out.parts = {*volume.mesh};
        return out;
    }
    out.split = true;
    out.parts.erase(std::remove_if(out.parts.begin(), out.parts.end(), [](const TriangleMesh &part) { return part.empty(); }),
                    out.parts.end());
    for (TriangleMesh &part : out.parts)
        centre_like_a_split_part(part);
    return out;
}

size_t mesh_hash(const indexed_triangle_set &its)
{
    auto bytes = [](const auto &items) {
        return std::string_view(reinterpret_cast<const char *>(items.data()), items.size() * sizeof(items.front()));
    };
    const size_t vertices = its.vertices.empty() ? 0 : std::hash<std::string_view>{}(bytes(its.vertices));
    const size_t indices  = its.indices.empty() ? 0 : std::hash<std::string_view>{}(bytes(its.indices));
    return vertices ^ (indices + 0x9e3779b97f4a7c15ULL + (vertices << 6) + (vertices >> 2));
}

bool same_mesh(const indexed_triangle_set &a, const indexed_triangle_set &b)
{
    return a.indices.size() == b.indices.size() && a.vertices.size() == b.vertices.size() && a.indices == b.indices &&
           a.vertices == b.vertices;
}

// OrcaMCP: painting, captured and remapped off the main thread, as ModelVolume::save_painting and
// restore_painting do it on the volume.
bool painted(const CgalRepairPainting &painting)
{
    return !painting.supported.triangles_to_split.empty() || !painting.seam.triangles_to_split.empty() ||
           !painting.mmu.triangles_to_split.empty() || !painting.fuzzy.triangles_to_split.empty();
}

CgalRepairPainting painting_of(const ModelVolume &volume)
{
    return {volume.supported_facets.get_data(), volume.seam_facets.get_data(), volume.mmu_segmentation_facets.get_data(),
            volume.fuzzy_skin_facets.get_data()};
}

// `painting`, laid on `source`, remapped onto `target`, placed by its init shift, as
// ModelVolume::restore_painting remaps a volume's saved painting onto its new mesh.
CgalRepairPainting remapped(const indexed_triangle_set &source, const CgalRepairPainting &painting, const TriangleMesh &target)
{
    const Transform3d to_target = Geometry::translation_transform(target.get_init_shift());
    auto one = [&](const TriangleSelector::TriangleSplittingData &data) {
        return data.bitstream.empty() ? TriangleSelector::TriangleSplittingData{} :
                                        TriangleSelector::remap_painting(source, data, target.its, to_target, std::nullopt);
    };
    return {one(painting.supported), one(painting.seam), one(painting.mmu), one(painting.fuzzy)};
}

// What ModelVolume::restore_painting leaves on a volume: its painting reset, then every non-empty kind
// set.
void set_painting(ModelVolume &volume, const CgalRepairPainting &painting)
{
    volume.reset_extra_facets();
    auto set = [](FacetsAnnotation &facets, const TriangleSelector::TriangleSplittingData &data) {
        if (!data.bitstream.empty())
            facets.set_data(TriangleSelector::TriangleSplittingData(data));
    };
    set(volume.supported_facets, painting.supported);
    set(volume.seam_facets, painting.seam);
    set(volume.mmu_segmentation_facets, painting.mmu);
    set(volume.fuzzy_skin_facets, painting.fuzzy);
}

// OrcaMCP: one part's repair in the loop: the plan's, worked out off the main thread, with the painting
// to give it. A part the plan did not foresee (or a plan made from an input alone) is repaired here, as
// upstream repaired every part.
bool repair_part(TriangleMesh &mesh, const ObjectID &target, CgalRepairPlan &plan, CgalRepairResult &result, std::string &error,
                 std::optional<TriangleMesh> &hull, std::optional<CgalRepairPainting> &painting)
{
    TriangleMesh planned_hull;
    switch (plan.take_repaired(target, mesh, planned_hull, painting)) {
    case CgalRepairPlan::Part::planned: hull = std::move(planned_hull); return true;
    case CgalRepairPlan::Part::failed: error = plan.error; return false;
    case CgalRepairPlan::Part::unplanned: break;
    }
    ++result.parts_repaired_here;
    if (plan.planned())
        BOOST_LOG_TRIVIAL(warning) << "Mesh repair: a part the plan did not foresee is repaired on the main thread";
    return MeshBoolean::cgal::repair(mesh, nullptr, &error);
}

// OrcaMCP: the loop's two tests of a part, taken from the plan when it foresaw the part.
bool drops(const CgalRepairPlan &plan, const ObjectID &target, const TriangleMesh &mesh)
{
    const CgalRepairPlan::PlannedPart *part = plan.find_part(target, mesh.its);
    return part != nullptr ? part->dropped : is_not_3dimensional_part(mesh);
}

bool has_open_edges(const CgalRepairPlan &plan, const ObjectID &target, const TriangleMesh &mesh)
{
    const CgalRepairPlan::PlannedPart *part = plan.find_part(target, mesh.its);
    return part != nullptr ? part->open || part->failed : its_num_open_edges(mesh.its) != 0;
}

std::array<ObjectWithTimestamp::Timestamp, 4> painting_stamps(const ModelVolume &volume)
{
    return {volume.supported_facets.timestamp(), volume.seam_facets.timestamp(), volume.mmu_segmentation_facets.timestamp(),
            volume.fuzzy_skin_facets.timestamp()};
}

// OrcaMCP: the Repair dialogs running, which let other main-thread work through. Main thread only.
int g_repair_dialogs_running = 0;

struct RepairDialogRunning
{
    RepairDialogRunning() { ++g_repair_dialogs_running; }
    ~RepairDialogRunning() { --g_repair_dialogs_running; }
};

} // namespace

// ---- OrcaMCP: capture, plan, apply --------------------------------------------------------------

CgalRepairInput capture_cgal_repair(const ModelObject &object, int volume_idx, bool keep_painting)
{
    CgalRepairInput input;
    const Model    *model = object.get_model();
    if (model != nullptr && std::find(model->objects.begin(), model->objects.end(), &object) != model->objects.end())
        input.model = model;
    input.object       = &object;
    input.object_id    = object.id();
    input.whole_object  = volume_idx < 0;
    input.volume_count  = object.volumes.size();
    input.keep_painting = keep_painting;
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume *volume = object.volumes[i];
        if (input.whole_object || int(i) == volume_idx) {
            CgalRepairVolume target{i, volume->id(), volume->mesh_ptr(), volume->is_splittable(), volume->is_model_part()};
            target.painting_stamps = painting_stamps(*volume);
            // What ModelVolume::save_painting keeps.
            if (keep_painting && volume->is_any_painted() && volume->is_model_part() && !volume->mesh().empty())
                target.painting = painting_of(*volume);
            input.targets.push_back(std::move(target));
        } else if (volume->is_model_part())
            ++input.other_model_parts;
    }
    return input;
}

bool CgalRepairPlan::applies_to(const ModelObject &object) const
{
    if (object.id() != m_input.object_id)
        return false;
    if (m_input.whole_object && object.volumes.size() != m_input.volume_count)
        return false;
    // A one-volume repair captured without its volume (an index the object did not have) repairs nothing.
    if (!m_input.whole_object && m_input.targets.empty())
        return false;
    for (const CgalRepairVolume &target : m_input.targets) {
        const auto it = std::find_if(object.volumes.begin(), object.volumes.end(),
                                     [&target](const ModelVolume *volume) { return volume->id() == target.id; });
        if (it == object.volumes.end() || (*it)->mesh_ptr() != target.mesh)
            return false;
        if (m_input.whole_object && size_t(it - object.volumes.begin()) != target.index)
            return false;
    }
    return true;
}

int CgalRepairPlan::volume_index_in(const ModelObject &object) const
{
    if (m_input.whole_object || m_input.targets.empty())
        return -1;
    for (size_t i = 0; i < object.volumes.size(); ++i)
        if (object.volumes[i]->id() == m_input.targets.front().id)
            return int(i);
    return -1;
}

void CgalRepairPlan::add_part(PlannedPart part)
{
    m_hashes.push_back(mesh_hash(part.its));
    m_parts.push_back(std::move(part));
}

const CgalRepairPlan::PlannedPart *CgalRepairPlan::find_part(const ObjectID &target, const indexed_triangle_set &its) const
{
    const size_t hash = mesh_hash(its);
    for (size_t i = 0; i < m_parts.size(); ++i)
        if (m_parts[i].target == target && m_hashes[i] == hash && same_mesh(its, m_parts[i].its))
            return &m_parts[i];
    return nullptr;
}

const std::optional<CgalRepairPainting> *CgalRepairPlan::take_split_painting(const ObjectID &target, const indexed_triangle_set &its)
{
    const size_t hash = mesh_hash(its);
    for (size_t i = 0; i < m_parts.size(); ++i)
        if (PlannedPart &part = m_parts[i];
            !part.painting_taken && part.target == target && m_hashes[i] == hash && same_mesh(its, part.its)) {
            part.painting_taken = true;
            return &part.painting;
        }
    return nullptr;
}

const CgalRepairVolume *CgalRepairPlan::target_of(const ModelVolume &volume) const
{
    for (const CgalRepairVolume &target : m_input.targets)
        if (target.id == volume.id())
            return &target;
    for (const CgalRepairVolume &target : m_input.targets)
        if (target.mesh == volume.mesh_ptr())
            return &target;
    return nullptr;
}

bool CgalRepairPlan::painting_unchanged(const ModelVolume &volume) const
{
    const CgalRepairVolume *target = target_of(volume);
    return target != nullptr && target->painting_stamps == painting_stamps(volume);
}

CgalRepairPlan::Part CgalRepairPlan::take_repaired(const ObjectID &target, TriangleMesh &part, TriangleMesh &hull,
                                                   std::optional<CgalRepairPainting> &painting)
{
    const size_t hash = mesh_hash(part.its);
    for (size_t i = 0; i < m_parts.size(); ++i) {
        PlannedPart &planned = m_parts[i];
        if (planned.taken || !(planned.open || planned.failed) || planned.target != target || m_hashes[i] != hash ||
            !same_mesh(part.its, planned.its))
            continue;
        if (planned.failed)
            return Part::failed;
        part          = std::move(planned.repaired);
        hull          = std::move(planned.repaired_hull);
        painting      = std::move(planned.repaired_painting);
        planned.taken = true;
        return Part::planned;
    }
    return Part::unplanned;
}

bool CgalRepairPlan::has_failed_part() const
{
    return std::any_of(m_parts.begin(), m_parts.end(), [](const PlannedPart &part) { return part.failed; });
}

CgalRepairPlan plan_cgal_repair(CgalRepairInput input, const CgalRepairProgress &progress, const std::function<bool()> &canceled)
{
    CgalRepairPlan plan(std::move(input));
    plan.set_planned();
    const std::vector<CgalRepairVolume> &targets = plan.input().targets;

    // Orca: the dialog's percent, over every volume repaired.
    const float total  = float(std::max<size_t>(1, targets.size()));
    auto        report = [&progress, total](const std::string &message, unsigned percent, size_t ordinal) {
        if (progress)
            progress(message, unsigned(std::floor((float(percent) + float(ordinal) * 100.f) / total)));
    };
    auto mark_canceled = [&plan, &report](size_t ordinal) {
        plan.canceled = true;
        report(L("Repair canceled"), 100, ordinal);
    };

    size_t model_parts_after = plan.input().other_model_parts;
    size_t ordinal           = 0;
    try {
        for (; ordinal < targets.size(); ++ordinal) {
            const CgalRepairVolume &volume = targets[ordinal];
            if (canceled()) {
                mark_canceled(ordinal);
                return plan;
            }
            report(L("Repairing model object"), 10, ordinal);

            VolumeParts split = parts_of(volume);
            if (split.split) {
                plan.parts_split += split.parts.size();
                report(Slic3r::format(L("Split into %1% parts"), split.parts.size()), 10, ordinal);
            }
            for (TriangleMesh &part : split.parts) {
                CgalRepairPlan::PlannedPart planned;
                planned.target = volume.id;
                // The painting ModelVolume::split leaves on the part (remapped onto it), or the whole's.
                if (volume.painting && split.split) {
                    if (canceled()) {
                        mark_canceled(ordinal);
                        return plan;
                    }
                    planned.painting = remapped(volume.mesh->its, *volume.painting, part);
                } else
                    planned.painting = volume.painting;
                planned.dropped = is_not_3dimensional_part(part);
                planned.open    = !planned.dropped && its_num_open_edges(part.its) != 0;
                if (!planned.dropped && volume.model_part)
                    ++model_parts_after;
                plan.parts_dropped += planned.dropped ? 1 : 0;
                if (!planned.open) {
                    planned.its = std::move(part.its);
                    plan.add_part(std::move(planned));
                    continue;
                }
                if (canceled()) {
                    mark_canceled(ordinal);
                    return plan;
                }
                TriangleMesh repaired = part;
                std::string  error;
                if (!MeshBoolean::cgal::repair(repaired, nullptr, &error)) {
                    planned.failed    = true;
                    planned.its       = std::move(part.its);
                    plan.error        = error.empty() ? L("Repair failed") : error;
                    plan.error_volume = int(volume.index);
                    plan.add_part(std::move(planned));
                    report(plan.error, 100, ordinal);
                    return plan;
                }
                // What restore_painting gives the repaired part: its painting remapped, when it has any.
                if (planned.painting && painted(*planned.painting) && volume.model_part) {
                    if (canceled()) {
                        mark_canceled(ordinal);
                        return plan;
                    }
                    planned.repaired_painting = remapped(part.its, *planned.painting, repaired);
                } else if (plan.input().keep_painting)
                    planned.repaired_painting = CgalRepairPainting{};
                if (canceled()) {
                    mark_canceled(ordinal);
                    return plan;
                }
                planned.repaired_hull = repaired.convex_hull_3d();
                planned.repaired      = std::move(repaired);
                planned.its           = std::move(part.its);
                plan.add_part(std::move(planned));
                ++plan.parts_repaired;
            }
            report(L("Repair finished"), 100, ordinal);
        }
    } catch (const std::exception &ex) {
        plan.error        = ex.what();
        plan.error_volume = ordinal < targets.size() ? int(targets[ordinal].index) : -1;
        report(plan.error, 100, std::min(ordinal, targets.size()));
        return plan;
    }
    plan.model_parts_after = model_parts_after;
    return plan;
}

std::string cgal_repair_refusal(const ModelObject *object, const CgalRepairPlan &plan)
{
    if (plan.canceled)
        return L("Repairing was canceled");
    if (object == nullptr || !plan.applies_to(*object))
        return k_changed_meanwhile;
    if (plan.leaves_no_model_part())
        return k_leaves_nothing;
    if (!plan.error.empty() && !plan.has_failed_part())
        return plan.error;
    return {};
}

CgalRepairResult apply_cgal_repair(ModelObject &model_object, bool keep_painting, CgalRepairPlan &plan)
{
    CgalRepairResult result;
    result.error = cgal_repair_refusal(&model_object, plan);
    if (!result.error.empty())
        return result;
    const int volume_idx = plan.volume_index_in(model_object);
    // OrcaMCP: the plan remapped the kept painting, off the main thread; the loop only sets it.
    const bool painting_planned = keep_painting && plan.planned() && plan.input().keep_painting;

    // Hold SaveObjectGaurd so the backup manager saves the object once, after the loop, not partway.
    SaveObjectGaurd backup_gaurd(model_object);

    try {
        size_t start_volume = volume_idx == -1 ? 0 : size_t(volume_idx);
        size_t end_volume   = volume_idx == -1 ? std::numeric_limits<size_t>::max() : size_t(volume_idx);

        for (size_t ivolume = start_volume; ivolume < model_object.volumes.size(); ++ivolume) {
            if (volume_idx != -1 && ivolume > end_volume)
                break;

            ModelVolume *volume = model_object.volumes[ivolume];
            // OrcaMCP: the captured volume these parts are (the split gives this one a new id, and so
            // does deleting every other volume of the object), and
            // whether its painting is still what the plan remapped: painting written since the capture
            // is remapped here, in place, as upstream remapped it.
            const CgalRepairVolume *target               = plan.target_of(*volume);
            const ObjectID          target_id            = target != nullptr ? target->id : volume->id();
            const bool              use_planned_painting = painting_planned && plan.painting_unchanged(*volume);

            // Orca: Split splittable volumes into parts for individual processing.
            size_t parts_count = 1;
            const bool splittable = volume->is_splittable();
            if (splittable)
                parts_count = volume->split(1, keep_painting && !use_planned_painting);

            size_t part_end = std::min(ivolume + parts_count - 1, model_object.volumes.size() - 1);
            if (volume_idx != -1)
                end_volume = part_end;

            // OrcaMCP: the painting the split would have remapped onto each part, from the plan. Also when
            // the split left one part (it drops parts without a convex hull), which it remapped too.
            if (use_planned_painting && splittable)
                for (size_t part_idx = ivolume; part_idx <= part_end; ++part_idx) {
                    ModelVolume *part_volume = model_object.volumes[part_idx];
                    const std::optional<CgalRepairPainting> *painting = plan.take_split_painting(target_id, part_volume->mesh().its);
                    if (painting == nullptr)
                        BOOST_LOG_TRIVIAL(warning) << "Mesh repair: a part the plan did not foresee keeps no painting";
                    else if (*painting)
                        set_painting(*part_volume, **painting);
                }

            size_t removed_parts = 0;
            for (size_t idx = part_end + 1; idx > ivolume; --idx) {
                const size_t part_idx = idx - 1;
                const ModelVolume *part_volume = model_object.volumes[part_idx];
                if (!drops(plan, target_id, part_volume->mesh()))
                    continue;
                // OrcaMCP: never the object's last model part. Upstream deleted it too, leaving an
                // object with no volume.
                if (part_volume->is_model_part() && model_object.parts_count() == 1)
                    throw Slic3r::RuntimeError(k_leaves_nothing);

                model_object.delete_volume(part_idx);
                ++removed_parts;
                if (part_end > 0)
                    --part_end;
                else
                    part_end = 0;
                if (volume_idx != -1)
                    end_volume = part_end;
            }

            if (removed_parts >= parts_count) {
                // OrcaMCP: the volume is gone, and the next one now has its index: go on from there.
                // Upstream went on from part_end, one short of that only when this was volume 0 (it
                // cannot go below 0), so it skipped the volume after a first volume dropped whole.
                if (volume_idx != -1)
                    break;
                --ivolume; // wraps below 0; the loop's ++ivolume brings it back
                continue;
            }

            for (size_t part_idx = ivolume; part_idx <= part_end && part_idx < model_object.volumes.size(); ++part_idx) {
                ModelVolume *part_volume = model_object.volumes[part_idx];
                TriangleMesh mesh = part_volume->mesh();
                if (has_open_edges(plan, target_id, mesh)) {

                    // Save painting for later remap
                    const std::optional<TriangleSelector::SavedPainting> saved_painting = keep_painting ?
                                                                                        part_volume->save_painting() :
                                                                                        std::optional<TriangleSelector::SavedPainting>{};

                    std::string error;
                    std::optional<TriangleMesh>       planned_hull;
                    std::optional<CgalRepairPainting> planned_painting;
                    if (!repair_part(mesh, target_id, plan, result, error, planned_hull, planned_painting))
                        throw Slic3r::RuntimeError(error.empty() ? L("Repair failed") : error);

                    part_volume->set_mesh(std::move(mesh));
                    // OrcaMCP: the plan's hull of the repaired mesh, worked out off the main thread.
                    if (planned_hull)
                        part_volume->set_convex_hull(std::move(*planned_hull));
                    else
                        part_volume->calculate_convex_hull();
                    part_volume->invalidate_convex_hull_2d();
                    part_volume->set_new_unique_id();

                    // Remap paint back. OrcaMCP: the plan remapped it off the main thread; a part it did
                    // not foresee is remapped here.
                    if (use_planned_painting && planned_painting)
                        set_painting(*part_volume, *planned_painting);
                    else
                        part_volume->restore_painting(saved_painting);
                }
            }

            if (volume_idx != -1) {
                result.first_volume = int(ivolume);
                result.last_volume  = int(part_end);
            }
            ivolume = part_end;
        }
    } catch (const std::exception &ex) {
        result.error = ex.what();
    }

    model_object.invalidate_bounding_box();
    return result;
}

bool cgal_repair_dialog_running() { return g_repair_dialogs_running > 0; }

int cgal_repair_object_index(const Model &model, const CgalRepairPlan &plan)
{
    const CgalRepairInput &input = plan.input();
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i] == input.object && model.objects[i]->id() == input.object_id)
            return int(i);
    return -1;
}

// Orca: Plans the repair on a worker thread, with progress dialog and cancellation support.
CgalRepairPlan plan_cgal_repair_with_dialog(CgalRepairInput input, GUI::ProgressDialog &progress_dialog, const wxString &msg_header)
{
    const RepairDialogRunning running;

    // Orca: Synchronization primitives for progress updates between worker thread and GUI.
    std::mutex mtx;
    std::condition_variable condition;
    struct Progress {
        std::string message;
        int         percent  = 0;
        bool        updated  = false;
    } progress;

    std::atomic<bool> canceled = false;
    std::atomic<bool> finished = false;

    // Orca: Lambda for updating progress from worker thread.
    auto on_progress = [&mtx, &condition, &progress](const std::string &msg, unsigned percent) {
        std::unique_lock<std::mutex> lock(mtx);
        progress.message = msg;
        progress.percent = int(percent);
        progress.updated = true;
        condition.notify_all();
    };

    // Orca: Worker thread that works out the repair. OrcaMCP: create_thread's stack, as the app's other
    // CGAL work has: a std::thread gets the platform's default (512 KB on macOS), and CGAL can recurse deeper.
    CgalRepairPlan plan;
    boost::thread  worker_thread = create_thread([&input, &plan, on_progress, &canceled, &finished]() {
        set_current_thread_name("cgal_fix_model");
        plan     = plan_cgal_repair(std::move(input), on_progress, [&canceled] { return canceled.load(); });
        finished = true;
    });

    // Orca: Main GUI loop to update progress dialog and handle cancellation.
    while (!finished) {
        std::unique_lock<std::mutex> lock(mtx);
        condition.wait_for(lock, std::chrono::milliseconds(250), [&progress]{ return progress.updated; });

        // Decrease progress percent slightly to avoid auto-closing.
        if (!progress_dialog.Update(progress.percent - 1, msg_header + _(progress.message)))
            canceled = true;
        else
            progress_dialog.Fit();

        progress.updated = false;
    }

    if (worker_thread.joinable())
        worker_thread.join();

    // A Cancel pressed after the plan last asked still cancels, as upstream's returned !canceled.
    if (canceled)
        plan.canceled = true;
    return plan;
}

// Orca: Main function to repair model objects using CGAL, with progress dialog and cancellation support.
// Returns false if fixing was canceled. fix_result contains error message if failed.
// OrcaMCP: planned under the dialog, then applied on the main thread; a canceled repair changes nothing.
bool fix_model_with_cgal_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dialog, const wxString &msg_header, std::string &fix_result, bool keep_painting)
{
    CgalRepairPlan plan = plan_cgal_repair_with_dialog(capture_cgal_repair(model_object, volume_idx, keep_painting), progress_dialog, msg_header);
    // The dialog lets other work run, which can delete the object: it is only read while its model lists it.
    if (plan.canceled || (plan.input().model != nullptr && cgal_repair_object_index(*plan.input().model, plan) < 0))
        return false;
    fix_result = apply_cgal_repair(model_object, keep_painting, plan).error;
    return true;
}

} // namespace Slic3r
