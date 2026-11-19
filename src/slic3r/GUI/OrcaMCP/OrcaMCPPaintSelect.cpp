// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp
#include "OrcaMCPPaintSelect.hpp"
#include "OrcaMCPCommon.hpp"

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "slic3r/Utils/ThreadCancel.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// The two scalar guards every MCP parameter is read through, declared rather than included:
// OrcaMCPCommon.hpp pulls wxWidgets in through GUI_App.hpp, and this translation unit stays
// libslic3r-only so the camera codec below can be exercised headlessly. They are the same
// namespace, so a signature that drifts is a link error rather than a second definition.
bool parse_integer_param(const nlohmann::json& value, int& out);
bool parse_double_param(const nlohmann::json& value, double& out);

std::vector<int> facet_component_ids(const indexed_triangle_set& its, int& component_count)
{
    component_count = 0;
    std::vector<int> ids(its.indices.size(), -1);
    if (its.indices.empty())
        return ids;

    const std::vector<Vec3i32> neighbors = its_face_neighbors_par(its);
    std::vector<int>           stack;
    for (std::size_t seed = 0; seed < ids.size(); ++seed) {
        if (ids[seed] != -1)
            continue;
        const int id = component_count++;
        ids[seed]    = id;
        stack.push_back(int(seed));
        // Iterative flood fill: a 4-million-facet shell would overflow a recursive one.
        while (!stack.empty()) {
            const int facet = stack.back();
            stack.pop_back();
            for (int neighbor : neighbors[facet])
                if (neighbor >= 0 && ids[neighbor] == -1) {
                    ids[neighbor] = id;
                    stack.push_back(neighbor);
                }
        }
    }
    return ids;
}

std::vector<ComponentInfo> summarize_components(const indexed_triangle_set& its,
                                                const std::vector<int>&     ids,
                                                int                         component_count,
                                                const Transform3d&          to_plate)
{
    std::vector<ComponentInfo> out(std::size_t(std::max(component_count, 0)));
    for (int c = 0; c < component_count; ++c)
        out[c].component = c;

    for (std::size_t f = 0; f < its.indices.size() && f < ids.size(); ++f) {
        const int id = ids[f];
        if (id < 0 || id >= component_count)
            continue;
        const Vec3i32& face = its.indices[f];
        const Vec3d a = to_plate * its.vertices[face[0]].cast<double>();
        const Vec3d b = to_plate * its.vertices[face[1]].cast<double>();
        const Vec3d c = to_plate * its.vertices[face[2]].cast<double>();
        ComponentInfo& info = out[id];
        info.facet_count += 1;
        info.area += 0.5 * (b - a).cross(c - a).norm();
        info.bbox.merge(a);
        info.bbox.merge(b);
        info.bbox.merge(c);
    }
    return out;
}

std::vector<ComponentInfo> shells_most_facets_first(const indexed_triangle_set& its, const Transform3d& to_plate)
{
    int                        count  = 0;
    const std::vector<int>     ids    = facet_component_ids(its, count);
    std::vector<ComponentInfo> shells = summarize_components(its, ids, count, to_plate);
    std::stable_sort(shells.begin(), shells.end(),
                     [](const ComponentInfo& a, const ComponentInfo& b) { return a.facet_count > b.facet_count; });
    return shells;
}

nlohmann::json bbox_json(const BoundingBoxf3& bbox)
{
    return {{"min", {{"x", bbox.min.x()}, {"y", bbox.min.y()}, {"z", bbox.min.z()}}},
            {"max", {{"x", bbox.max.x()}, {"y", bbox.max.y()}, {"z", bbox.max.z()}}}};
}

nlohmann::json component_json(const ComponentInfo& component)
{
    return {{"component", component.component},
            {"facet_count", component.facet_count},
            {"area_mm2", component.area},
            {"bounding_box", bbox_json(component.bbox)}};
}

FacetAssignment assign_component(const std::vector<int>& ids, int component, int state)
{
    FacetAssignment out;
    out.states.assign(ids.size(), -1);
    out.unassigned = int(ids.size());
    for (std::size_t f = 0; f < ids.size(); ++f)
        if (ids[f] == component) {
            out.states[f] = state;
            --out.unassigned;
        }
    return out;
}

