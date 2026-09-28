#ifndef slic3r_GUI_Utils_FixModelByCgal_hpp_
#define slic3r_GUI_Utils_FixModelByCgal_hpp_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "../GUI/Widgets/ProgressDialog.hpp"

namespace Slic3r {

class Model;
class ModelObject;
class Print;

// OrcaMCP: the repair in two halves, so that only the main thread ever changes the model.
//
// Upstream ran the whole repair on a worker thread against the live ModelObject while the progress
// dialog pumped events: it split, deleted and re-meshed volumes there, and created ModelVolumes,
// whose ids only the main thread may make (ObjectID.hpp), while the dialog's yields ran other
// main-thread work -- an MCP call's among it -- on the same object. Now the worker only plans: pure
// mesh work on the meshes captured on the main thread. The model changes afterwards, on the main
// thread, in upstream's own loop, which takes each repaired part from the plan. The object list's
// Repair (ObjectList::fix_through_cgal), the cut gizmo (fix_model_with_cgal_gui) and MCP's
// repair_mesh all run these steps: capture, plan, apply.

// One volume the repair reads, as captured on the main thread.
struct CgalRepairVolume
{
    size_t                              index = 0; // in the object's volumes when captured
    ObjectID                            id;
    std::shared_ptr<const TriangleMesh> mesh;      // immutable once shared: safe to read on any thread
    bool                                splittable = false;
    bool                                model_part = false;
};

// What repairing an object reads: every volume, or the one volume asked for.
struct CgalRepairInput
{
    const Model*                  model  = nullptr; // the model whose list held the object, or nullptr
    const ModelObject*            object = nullptr;
    ObjectID                      object_id;
    bool                          whole_object = true;
    size_t                        volume_count = 0;
    std::vector<CgalRepairVolume> targets;              // the volumes repaired, in volume order
    size_t                        other_model_parts = 0; // model parts the repair leaves alone
};

// Main thread.
CgalRepairInput capture_cgal_repair(const ModelObject& object, int volume_idx);

// The Repair dialog's progress: a message (a msgid, translated where it is shown) and a percent.
using CgalRepairProgress = std::function<void(const std::string& message, unsigned percent)>;

// What the repair will do to the captured volumes, worked out on the meshes alone: each splittable
// volume split into the parts ModelVolume::split makes, the parts with no volume dropped, and every
// other part with open edges repaired by MeshBoolean::cgal::repair. Applying it makes those changes.
class CgalRepairPlan
{
public:
    CgalRepairPlan() = default;
    // Nothing worked out: applying it repairs every part in place, as upstream did.
    explicit CgalRepairPlan(CgalRepairInput input) : m_input(std::move(input)) {}

    const CgalRepairInput& input() const { return m_input; }
    // Worked out by plan_cgal_repair, rather than made from an input alone.
    bool planned() const { return m_planned; }
    void set_planned() { m_planned = true; }

    bool        canceled = false; // stopped before the end: nothing may be applied
    std::string error;            // the part that could not be repaired, in the repair's words (a msgid when its own)
    int         error_volume = -1; // that part's volume, as captured
    size_t      parts_split    = 0; // the parts the splits made; 0 when no volume had several shells
    size_t      parts_dropped  = 0; // parts with no volume (flat or empty), deleted
    size_t      parts_repaired = 0; // parts whose holes are closed
    size_t      model_parts_after = 0;

    // Neither splits, drops nor repairs anything. Known only for a plan that was planned to its end.
    bool changes_nothing() const { return parts_split == 0 && parts_dropped == 0 && parts_repaired == 0; }
    // Would delete every model part of the object, leaving it nothing to print. Known only for a plan
    // that was planned to its end.
    bool leaves_no_model_part() const { return m_planned && error.empty() && !canceled && model_parts_after == 0; }

    // Whether `object` is still what was captured: the same object, with every volume the plan read
    // still there, holding the same mesh (and, for a whole-object repair, no volume added or removed).
    bool applies_to(const ModelObject& object) const;
    // The index in `object` of the one volume planned for; -1 for a whole-object plan.
    int volume_index_in(const ModelObject& object) const;

    // The part as planned: replaced by its repaired mesh (planned), its repair's failure (failed), or
    // a part the plan did not foresee (unplanned), which the caller repairs itself.
    enum class Part { planned, failed, unplanned };
    Part take_repaired(TriangleMesh& part);

    void add_repaired(indexed_triangle_set input, TriangleMesh repaired);
    void set_failed(indexed_triangle_set input, int volume, std::string error);
    // The error is a part's repair failing, rather than the plan itself.
    bool has_failed_part() const { return m_has_failed_part; }

private:
    struct RepairedPart
    {
        indexed_triangle_set input;
        TriangleMesh         repaired;
        bool                 taken = false;
    };
    CgalRepairInput           m_input;
    bool                      m_planned = false;
    std::vector<RepairedPart> m_repaired;
    indexed_triangle_set      m_failed_part;
    bool                      m_has_failed_part = false;
};

// Any thread: the expensive half. Reads only `input`'s meshes, never a Model, a ModelVolume or wx.
// `canceled` is asked before each volume and before each part's repair; a part being repaired runs to
// its end.
CgalRepairPlan plan_cgal_repair(CgalRepairInput input, const CgalRepairProgress& progress, const std::function<bool()>& canceled);

// What applying a plan did.
struct CgalRepairResult
{
    std::string error;                // why it stopped (a msgid when its own); what it changed before that stays
    size_t      parts_repaired_here = 0; // parts the plan had not foreseen, repaired on this thread
    int         first_volume = -1;    // a one-volume repair: the volumes it became, first and last
    int         last_volume  = -1;
};

// Main thread: why `plan` must not be applied to `object`, or "" when it can be: it was canceled, the
// object is gone (nullptr) or no longer what the plan read, the repair would leave it no part to print,
// or the plan failed other than on a part. A msgid, translated where it is shown.
std::string cgal_repair_refusal(const ModelObject* object, const CgalRepairPlan& plan);

// Main thread: upstream's loop, making the plan's changes to `object`, unless cgal_repair_refusal
// refuses it. Stops at a part the plan could not repair, keeping what it changed before it, as
// upstream did.
CgalRepairResult apply_cgal_repair(ModelObject& object, bool keep_painting, CgalRepairPlan& plan);

// Main thread: plans on a worker thread while `progress_dialog` shows the progress and offers Cancel.
CgalRepairPlan plan_cgal_repair_with_dialog(CgalRepairInput input, GUI::ProgressDialog& progress_dialog, const wxString& msg_header);

// Main thread: true while a Repair's progress dialog runs, when other main-thread work can be served.
bool cgal_repair_dialog_running();

// Main thread: the index of the captured object in `model`'s list, or -1 when that list no longer
// holds it. The dialog lets other work run, which can delete the object or move it in the list.
int cgal_repair_object_index(const Model& model, const CgalRepairPlan& plan);

// Return false if fixing was canceled. fix_result is empty on success.
extern bool fix_model_with_cgal_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dlg, const wxString &msg_header, std::string &fix_result, bool keep_painting);

} // namespace Slic3r

#endif /* slic3r_GUI_Utils_FixModelByCgal_hpp_ */
