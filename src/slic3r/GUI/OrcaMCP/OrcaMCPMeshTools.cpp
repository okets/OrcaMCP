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

void OrcaMCPServer::register_mesh_tools()
{
    register_tool({
        "get_mesh_health",
        ToolCategory::Models,
        "Mesh errors: holes, open edges, repairs",
        "Check an object's mesh for problems: holes and open edges (non-manifold), repaired facets, and "
        "loose parts or stray shells, for one object and each of its volumes -- the errors behind the "
        "object list's warning icon. "
        "Every row -- the object and each volume -- has mesh_warning (whether the icon shows) and, only "
        "when it is true, `tooltip` (the icon's tooltip, exactly as the GUI shows it, in the app's "
        "language; its last line is the GUI's \"click the icon\") and mesh_warning_reason (the "
        "sidebar's one line). A flagged object also gets `advice`: MCP has no repair tool, but the "
        "user can repair it with the object list's Repair, on every platform; and what slicing does with it. The numbers: facets, shells, open_edges, manifold, repaired, "
        "errors_repaired and each recorded repair count. An object's open_edges and repairs count every volume, as the "
        "list does; its facets, shells and volume_mm3 count model parts only. Repair counts exist "
        "only for a mesh loaded from a 3MF that recorded them: an STL is repaired silently on import "
        "and records none. A part with more than one shell also gets `shell_list`: the 10 shells with "
        "the most facets, each with its area and plate-millimetre bounding box (instance 0); "
        "get_object_components lists them all.",
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
                const Slic3r::ModelObject* object = resolve_object_id(params, wxGetApp().plater()->model(), object_id, error);
                if (object == nullptr)
                    return error_response(error);
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