bool plate_transform_is_invertible(const Transform3d& to_plate)
{
    // FullPivLU rather than a fixed epsilon on the determinant: the threshold that decides
    // "singular" has to be relative to the matrix's own magnitude, or a legitimately tiny uniform
    // scale -- whose inverse is perfectly well defined -- would be refused along with a zero one.
    return Eigen::FullPivLU<Eigen::Matrix3d>(to_plate.linear()).isInvertible();
}

Vec3d facet_normal_plate(const indexed_triangle_set& its, int facet, const Transform3d& to_plate)
{
    if (facet < 0 || facet >= int(its.indices.size()) || !plate_transform_is_invertible(to_plate))
        return Vec3d::Zero();
    const Vec3i32& face = its.indices[facet];
    const Vec3d a = its.vertices[face[0]].cast<double>();
    const Vec3d b = its.vertices[face[1]].cast<double>();
    const Vec3d c = its.vertices[face[2]].cast<double>();
    const Vec3d n_local = (b - a).cross(c - a);
    if (n_local.squaredNorm() == 0.0)
        return Vec3d::Zero();
    // Normals transform by the inverse transpose of the linear part, not by the matrix itself.
    const Eigen::Matrix3d normal_matrix = to_plate.linear().inverse().transpose();
    return (normal_matrix * n_local).normalized();
}

bool pick_nearest_point(const TriangleMesh& mesh,
                        const Transform3d&  to_plate,
                        const Vec3d&        plate_point,
                        SurfacePick&        out)
{
    out = SurfacePick{};
    if (mesh.its.indices.empty() || !plate_transform_is_invertible(to_plate))
        return false;

    const AABBMesh aabb(mesh.its);
    const Vec3d    local_point = to_plate.inverse() * plate_point;
    int            face        = -1;
    Vec3d          closest     = Vec3d::Zero();
    aabb.squared_distance(local_point, face, closest);
    if (face < 0 || face >= int(mesh.its.indices.size()))
        return false;

    out.facet        = face;
    out.point_local  = closest;
    out.point_plate  = to_plate * closest;
    out.normal_plate = facet_normal_plate(mesh.its, face, to_plate);
    // Measured in the plate frame, so a scaled instance reports millimetres the caller can use.
    out.distance     = (out.point_plate - plate_point).norm();
    return true;
}

bool pick_ray(const TriangleMesh& mesh,
              const Transform3d&  to_plate,
              const Vec3d&        origin_plate,
              const Vec3d&        dir_plate,
              SurfacePick&        out)
{
    out = SurfacePick{};
    if (mesh.its.indices.empty() || dir_plate.squaredNorm() == 0.0 ||
        !plate_transform_is_invertible(to_plate))
        return false;

    const AABBMesh    aabb(mesh.its);
    const Transform3d to_local = to_plate.inverse();
    const Vec3d       origin   = to_local * origin_plate;
    const Vec3d       dir      = (to_local.linear() * dir_plate).normalized();
    const AABBMesh::hit_result hit = aabb.query_ray_hit(origin, dir);
    if (!hit.is_hit())
        return false;

    out.facet        = hit.face();
    out.point_local  = hit.position();
    out.point_plate  = to_plate * out.point_local;
    out.normal_plate = facet_normal_plate(mesh.its, out.facet, to_plate);
    out.distance     = (out.point_plate - origin_plate).norm();
    return true;
}

bool unproject_pixel_to_ray(const CameraFrame& camera, double px, double py, Vec3d& origin, Vec3d& dir)
{
    const int vx = camera.viewport[0], vy = camera.viewport[1];
    const int vw = camera.viewport[2], vh = camera.viewport[3];
    if (vw <= 0 || vh <= 0)
        return false;

    const Eigen::Matrix4d pv = camera.projection * camera.view;
    Eigen::FullPivLU<Eigen::Matrix4d> lu(pv);
    if (!lu.isInvertible())
        return false;
    const Eigen::Matrix4d inv = lu.inverse();

    // Normalised device coordinates. Image row 0 is the top, so y is flipped relative to NDC.
    const double nx = 2.0 * (px - vx) / vw - 1.0;
    const double ny = 1.0 - 2.0 * (py - vy) / vh;

    auto unproject = [&inv](double x, double y, double z, Vec3d& out) -> bool {
        const Eigen::Vector4d clip(x, y, z, 1.0);
        const Eigen::Vector4d world = inv * clip;
        if (std::abs(world.w()) < 1e-12)
            return false;
        out = world.head<3>() / world.w();
        return true;
    };

    Vec3d near_point, far_point;
    if (!unproject(nx, ny, -1.0, near_point) || !unproject(nx, ny, 1.0, far_point))
        return false;
    const Vec3d d = far_point - near_point;
    if (d.squaredNorm() == 0.0)
        return false;

    origin = near_point;
    dir    = d.normalized();
    return true;
}

