#include "OrcaMCPGeometryImport.hpp"

#include <optional>
#include <string>

#include "libslic3r/Format/ImportNames.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

std::optional<GeometryImportRequest> g_current_request;

const char* const k_load_as_values    = "assembly, single_object, separate_objects";
const char* const k_name_cleanup_values = "remove, replace";

std::string field_error(const std::string& field, const std::string& what)
{
    return "import_options." + field + " " + what;
}

bool is_one_of(const std::string& value, std::initializer_list<const char*> allowed)
{
    for (const char* candidate : allowed)
        if (value == candidate)
            return true;
    return false;
}

// Reads an optional boolean field; returns a refusal when it is present and not a boolean.
std::string read_bool(const nlohmann::json& object, const char* key, std::optional<bool>& out)
{
    auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return {};
    if (!it->is_boolean())
        return field_error(key, "must be true or false");
    out = it->get<bool>();
    return {};
}

std::string read_string(const nlohmann::json& object, const char* key, std::optional<std::string>& out)
{
    auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return {};
    if (!it->is_string())
        return field_error(key, "must be a string");
    out = it->get<std::string>();
    return {};
}

std::string check_values(const GeometryImportRequest& request)
{
    if (request.load_as && !is_one_of(*request.load_as, {"assembly", "single_object", "separate_objects"}))
        return field_error("load_as", std::string("must be one of: ") + k_load_as_values);
    if (request.name_cleanup && !is_one_of(*request.name_cleanup, {"remove", "replace"}))
        return field_error("name_cleanup", std::string("must be one of: ") + k_name_cleanup_values);
    if (request.replacement && request.name_cleanup.value_or("") != "replace")
        return "import_options.replacement is the text that replaces each unsupported character, so it needs "
               "name_cleanup: \"replace\"";
    if (request.replacement && !valid_import_name_replacement(*request.replacement))
        return field_error("replacement", "contains a character names cannot hold; send a plain character such as \"_\" or \"-\"");
    return {};
}

} // namespace

GeometryImportRead read_geometry_import_request(const nlohmann::json& params)
{
    GeometryImportRead read;
    auto               it = params.find("import_options");
    if (it == params.end() || it->is_null())
        return read;
    if (!it->is_object()) {
        read.refusal = "import_options must be an object";
        return read;
    }
    GeometryImportRequest request;
    for (const std::string& error : {read_bool(*it, "keep_names", request.keep_names), read_bool(*it, "sort_names", request.sort_names),
                                     read_string(*it, "load_as", request.load_as), read_string(*it, "name_cleanup", request.name_cleanup),
                                     read_string(*it, "replacement", request.replacement)})
        if (!error.empty()) {
            read.refusal = error;
            return read;
        }
    read.refusal = check_values(request);
    if (read.refusal.empty())
        read.request = request;
    return read;
}

std::optional<std::string> geometry_import_refusal(const GeometryImportRequest& request, const std::string& format, bool cad_3mf)
{
    if (format.empty())
        return std::string("import_options applies only to STEP, AMF and CAD 3MF files: this file's import asks nothing");
    if (format == "3mf" && !cad_3mf)
        return std::string("import_options does not apply to a 3MF project: it has no import choices (only a CAD 3MF, "
                           "without project settings, does)");
    if (format == "3mf" && request.load_as)
        return std::string("import_options.load_as does not apply to a CAD 3MF: its import offers keep_names, sort_names "
                           "and the name clean-up only (load_as is for STEP and AMF)");
    if (format == "step" && request.load_as && *request.load_as == "separate_objects")
        return std::string("import_options.load_as: \"separate_objects\" is for AMF only; a STEP takes assembly or "
                           "single_object (assembly, single_object). Use multipart: \"separate\" to keep objects at "
                           "different heights apart.");
    return std::nullopt;
}

void apply_geometry_import_request(const GeometryImportRequest& request, const std::string& format, GeometryImportOptions& options)
{
    if (request.keep_names)
        options.keep_names = *request.keep_names;
    if (request.sort_names)
        options.sort_names = *request.sort_names;
    if (request.load_as && (format == "step" || format == "amf")) {
        options.separate_objects  = *request.load_as == "separate_objects";
        options.load_as_assembly = *request.load_as == "assembly";
    }
    options.show_dialog = false;
    options.remember    = false;
    options.dont_ask    = false;
}

void apply_geometry_import_request(const GeometryImportRequest& request, GeometryImportNamePolicy& policy)
{
    if (request.name_cleanup) {
        policy.remove = *request.name_cleanup == "remove";
        if (!policy.remove && request.replacement)
            policy.replacement = *request.replacement;
    }
    policy.show_dialog = false;
    policy.remember    = false;
    policy.dont_ask    = false;
}

ScopedGeometryImportRequest::ScopedGeometryImportRequest(const std::optional<GeometryImportRequest>& request)
{
    g_current_request = request;
}

ScopedGeometryImportRequest::~ScopedGeometryImportRequest() { g_current_request.reset(); }

MultipartChoice choose_multipart(const std::optional<std::string>& multipart, const std::optional<GeometryImportRequest>& request)
{
    MultipartChoice choice;
    const bool      separate_objects = request && request->load_as && *request->load_as == "separate_objects";
    if (multipart && *multipart != "merge" && *multipart != "separate") {
        choice.refusal = "multipart must be one of: merge, separate";
        return choice;
    }
    if (separate_objects && multipart && *multipart == "merge") {
        choice.refusal = "import_options.load_as: \"separate_objects\" and multipart: \"merge\" ask for opposite things "
                         "(separate objects, and one object with parts); leave multipart out or send \"separate\".";
        return choice;
    }
    choice.answer = multipart ? *multipart : (separate_objects ? "separate" : "merge");
    if (!multipart && separate_objects)
        choice.note = "The multi-part question was answered separate, as import_options.load_as: separate_objects asks.";
    return choice;
}

const GeometryImportRequest* current_geometry_import_request()
{
    return g_current_request ? &*g_current_request : nullptr;
}

}}} // namespace Slic3r::GUI::OrcaMCP
