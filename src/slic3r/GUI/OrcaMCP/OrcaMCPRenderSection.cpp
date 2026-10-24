#include "OrcaMCPRenderSection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

const char* const k_usage =
    "section must be \"app\" (the cut the 3D view's section view shows now), {normal: [x, y, z], point: [x, y, z]}, "
    "{normal: [x, y, z], ratio: 0..1} or {plane: \"x\"|\"y\"|\"z\", at_mm}";

Vec3d read_triple(const nlohmann::json& value, const char* name)
{
    if (!value.is_array() || value.size() != 3 || !std::all_of(value.begin(), value.end(), [](const nlohmann::json& v) { return v.is_number(); }))
        throw std::runtime_error(std::string("section.") + name + " must be [x, y, z], three numbers");
    return Vec3d(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
}

double read_number(const nlohmann::json& value, const char* name)
{
    if (!value.is_number())
        throw std::runtime_error(std::string("section.") + name + " must be a number");
    return value.get<double>();
}

Vec3d read_normal(const nlohmann::json& value)
{
    const Vec3d normal = read_triple(value, "normal");
    if (!(normal.norm() > 1e-9))
        throw std::runtime_error("section.normal must not be the zero vector");
    return normal.normalized();
}

} // namespace

SectionRequest read_section(const nlohmann::json& section)
{
    SectionRequest out;
    if (section.is_string()) {
        if (section != "app")
            throw std::runtime_error(k_usage);
        out.form = SectionRequest::Form::App;
        return out;
    }
    if (!section.is_object())
        throw std::runtime_error(k_usage);
    // What an answer's section carries besides its form is accepted back and ignored.
    static const char* const ignored[] = {"source", "removes", "ratio_used", "cap_color", "applied", "empty", "objects", "notes", "warning"};
    for (const auto& item : section.items()) {
        const std::string& key = item.key();
        const bool known = key == "normal" || key == "point" || key == "ratio" || key == "plane" || key == "at_mm" || key == "remove" || key == "frame";
        if (!known && std::find_if(std::begin(ignored), std::end(ignored), [&key](const char* name) { return key == name; }) == std::end(ignored))
            throw std::runtime_error("section has no key \"" + key + "\"; " + k_usage);
    }
    const bool has_plane = section.contains("plane") || section.contains("at_mm") || section.contains("remove");
    const bool has_point = section.contains("point");
    const bool has_ratio = section.contains("ratio");
    if (has_plane && (section.contains("normal") || has_point || has_ratio))
        throw std::runtime_error("section: plane / at_mm / remove is one form and normal / point / ratio another; send one. " + std::string(k_usage));
    if (has_point && has_ratio)
        throw std::runtime_error("section: send a point or a ratio with the normal, not both. " + std::string(k_usage));

    if (section.contains("frame")) {
        if (!section["frame"].is_string() || (section["frame"] != "bed_mm" && section["frame"] != "plate_local"))
            throw std::runtime_error("section.frame must be \"bed_mm\" or \"plate_local\"");
        out.plate_local = section["frame"] == "plate_local";
    }

    if (has_plane) {
        if (!section.contains("plane") || !section.contains("at_mm"))
            throw std::runtime_error("section: a plane form needs both plane (\"x\", \"y\" or \"z\") and at_mm. " + std::string(k_usage));
        const nlohmann::json& plane = section["plane"];
        if (!plane.is_string() || (plane != "x" && plane != "y" && plane != "z"))
            throw std::runtime_error("section.plane must be \"x\", \"y\" or \"z\"");
        const double at = read_number(section["at_mm"], "at_mm");
        double       sign = 1.;
        if (section.contains("remove")) {
            const nlohmann::json& remove = section["remove"];
            if (!remove.is_string() || (remove != "above" && remove != "below"))
                throw std::runtime_error("section.remove must be \"above\" (the larger coordinates, default) or \"below\"");
            sign = remove == "below" ? -1. : 1.;
        }
        const int axis = plane == "x" ? 0 : plane == "y" ? 1 : 2;
        out.form       = SectionRequest::Form::Axis;
        out.normal     = Vec3d::Zero();
        out.normal[axis] = sign;
        out.point      = Vec3d::Zero();
        out.point[axis]  = at;
        return out;
    }

    if (!section.contains("normal"))
        throw std::runtime_error("section needs a normal with point or ratio. " + std::string(k_usage));
    out.normal = read_normal(section["normal"]);
    if (has_ratio) {
        if (section.contains("frame"))
            throw std::runtime_error("section.frame is for a point; a ratio sweeps the objects' own bounding sphere");
        out.form  = SectionRequest::Form::Ratio;
        out.ratio = read_number(section["ratio"], "ratio");
        if (!(out.ratio > 0. && out.ratio <= 1.))
            throw std::runtime_error("section.ratio must be above 0 and at most 1 (0 is no cut, 1 sweeps the whole bounding sphere)");
        return out;
    }
    if (!has_point)
        throw std::runtime_error("section needs a point or a ratio with the normal. " + std::string(k_usage));
    out.form  = SectionRequest::Form::PointNormal;
    out.point = read_triple(section["point"], "point");
    return out;
}

