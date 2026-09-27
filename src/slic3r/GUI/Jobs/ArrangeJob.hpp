#ifndef ARRANGEJOB_HPP
#define ARRANGEJOB_HPP


#include <atomic>
#include <functional>
#include <optional>
#include <vector>

#include "Job.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/ObjectID.hpp"

namespace Slic3r {

class ModelInstance;

namespace GUI {

class Plater;
class PartPlateList;
class NotificationManager;

// Orca: what an arrange sets up for its run, undone when it ends -- however it ends: the plates
// prepare_all locked because their print sequence differs from the global one (`plates_to_unlock`,
// by identity: a plate of a list that replaced them since, a new or opened project's, is never
// touched), the plater's "an arrange is running" flag, and the "Arranging..." notification. Upstream
// undid them only after an arrange it applied, so a cancelled or failed one left those plates locked,
// the plate toolbar's arrange button refusing, and the notification up until it timed out.
void end_arrange_run(PartPlateList& plates, const std::vector<ObjectID>& plates_to_unlock, std::atomic<bool>& arrange_running,
                     const std::function<void()>& close_notification);

class ArrangeJob : public Job
{
    using ArrangePolygon = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    //BBS: add locked logic
    ArrangePolygons m_selected, m_unselected, m_unprintable, m_locked;
    std::vector<ModelInstance*> m_unarranged;
    std::map<int, ArrangePolygons> m_selected_groups;   // groups of selected items for sequential printing
    std::vector<ObjectID> m_uncompatible_plates;  // Orca: the plates with a printing sequence other than the global one, by identity

    arrangement::ArrangeParams params;
    int current_plate_index = 0;
    Polygon bed_poly;
    Plater *m_plater;
    // Orca: what end_arrange_run undoes, taken when the job is made, while the plater is whole. A late
    // finalize -- one ~Plater's drain did not see through -- runs in ~priv, where Plater::p is null;
    // these live in parts of the plater that outlive its worker.
    PartPlateList*       m_plate_list      = nullptr;
    NotificationManager* m_notifications   = nullptr;
    std::atomic<bool>*   m_arrange_running = nullptr;

    // BBS: add flag for whether on current part plate
    bool only_on_partplate{false};

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();

    // Prepare the selected and unselected items separately. If nothing is
    // selected, behaves as if everything would be selected.
    void prepare_selected();

    void prepare_all();

    //BBS:prepare the items from current selected partplate
    void prepare_partplate();
    void prepare_wipe_tower();

    ArrangePolygon prepare_arrange_polygon(void* instance);

protected:

    void check_unprintable();

public:

    void prepare();

    void process(Ctl &ctl) override;

    ArrangeJob();

    int status_range() const
    {
        // ensure finalize() is called after all operations in process() is finished.
        return int(m_selected.size() + m_unprintable.size() + 1);
    }

    void finalize(bool canceled, std::exception_ptr &e) override;
};

std::optional<arrangement::ArrangePolygon> get_wipe_tower_arrangepoly(const Plater &);

// The gap between logical beds in the x axis expressed in ratio of
// the current bed width.
static const constexpr double LOGICAL_BED_GAP = 1. / 5.;

//BBS: add sudoku-style strides for x and y
// Stride between logical beds
double bed_stride_x(const Plater* plater);
double bed_stride_y(const Plater* plater);

arrangement::ArrangeParams init_arrange_params(Plater *p);

}} // namespace Slic3r::GUI

#endif // ARRANGEJOB_HPP