nlohmann::json matrix4_to_json(const Eigen::Matrix4d& m)
{
    nlohmann::json out = nlohmann::json::array();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out.push_back(m(r, c));
    return out;
}

nlohmann::json camera_frame_to_json(const CameraFrame& camera)
{
    return {{"view_matrix", matrix4_to_json(camera.view)},
            {"projection_matrix", matrix4_to_json(camera.projection)},
            {"viewport", {camera.viewport[0], camera.viewport[1], camera.viewport[2], camera.viewport[3]}}};
}

bool parse_matrix4(const nlohmann::json& value, const char* what, Eigen::Matrix4d& out, std::string& error)
{
    if (!value.is_array() || value.size() != 16) {
        error = std::string(what) + " must be an array of 16 numbers, row-major";
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        double v = 0.0;
        if (!parse_double_param(value[i], v)) {
            error = std::string(what) + "[" + std::to_string(i) + "] is not a finite number";
            return false;
        }
        // Row-major, the order matrix4_to_json writes: element i is row i / 4, column i % 4.
        out(i / 4, i % 4) = v;
    }
    return true;
}

bool parse_camera_frame(const nlohmann::json& value, CameraFrame& out, std::string& error)
{
    if (!value.is_object() || !value.contains("view_matrix") || !value.contains("projection_matrix") ||
        !value.contains("viewport")) {
        error = "camera needs view_matrix, projection_matrix and viewport -- pass the `camera` object a "
                "render_plate_view result contains, unchanged";
        return false;
    }
    if (!parse_matrix4(value["view_matrix"], "camera.view_matrix", out.view, error) ||
        !parse_matrix4(value["projection_matrix"], "camera.projection_matrix", out.projection, error))
        return false;
    const nlohmann::json& vp = value["viewport"];
    if (!vp.is_array() || vp.size() != 4) {
        error = "camera.viewport must be [x, y, width, height]";
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        int v = 0;
        if (!parse_integer_param(vp[i], v)) {
            error = "camera.viewport[" + std::to_string(i) + "] is not an integer";
            return false;
        }
        out.viewport[std::size_t(i)] = v;
    }
    return true;
}

namespace {

// TriangleSelector keeps its per-triangle seed-fill flag protected. This reads it back for the
// original (unsplit) facets, which is all a fresh selector has: nothing is split until something
// paints with a cursor, and this code never does. The same subclassing move the GUI makes
// (TriangleSelectorGUI, GLGizmoPainterBase.hpp:33).
class SeedFillReader : public TriangleSelector
{
public:
    explicit SeedFillReader(const TriangleMesh& mesh) : TriangleSelector(mesh) {}

    std::vector<int> selected_original_facets() const
    {
        std::vector<int> out;
        for (int i = 0; i < m_orig_size_indices; ++i)
            if (!m_triangles[i].is_split() && m_triangles[i].is_selected_by_seed_fill())
                out.push_back(i);
        return out;
    }
};

} // namespace

