// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPMeshHealth.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Model.hpp"

#include <string>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {

nlohmann::json error_json(const std::string& message)
{
    return {{"status", "error"}, {"message", message}};
}

// Main thread only: the object `params` names, or an error saying why there is none.
const Slic3r::ModelObject* resolve_object(const nlohmann::json& params, int& object_id, std::string& error)
{
    const Slic3r::Model& model = wxGetApp().plater()->model();
    if (!params.contains("object_id")) {
        error = "object_id is required: the 0-based object_index get_scene_info reports";
        return nullptr;
    }
    if (!parse_integer_param(params["object_id"], object_id)) {
        error = "object_id must be a whole number";
        return nullptr;
    }
    if (object_id < 0 || object_id >= int(model.objects.size())) {
        error = "Invalid object_id " + std::to_string(object_id) + ": the scene has " +
                std::to_string(model.objects.size()) + " objects";
        return nullptr;
    }
    return model.objects[std::size_t(object_id)];
}

} // namespace

void OrcaMCPServer::register_mesh_tools()
{
    register_tool({
        "get_mesh_health",
        ToolCategory::Models,
        "Mesh errors, shells, the warning icon",
        "Mesh errors behind the object list's warning icon: open edges (holes, non-manifold), "
        "repaired facets, and loose parts or stray shells, for one object and each of its volumes. "
        "Returns mesh_warning (whether the icon shows), `tooltip` (the icon's tooltip, exactly as the "
        "GUI shows it, in the app's language) and mesh_warning_reason (the sidebar's one line), with "
        "the numbers behind them: facets, shells, open_edges, manifold, repaired, errors_repaired and "
        "each recorded repair count. An object's open_edges and repairs count every volume, as the "
        "list does; its facets, shells and volume_mm3 count model parts only. Repair counts exist "
        "only for a mesh loaded from a 3MF that recorded them: an STL is repaired silently on import "
        "and records none. A part with more than one shell also gets `shell_list`, its 10 largest "
        "shells in plate millimetres (instance 0); get_object_components lists them all.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based)"}}}
            }},
            {"required", {"object_id"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            MeshHealthReport report;
            nlohmann::json   gate = run_on_main_thread([&params, &report]() -> nlohmann::json {
                int         object_id = -1;
                std::string error;
                const Slic3r::ModelObject* object = resolve_object(params, object_id, error);
                if (object == nullptr)
                    return error_json(error);
                report = mesh_health_report(*object, object_id);
                return {{"status", "success"}};
            });
            if (gate.value("status", "") != "success")
                return gate;

            // Worker thread: the flood fill over each multi-shell part's captured mesh.
            add_shell_lists(report);
            return std::move(report.response);
        }
    });
}
