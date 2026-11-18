#include "FixModelByCgal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wx/string.h>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Thread.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/ProgressDialog.hpp"

namespace Slic3r {

namespace {

class RepairCanceledException : public std::exception
{
public:
    const char *what() const noexcept override { return "Model repair has been canceled"; }
};

// OrcaMCP: the msgid of the refusal that changes nothing, translated where it is shown.
const char *const k_changed_meanwhile = L("The object changed while it was being repaired.");

// OrcaMCP: painting, captured and remapped off the main thread, as ModelVolume::save_painting and
// restore_painting do it on the volume.
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

std::array<ObjectWithTimestamp::Timestamp, 4> painting_stamps(const ModelVolume &volume)
{
    return {volume.supported_facets.timestamp(), volume.seam_facets.timestamp(), volume.mmu_segmentation_facets.timestamp(),
            volume.fuzzy_skin_facets.timestamp()};
}

void add_stats(MeshRepairStats &sum, const MeshRepairStats &stats)
{
    sum.welded_vertices += stats.welded_vertices;
    sum.removed_triangles += stats.removed_triangles;
    sum.reversed_shells += stats.reversed_shells;
    sum.filled_holes += stats.filled_holes;
    sum.removed_shells += stats.removed_shells;
    sum.self_intersections += stats.self_intersections;
    sum.modified = sum.modified || stats.modified;
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
    input.object        = &object;
    input.object_id     = object.id();
    input.whole_object  = volume_idx < 0;
    input.volume_count  = object.volumes.size();
    input.keep_painting = keep_painting;
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume *volume = object.volumes[i];
        if (!input.whole_object && int(i) != volume_idx)
            continue;
        // Upstream's rule (db9968b9ee): a painted modifier follows the mesh of its host, never repaired itself.
        if (volume->is_painted_modifier())
            continue;
        CgalRepairVolume target{i, volume->id(), volume->name, volume->mesh_ptr()};
        target.painting_stamps = painting_stamps(*volume);
        // What ModelVolume::save_painting keeps.
        if (keep_painting && volume->is_any_painted() && volume->is_model_part() && !volume->mesh().empty())
            target.painting = painting_of(*volume);
        input.targets.push_back(std::move(target));
    }
    return input;
}

size_t CgalRepairPlan::volumes_changed() const
{
    return size_t(std::count_if(volumes.begin(), volumes.end(), [](const PlannedVolume &volume) { return volume.stats.modified; }));
}

MeshRepairStats CgalRepairPlan::totals() const
{
    MeshRepairStats sum;
    for (const PlannedVolume &volume : volumes)
        add_stats(sum, volume.stats);
    return sum;
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

const CgalRepairVolume *CgalRepairPlan::target_of(const ModelVolume &volume) const
{
    for (const CgalRepairVolume &target : m_input.targets)
        if (target.id == volume.id())
            return &target;
    return nullptr;
}

bool CgalRepairPlan::painting_unchanged(const ModelVolume &volume) const
{
    const CgalRepairVolume *target = target_of(volume);
    return target != nullptr && target->painting_stamps == painting_stamps(volume);
}

CgalRepairPlan plan_cgal_repair(CgalRepairInput input, const CgalRepairProgress &progress, const std::function<bool()> &canceled)
{
    CgalRepairPlan                       plan(std::move(input));
    const std::vector<CgalRepairVolume> &targets = plan.input().targets;
    const size_t                         count   = std::max<size_t>(1, targets.size());
    size_t                               i       = 0;
    try {
        for (; i < targets.size(); ++i) {
            const CgalRepairVolume &target = targets[i];
            // Upstream's progress: the volume's name when there are several, and the percent over all of them.
            const std::string name   = targets.size() > 1 ? target.name : std::string();
            const auto        report = [&](int percent) {
                if (canceled && canceled())
                    throw RepairCanceledException();
                if (progress)
                    progress(name, unsigned((100 * i + size_t(std::clamp(percent, 0, 100))) / count));
            };
            report(0);

            CgalRepairPlan::PlannedVolume planned;
            planned.target   = target.id;
            planned.repaired = *target.mesh;
            if (!repair_mesh(planned.repaired, &planned.stats, &planned.error, report)) {
                plan.error += (plan.error.empty() ? "" : "\n") + (targets.size() > 1 ? target.name + ": " : std::string()) + planned.error;
                if (plan.error_volume < 0)
                    plan.error_volume = int(target.index);
                planned.stats.modified = false;
                planned.repaired       = TriangleMesh();
            } else if (planned.stats.modified) {
                // What ModelVolume::repair gives the volume after set_mesh: its convex hull, and its painting
                // remapped by restore_painting.
                report(100);
                planned.repaired_hull = planned.repaired.convex_hull_3d();
                if (plan.input().keep_painting)
                    planned.repaired_painting = target.painting ? remapped(target.mesh->its, *target.painting, planned.repaired) :
                                                                  CgalRepairPainting{};
            } else
                planned.repaired = TriangleMesh();
            plan.volumes.push_back(std::move(planned));
        }
    } catch (const RepairCanceledException &) {
        plan.canceled = true;
    } catch (const std::exception &ex) {
        plan.error += (plan.error.empty() ? "" : "\n") + std::string(ex.what());
        if (plan.error_volume < 0 && i < targets.size())
            plan.error_volume = int(targets[i].index);
    }
    if (progress)
        progress(L("Repair finished"), 100);
    return plan;
}

std::string cgal_repair_refusal(const ModelObject *object, const CgalRepairPlan &plan)
{
    if (plan.canceled)
        return L("Repairing was canceled");
    if (object == nullptr || !plan.applies_to(*object))
        return k_changed_meanwhile;
    return {};
}

CgalRepairResult apply_cgal_repair(ModelObject &model_object, bool keep_painting, CgalRepairPlan &plan)
{
    CgalRepairResult result;
    result.error = cgal_repair_refusal(&model_object, plan);
    if (!result.error.empty())
        return result;
    // OrcaMCP: the plan remapped the kept painting, off the main thread; here it is only set.
    const bool painting_planned = keep_painting && plan.input().keep_painting;

    // Hold SaveObjectGaurd so the backup manager saves the object once, after the changes, not partway.
    SaveObjectGaurd backup_gaurd(model_object);
    for (CgalRepairPlan::PlannedVolume &planned : plan.volumes) {
        if (!planned.stats.modified || planned.taken)
            continue;
        const auto it = std::find_if(model_object.volumes.begin(), model_object.volumes.end(),
                                     [&planned](const ModelVolume *volume) { return volume->id() == planned.target; });
        if (it == model_object.volumes.end())
            continue;
        ModelVolume *volume = *it;
        // Painting written since the capture is remapped here, in place, as ModelVolume::repair remaps it.
        const bool use_planned_painting = painting_planned && planned.repaired_painting && plan.painting_unchanged(*volume);
        const std::optional<TriangleSelector::SavedPainting> saved_painting =
            keep_painting && !use_planned_painting ? volume->save_painting() : std::nullopt;

        // The end of ModelVolume::repair, with the plan's mesh and hull.
        volume->set_mesh(std::move(planned.repaired));
        volume->set_convex_hull(std::move(planned.repaired_hull));
        volume->invalidate_convex_hull_2d();
        // Assign a new unique ID, so that a new GLVolume will be generated.
        volume->set_new_unique_id();
        if (use_planned_painting)
            set_painting(*volume, *planned.repaired_painting);
        else
            volume->restore_painting(saved_painting);
        planned.taken = true;
        ++result.volumes_changed;
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

// Plans the repair on a worker thread, with progress dialog and cancellation support.
CgalRepairPlan plan_cgal_repair_with_dialog(CgalRepairInput input, GUI::ProgressDialog &progress_dialog, const wxString &msg_header)
{
    const RepairDialogRunning running;

    std::mutex              mtx;
    std::condition_variable condition;
    struct Progress
    {
        std::string message;
        int         percent = 0;
        bool        updated = false;
    } progress;

    std::atomic<bool> canceled = false;
    std::atomic<bool> finished = false;

    auto on_progress = [&mtx, &condition, &progress](const std::string &msg, unsigned percent) {
        std::lock_guard<std::mutex> lock(mtx);
        progress.message = msg;
        progress.percent = int(percent);
        progress.updated = true;
        condition.notify_all();
    };

    // OrcaMCP: create_thread's stack, as the app's other CGAL work has: a std::thread gets the platform's
    // default (512 KB on macOS), and CGAL can recurse deeper.
    CgalRepairPlan plan;
    boost::thread  worker_thread = create_thread([&input, &plan, on_progress, &canceled, &finished]() {
        set_current_thread_name("cgal_fix_model");
        plan     = plan_cgal_repair(std::move(input), on_progress, [&canceled] { return canceled.load(); });
        finished = true;
    });

    while (!finished) {
        std::unique_lock<std::mutex> lock(mtx);
        condition.wait_for(lock, std::chrono::milliseconds(250), [&progress] { return progress.updated; });
        // One percent short of the maximum, so that the dialog does not close itself.
        if (!progress_dialog.Update(std::max(0, progress.percent - 1), msg_header + _(progress.message)))
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

// Returns false if fixing was canceled. fix_result contains the error message if a volume failed.
// OrcaMCP: planned under the dialog, then applied on the main thread; a canceled repair changes nothing.
bool fix_model_with_cgal_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dialog, const wxString &msg_header,
                             std::string &fix_result, bool keep_painting, bool *modified)
{
    if (modified)
        *modified = false;
    CgalRepairPlan plan = plan_cgal_repair_with_dialog(capture_cgal_repair(model_object, volume_idx, keep_painting), progress_dialog, msg_header);
    // The dialog lets other work run, which can delete the object: it is only read while its model lists it.
    if (plan.canceled || (plan.input().model != nullptr && cgal_repair_object_index(*plan.input().model, plan) < 0))
        return false;
    const CgalRepairResult result = apply_cgal_repair(model_object, keep_painting, plan);
    fix_result = !result.error.empty() ? result.error : plan.error;
    if (modified)
        *modified = result.volumes_changed > 0;
    return true;
}

} // namespace Slic3r