FacetAssignment assign_connected(const TriangleMesh& mesh,
                                 const Transform3d&  to_plate,
                                 int                 seed_facet,
                                 const Vec3f&        seed_point_local,
                                 double              angle_deg,
                                 int                 state)
{
    FacetAssignment out;
    out.states.assign(mesh.its.indices.size(), -1);
    out.unassigned = int(out.states.size());
    if (mesh.its.indices.empty() || seed_facet < 0 || seed_facet >= int(mesh.its.indices.size()))
        return out;

    SeedFillReader selector(mesh);
    // The fill only reads the transform when painting overhangs only (highlight_by_angle_deg
    // != 0), which this never does; passed anyway, without translation, as the signature asks.
    Transform3d no_translate = to_plate;
    no_translate.translation() = Vec3d::Zero();
    // force_reselection = true: a fresh selector has nothing selected, but the early-return in
    // seed_fill_select_triangles is keyed on the start facet's flag, and asking for a recompute
    // is what a one-shot call means.
    selector.seed_fill_select_triangles(seed_point_local, seed_facet, no_translate,
                                        TriangleSelector::ClippingPlane(), float(angle_deg), 0.f, Vec3f::UnitZ(), true);

    for (int facet : selector.selected_original_facets()) {
        out.states[facet] = state;
        --out.unassigned;
    }
    return out;
}

bool parse_state_mapping(const nlohmann::json& value, PaintStateMap& out, std::string& error)
{
    if (!value.is_object() || value.empty()) {
        error = "mapping must be an object of old filament -> new filament, e.g. {\"1\": 2, \"2\": 3}";
        return false;
    }
    PaintStateMap mapping;
    for (const auto& [key, to_value] : value.items()) {
        int from = -1;
        int to   = -1;
        if (!parse_integer_param(nlohmann::json(key), from) || from < 0) {
            error = "mapping key \"" + key + "\" is not a state: use a whole number of 0 or more (0 = unpainted)";
            return false;
        }
        if (!parse_integer_param(to_value, to) || to < 0) {
            error = "mapping \"" + key + "\" -> " + to_value.dump() + ": the new state must be a whole number of 0 or more";
            return false;
        }
        mapping[from] = to;
    }
    out = std::move(mapping);
    return true;
}

std::string state_names(PaintMode mode)
{
    if (mode == PaintMode::Texture || mode == PaintMode::PaintedModifier)
        return "none or painted";
    return mode == PaintMode::FuzzySkin ? "none or enforcer" : "none, enforcer or blocker";
}

bool parse_state_param(const nlohmann::json& params, PaintMode mode, const StateParam& param, int& out, std::string& error)
{
    if (mode == PaintMode::Color) {
        const std::string key = param.filament_key;
        if (!params.contains(key)) {
            error = "mode 'color' needs " + key + ": the filament " + param.purpose + " (a 1-based slot, or 0 for unpainted)";
            return false;
        }
        // Through parse_integer_param rather than get<int>: a client whose cached tool schema predates
        // the parameter sends "2" as a string, and get<int> would throw a type_error instead.
        int value = 0;
        if (!parse_integer_param(params[key], value) || value < 0) {
            error = key + " must be a whole number of 0 or more: a 1-based slot, or 0 for unpainted";
            return false;
        }
        out = value;
        return true;
    }
    const std::string key    = param.state_key;
    const std::string states = state_names(mode);
    if (!params.contains(key) || !params[key].is_string()) {
        error = std::string("mode '") + paint_mode_name(mode) + "' needs " + key + ": the state " + param.purpose + " (" + states + ")";
        return false;
    }
    if (!parse_paint_state(mode, params[key].get<std::string>(), out)) {
        error = "Unknown " + key + " for mode '" + paint_mode_name(mode) + "': expected " + states +
                (mode == PaintMode::FuzzySkin ? " (fuzzy skin has no blocker: it is painted or it is not)" :
                 mode == PaintMode::Texture ? " (a texture layer applies where it is painted)" :
                 mode == PaintMode::PaintedModifier ? " (a painted modifier applies where it is painted)" : "");
        return false;
    }
    return true;
}

namespace {

// How far a ray starts off the surface, along its direction toward the viewer.
constexpr double k_visible_ray_offset_mm = 1e-4;

// A part's mesh for ray queries in its own frame, with the transform to and from the plate.
struct Occluder
{
    std::unique_ptr<AABBMesh> aabb;
    Transform3d               to_plate;
    Transform3d               to_local;
};

// True when a ray from `origin` (plate mm) along `dir` (unit) meets `occluder` before `limit` mm (infinity: anywhere).
bool ray_meets(const Occluder& occluder, const Vec3d& origin, const Vec3d& dir, double limit)
{
    const Vec3d local_dir = occluder.to_local.linear() * dir;
    if (local_dir.squaredNorm() == 0.0)
        return false;
    const AABBMesh::hit_result hit = occluder.aabb->query_ray_hit(occluder.to_local * origin, local_dir.normalized());
    if (!hit.is_hit())
        return false;
    return std::isinf(limit) || (occluder.to_plate * hit.position() - origin).norm() < limit;
}

} // namespace

