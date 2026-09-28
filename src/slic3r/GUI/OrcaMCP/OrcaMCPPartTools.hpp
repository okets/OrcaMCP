// src/slic3r/GUI/OrcaMCP/OrcaMCPPartTools.hpp
#pragma once

#include <nlohmann/json.hpp>

// The volume forms of the object tools in OrcaMCPServer.cpp -- move_object, rotate_object,
// scale_object, mirror_object, delete_object and rename_object with volume_id -- and the object list's
// delete and rename they share with the whole-object forms. The part tools themselves (split_object,
// add_volume, set_volume_type, assemble_objects, merge_parts, invalidate_cut_info) are registered in
// OrcaMCPPartTools.cpp (OrcaMCPServer::register_part_tools). The decisions are OrcaMCPPartEdits.hpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

enum class VolumeTransformKind { move, rotate, scale, mirror };

// move_object, rotate_object, scale_object or mirror_object with volume_id: that volume changed in
// plate axes on instance 0, about its own centre, then the object dropped as the GUI drops it after a
// part moved. Called on the HTTP thread; it runs on the main thread.
nlohmann::json transform_volume(const nlohmann::json& params, VolumeTransformKind kind);

// delete_object: the object list's Delete of the object -- or, with volume_id, of that volume -- under
// one undo step. HTTP thread.
nlohmann::json delete_in_object_list(const nlohmann::json& params);

// rename_object: the object list's rename of the object -- or, with volume_id, of that volume. HTTP thread.
nlohmann::json rename_in_object_list(const nlohmann::json& params);

}}} // namespace Slic3r::GUI::OrcaMCP
