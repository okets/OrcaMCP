// src/slic3r/GUI/OrcaMCP/OrcaMCPInstanceBox.cpp
#include "OrcaMCPInstanceBox.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

bool same_inputs(const std::weak_ptr<const TriangleMesh>& kept, const std::shared_ptr<const TriangleMesh>& mesh,
                 const Vec3f& kept_min, const Vec3f& kept_max, const Transform3d& kept_matrix, const Transform3d& matrix)
{
    const std::shared_ptr<const TriangleMesh> alive = kept.lock();
    return alive != nullptr && alive == mesh && kept_min == mesh->stats().min && kept_max == mesh->stats().max &&
           kept_matrix.matrix() == matrix.matrix();
}

} // namespace

BoundingBoxf3 InstanceBoxCache::box(const ModelObject& object, size_t instance)
{
    const ModelInstance& inst = *object.instances[instance];
    if (m_parts.size() > MAX_INSTANCES && m_parts.find(inst.id().id) == m_parts.end())
        m_parts.clear();
    std::vector<PartBox>& parts = m_parts[inst.id().id];

    const Transform3d inst_matrix = inst.get_transformation().get_matrix();
    BoundingBoxf3     box;
    size_t            part = 0;
    for (const ModelVolume* volume : object.volumes) {
        if (!volume->is_model_part())
            continue;
        const std::shared_ptr<const TriangleMesh>& mesh   = volume->get_mesh_shared_ptr();
        const Transform3d                          matrix = inst_matrix * volume->get_matrix();
        if (part == parts.size())
            parts.emplace_back();
        PartBox& kept = parts[part++];
        if (!same_inputs(kept.mesh, mesh, kept.mesh_min, kept.mesh_max, kept.matrix, matrix)) {
            // Exactly the walk ModelObject::instance_bounding_box makes for this part.
            kept = {mesh, mesh->stats().min, mesh->stats().max, matrix, mesh->transformed_bounding_box(matrix)};
            ++m_part_walks;
        }
        box.merge(kept.box);
    }
    parts.resize(part);
    return box;
}

BoundingBoxf3 instance_box(const ModelObject& object, size_t instance)
{
    static InstanceBoxCache cache;
    return cache.box(object, instance);
}

}}} // namespace Slic3r::GUI::OrcaMCP