std::vector<FacetAssignment> assign_visible(const std::vector<PaintPlanVolume>& targets,
                                            const std::vector<PaintPlanVolume>& occluder_volumes,
                                            const VisibleView&                  view,
                                            int                                 state)
{
    std::vector<Occluder> occluders;
    for (const PaintPlanVolume& pv : occluder_volumes)
        if (pv.mesh && !pv.mesh->its.indices.empty() && plate_transform_is_invertible(pv.to_plate))
            occluders.push_back({std::make_unique<AABBMesh>(pv.mesh->its), pv.to_plate, pv.to_plate.inverse()});

    const Vec3d forward = view.forward.squaredNorm() > 0.0 ? Vec3d(view.forward.normalized()) : Vec3d(-Vec3d::UnitZ());
    std::vector<FacetAssignment> out;
    for (const PaintPlanVolume& pv : targets) {
        const indexed_triangle_set& its = pv.mesh->its;
        FacetAssignment             assignment;
        assignment.states.assign(its.indices.size(), -1);
        // A quit ends the work: what is left stays unselected, and the caller's next hop is refused by the gate.
        if (plate_transform_is_invertible(pv.to_plate) && !Slic3r::this_thread_cancelled()) {
            // A mirroring transform (negative determinant) turns the winding over, and the normal with it.
            const double winding = pv.to_plate.linear().determinant() < 0.0 ? -1.0 : 1.0;
            tbb::parallel_for(tbb::blocked_range<size_t>(0, its.indices.size(), 256), [&](const tbb::blocked_range<size_t>& range) {
                if (Slic3r::this_thread_cancelled())
                    return;
                for (size_t f = range.begin(); f < range.end(); ++f) {
                    const Vec3i32& face = its.indices[f];
                    const Vec3d    a    = its.vertices[face[0]].cast<double>();
                    const Vec3d    b    = its.vertices[face[1]].cast<double>();
                    const Vec3d    c    = its.vertices[face[2]].cast<double>();
                    const Vec3d    pa     = pv.to_plate * a;
                    const Vec3d    normal = winding * (pv.to_plate * b - pa).cross(pv.to_plate * c - pa);
                    if (normal.squaredNorm() < 1e-20)
                        continue; // a degenerate facet has no facing
                    const Vec3d centroid = pv.to_plate * ((a + b + c) / 3.0);
                    const Vec3d seen_along = view.ortho ? forward : Vec3d(centroid - view.eye);
                    if (normal.dot(seen_along) >= 0.0)
                        continue; // facing away
                    if (view.frame) {
                        Vec2f uv;
                        if (!project_uv_projective(*view.frame, (view.plate_to_frame * centroid).cast<float>(), uv) ||
                            uv.x() < 0.f || uv.x() > 1.f || uv.y() < 0.f || uv.y() > 1.f)
                            continue;
                    }
                    const Vec3d toward = view.ortho ? Vec3d(-forward) : Vec3d(view.eye - centroid);
                    if (toward.squaredNorm() == 0.0)
                        continue;
                    const Vec3d    dir    = toward.normalized();
                    const Vec3d    origin = centroid + dir * k_visible_ray_offset_mm;
                    const double   limit  = view.ortho ? std::numeric_limits<double>::infinity() : toward.norm();
                    const bool     hidden = std::any_of(occluders.begin(), occluders.end(),
                                                    [&](const Occluder& o) { return ray_meets(o, origin, dir, limit); });
                    if (!hidden)
                        assignment.states[f] = state;
                }
            });
        }
        assignment.unassigned = int(std::count(assignment.states.begin(), assignment.states.end(), -1));
        out.push_back(std::move(assignment));
    }
    return out;
}

}}} // namespace Slic3r::GUI::OrcaMCP
