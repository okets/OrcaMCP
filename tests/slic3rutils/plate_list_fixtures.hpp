#pragma once

// A plate list without the app, and an object whose instances stand where a test puts them: the CLI's
// PartPlateList (no Plater), sized for a 256 mm square bed.

#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <memory>
#include <vector>

namespace plate_list_fixtures {

constexpr double k_plate_size = 256.0;
constexpr double k_cube_size  = 20.0;

// The plate list of a 256 mm square bed, with `count` plates.
inline std::unique_ptr<Slic3r::GUI::PartPlateList> plate_list_for(Slic3r::Model& model, int count)
{
    auto plates = std::make_unique<Slic3r::GUI::PartPlateList>(int(k_plate_size), int(k_plate_size), k_plate_size, nullptr,
                                                               &model, Slic3r::ptFFF);
    const Slic3r::Pointfs bed{{0.0, 0.0}, {k_plate_size, 0.0}, {k_plate_size, k_plate_size}, {0.0, k_plate_size}};
    plates->set_shapes(bed, {}, {}, {}, {}, "", 0.f, 0.f);
    while (plates->get_plate_count() < count)
        plates->create_plate(false);
    return plates;
}

// Where a cube resting on `plate` sits when it is centred on it.
inline Slic3r::Vec3d centre_of(Slic3r::GUI::PartPlateList& plates, int plate)
{
    const Slic3r::Vec3d centre = plates.get_plate(plate)->get_plate_box().center();
    return {centre.x(), centre.y(), k_cube_size / 2.0};
}

// A 20 mm cube with an instance at each of `positions`, added to the scene as the app adds a loaded
// object (ObjectList::add_object_to_list: PartPlateList::notify_object_added).
inline Slic3r::ModelObject& add_cube(Slic3r::Model& model, Slic3r::GUI::PartPlateList& plates,
                                     const std::vector<Slic3r::Vec3d>& positions)
{
    Slic3r::ModelObject* object = model.add_object();
    object->name                = "cube";
    object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(k_cube_size, k_cube_size, k_cube_size)));
    object->center_around_origin(false);
    for (const Slic3r::Vec3d& position : positions)
        object->add_instance()->set_offset(position);
    plates.notify_object_added(int(model.objects.size()) - 1);
    return *object;
}

} // namespace plate_list_fixtures