SectionPlane plane_through_point(const Vec3d& normal, const Vec3d& point)
{
    SectionPlane plane;
    plane.normal = normal.normalized();
    plane.offset = plane.normal.dot(point);
    return plane;
}

std::array<double, 4> shader_clipping_plane(const SectionPlane& plane)
{
    return {-plane.normal.x(), -plane.normal.y(), -plane.normal.z(), plane.offset};
}

const char* section_effect_name(SectionEffect effect)
{
    switch (effect) {
    case SectionEffect::Cut:        return "cut";
    case SectionEffect::RemovesAll: return "removes_all";
    default:                        return "keeps_all";
    }
}

namespace {

// The plate's slice of one part at the plane, as outlines in the plane's own frame (z = the plane's offset).
ExPolygons slice_part(const SectionPart& part, const Transform3d& to_plane_frame)
{
    MeshSlicingParams params;
    params.trafo = to_plane_frame * part.world;
    return union_ex(slice_mesh(part.mesh->its, float(0.), params));
}

} // namespace

SectionCap section_cap(const SectionBody& body, const SectionPlane& plane, const std::function<bool()>& stop)
{
    SectionCap cap;
    cap.object_id   = body.object_id;
    cap.instance_id = body.instance_id;

    // Which sides of the plane the solid lies on.
    double lowest = std::numeric_limits<double>::max(), highest = std::numeric_limits<double>::lowest();
    for (const SectionPart& part : body.parts) {
        if (part.negative || !part.mesh)
            continue;
        for (const Vec3f& v : part.mesh->its.vertices) {
            const double d = plane.signed_distance(part.world * v.cast<double>());
            lowest  = std::min(lowest, d);
            highest = std::max(highest, d);
        }
    }
    if (lowest > highest)
        return cap;   // no solid
    // Touching the plane is not cut by it.
    constexpr double tolerance = 1e-6;
    cap.effect = highest <= tolerance ? SectionEffect::KeepsAll : lowest >= -tolerance ? SectionEffect::RemovesAll : SectionEffect::Cut;
    if (cap.effect != SectionEffect::Cut)
        return cap;

    // Rotate the world so the plane is z = offset, and slice at that height.
    const Eigen::Quaterniond rotation = Eigen::Quaterniond::FromTwoVectors(plane.normal, Vec3d::UnitZ());
    Transform3d to_plane_frame = Transform3d::Identity();
    to_plane_frame.rotate(rotation);
    to_plane_frame.pretranslate(Vec3d(0., 0., -plane.offset));   // the plane is z = 0 now

    ExPolygons solid, holes;
    for (const SectionPart& part : body.parts) {
        if (stop()) {
            cap.complete = false;
            return cap;
        }
        if (!part.mesh)
            continue;
        ExPolygons slice = slice_part(part, to_plane_frame);
        append(part.negative ? holes : solid, std::move(slice));
    }
    ExPolygons cut = union_ex(solid);
    if (!holes.empty())
        cut = diff_ex(cut, union_ex(holes));

    double scaled_area = 0.;
    for (const ExPolygon& poly : cut)
        scaled_area += poly.area();
    cap.area_mm2 = scaled_area * SCALING_FACTOR * SCALING_FACTOR;
    cap.islands  = int(cut.size());

    // Triangulated on the plane, turned back into world mm, a hair toward the removed side so it sits clear of the walls.
    const std::vector<Vec3d> corners = triangulate_expolygons_3d(cut, 0.);
    const Transform3d        back    = to_plane_frame.inverse();
    cap.triangles.vertices.reserve(corners.size());
    cap.triangles.indices.reserve(corners.size() / 3);
    for (size_t i = 0; i + 2 < corners.size(); i += 3) {
        const int first = int(cap.triangles.vertices.size());
        for (size_t k = 0; k < 3; ++k)
            cap.triangles.vertices.emplace_back(((back * corners[i + k]) + 0.001 * plane.normal).cast<float>());
        cap.triangles.indices.emplace_back(first, first + 1, first + 2);
    }
    return cap;
}

