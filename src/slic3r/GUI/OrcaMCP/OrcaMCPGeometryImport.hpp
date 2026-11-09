// src/slic3r/GUI/OrcaMCP/OrcaMCPGeometryImport.hpp
#pragma once
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

#include "slic3r/GUI/GeometryImportDialog.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// What a load_model call chooses for the geometry import dialogs (STEP, AMF, CAD 3MF): the choices the
// dialogs offer, each left out to keep the user's saved one. load_as is assembly, single_object or
// separate_objects; name_cleanup is remove or replace (with `replacement`) for names with characters
// the app cannot use.
struct GeometryImportRequest {
    std::optional<bool>        keep_names;
    std::optional<bool>        sort_names;
    std::optional<std::string> load_as;
    std::optional<std::string> name_cleanup;
    std::optional<std::string> replacement;
};

struct GeometryImportRead {
    std::optional<GeometryImportRequest> request; // none when the call has no import_options
    std::string                          refusal; // non-empty: the value is not accepted
};

// Reads params["import_options"]; a refusal names what to send instead.
GeometryImportRead read_geometry_import_request(const nlohmann::json& params);

// Why the request does not fit the file, or nothing. `format` is geometry_import_format()'s answer
// ("step", "amf", "3mf", "" for any other file) and `cad_3mf` whether a 3MF is a CAD one.
std::optional<std::string> geometry_import_refusal(const GeometryImportRequest& request, const std::string& format, bool cad_3mf);

// The call's choices over the saved ones; the dialog is never shown and nothing is remembered.
void apply_geometry_import_request(const GeometryImportRequest& request, const std::string& format, GeometryImportOptions& options);
void apply_geometry_import_request(const GeometryImportRequest& request, GeometryImportNamePolicy& policy);

// The request of the running load_model call (main thread only), for the plater's import hooks.
class ScopedGeometryImportRequest
{
public:
    explicit ScopedGeometryImportRequest(const std::optional<GeometryImportRequest>& request);
    ~ScopedGeometryImportRequest();
    ScopedGeometryImportRequest(const ScopedGeometryImportRequest&)            = delete;
    ScopedGeometryImportRequest& operator=(const ScopedGeometryImportRequest&) = delete;
};
const GeometryImportRequest* current_geometry_import_request();

}}} // namespace Slic3r::GUI::OrcaMCP
