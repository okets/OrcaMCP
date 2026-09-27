// src/slic3r/GUI/OrcaMCP/OrcaMCPInstanceBox.hpp
#pragma once
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {
class ModelObject;
class TriangleMesh;
namespace GUI { namespace OrcaMCP {

// The exact world box of one instance -- ModelObject::instance_bounding_box, its model parts'
// meshes under the instance's transform -- without walking every vertex on every call.
// instance_bounding_box is not cached upstream, and every per-plate description of an object is
// built from it (instances_on_plate: get_scene_info, a render's fit, the first-layer plan): about
// 1.1 s per get_scene_info on a 1M-facet mesh in the -O0 dev build, again on each call.
//
// Each model part's box is kept with what it was computed from: the part's mesh (by a weak pointer,
// so a freed mesh whose address is reused never matches), that mesh's own stored box (upstream
// scales and moves a mesh in place in its unit conversions and centring, behind the const of the
// shared pointer) and the combined instance x part matrix. A part whose inputs all match is not
// walked again; one that changed is walked as instance_bounding_box walks it, so the box is the
// same box to the last bit. Entries are keyed by the instance's ObjectID, which a slice's Print copy
// shares with the model's instance; a copy with another transform simply misses.
class InstanceBoxCache
{
public:
    BoundingBoxf3 box(const ModelObject& object, size_t instance);
    // How many part boxes were computed, for the tests.
    size_t part_walks() const { return m_part_walks; }

private:
    struct PartBox
    {
        std::weak_ptr<const TriangleMesh> mesh;
        Vec3f                             mesh_min = Vec3f::Zero(), mesh_max = Vec3f::Zero();
        Transform3d                       matrix   = Transform3d::Identity();
        BoundingBoxf3                     box;
    };
    static constexpr size_t MAX_INSTANCES = 4096; // then forget everything: deleted instances pile up otherwise

    std::unordered_map<size_t, std::vector<PartBox>> m_parts; // by ModelInstance ObjectID
    size_t                                           m_part_walks = 0;
};

// The app's cache. GUI thread only: every caller runs inside run_on_main_thread.
BoundingBoxf3 instance_box(const ModelObject& object, size_t instance);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