nlohmann::json section_json(const SectionRequest& asked, const SectionPlane& plane, const std::vector<SectionCap>& caps,
                            const std::vector<std::pair<int, int>>& drawn, bool applied)
{
    using nlohmann::json;
    const Vec3d closest = plane.normal * plane.offset;
    json out;
    out["normal"]    = {plane.normal.x(), plane.normal.y(), plane.normal.z()};
    out["point"]     = {closest.x(), closest.y(), closest.z()};
    out["source"]    = asked.form == SectionRequest::Form::App ? "app" : asked.form == SectionRequest::Form::Ratio ? "ratio" : "plane";
    out["removes"]   = "the side the normal points to";
    if (asked.form == SectionRequest::Form::App || asked.form == SectionRequest::Form::Ratio)
        out["ratio_used"] = asked.ratio;
    out["cap_color"] = "#404040";
    out["applied"]   = applied;

    // The instances the picture shows, grouped by object in the order they were cut.
    std::vector<int>                       object_order;
    std::vector<std::vector<const SectionCap*>> by_object;
    bool any_cut = false, any_kept = false, any_removed = false;
    for (const SectionCap& cap : caps) {
        if (std::find(drawn.begin(), drawn.end(), std::make_pair(cap.object_id, cap.instance_id)) == drawn.end())
            continue;
        any_cut     |= cap.effect == SectionEffect::Cut;
        any_kept    |= cap.effect == SectionEffect::KeepsAll;
        any_removed |= cap.effect == SectionEffect::RemovesAll;
        auto at = std::find(object_order.begin(), object_order.end(), cap.object_id);
        if (at == object_order.end()) {
            object_order.push_back(cap.object_id);
            by_object.emplace_back();
            at = object_order.end() - 1;
        }
        by_object[size_t(at - object_order.begin())].push_back(&cap);
    }
    json objects = json::array();
    for (size_t i = 0; i < object_order.size(); ++i) {
        double area = 0.;
        int    islands = 0;
        SectionEffect effect = SectionEffect::KeepsAll;
        json instances = json::array();
        for (const SectionCap* cap : by_object[i]) {
            area += cap->area_mm2;
            islands += cap->islands;
            if (cap->effect == SectionEffect::Cut || (cap->effect == SectionEffect::RemovesAll && effect == SectionEffect::KeepsAll))
                effect = cap->effect;
            instances.push_back({{"instance_id", cap->instance_id}, {"section_area_mm2", cap->area_mm2}, {"islands", cap->islands},
                                 {"effect", section_effect_name(cap->effect)}});
        }
        json entry = {{"object_id", object_order[i]}, {"section_area_mm2", area}, {"islands", islands}, {"effect", section_effect_name(effect)}};
        if (instances.size() > 1)
            entry["instances"] = std::move(instances);
        objects.push_back(std::move(entry));
    }
    out["objects"] = objects;
    out["empty"]   = !any_cut;

    json notes = json::array();
    if (!any_cut) {
        if (objects.empty())
            notes.push_back("Nothing was drawn, so there was nothing for the plane to cut.");
        else if (any_removed && !any_kept)
            notes.push_back("The plane removes every object entirely and cuts none: the picture shows nothing of them. Move the plane or flip the normal.");
        else if (any_kept && !any_removed)
            notes.push_back("The plane cuts no object (they all lie on the kept side): this picture is the uncut view. Move the plane or flip the normal.");
        else
            notes.push_back("The plane cuts no object: some lie wholly on the kept side and some wholly on the removed side. Move the plane.");
    }
    if (!applied)
        notes.push_back("The section was not applied: the render shader that cuts was not available.");
    if (!notes.empty())
        out["notes"] = notes;
    return out;
}

}}} // namespace Slic3r::GUI::OrcaMCP
