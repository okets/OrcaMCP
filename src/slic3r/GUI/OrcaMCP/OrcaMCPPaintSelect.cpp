// src/slic3r/GUI/OrcaMCP/OrcaMCPPaintSelect.cpp
#include "OrcaMCPPaintSelect.hpp"

#include "libslic3r/AABBMesh.hpp"

#include <algorithm>
#include <cmath>
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

Vec3d facet_normal_plate(const indexed_triangle_set& its, int facet, const Transform3d& to_plate)
{
    if (facet < 0 || facet >= int(its.indices.size()))
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
    if (mesh.its.indices.empty())
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
    if (mesh.its.indices.empty() || dir_plate.squaredNorm() == 0.0)
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
                                        TriangleSelector::ClippingPlane(), float(angle_deg), 0.f, true);

    for (int facet : selector.selected_original_facets()) {
        out.states[facet] = state;
        --out.unassigned;
    }
    return out;
}

}}} // namespace Slic3r::GUI::OrcaMCP
