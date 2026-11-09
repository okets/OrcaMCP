#include <catch2/catch_test_macros.hpp>

#include <string>

#include "slic3r/GUI/OrcaMCP/OrcaMCPGeometryImport.hpp"

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

namespace {
bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
GeometryImportRead read(const char* json) { return read_geometry_import_request(nlohmann::json{{"import_options", nlohmann::json::parse(json)}}); }
} // namespace

TEST_CASE("import_options reads every field", "[McpGeometryImport][orcamcp]")
{
    const auto r = read(R"({"keep_names":false,"sort_names":true,"load_as":"single_object","name_cleanup":"replace","replacement":"-"})");
    REQUIRE(r.refusal.empty());
    REQUIRE(r.request.has_value());
    CHECK(*r.request->keep_names == false);
    CHECK(*r.request->sort_names == true);
    CHECK(*r.request->load_as == "single_object");
    CHECK(*r.request->name_cleanup == "replace");
    CHECK(*r.request->replacement == "-");
    CHECK_FALSE(read_geometry_import_request(nlohmann::json::object()).request.has_value());
}

TEST_CASE("an unknown load_as is refused", "[McpGeometryImport][orcamcp]")
{
    const auto r = read(R"({"load_as":"pile"})");
    CHECK_FALSE(r.request.has_value());
    CHECK(mentions(r.refusal, "assembly, single_object, separate_objects"));
}

TEST_CASE("replacement without name_cleanup replace is refused", "[McpGeometryImport][orcamcp]")
{
    CHECK_FALSE(read(R"({"replacement":"-"})").refusal.empty());
    CHECK_FALSE(read(R"({"name_cleanup":"remove","replacement":"-"})").refusal.empty());
}

TEST_CASE("an invalid replacement is refused", "[McpGeometryImport][orcamcp]")
{
    CHECK_FALSE(read(R"({"name_cleanup":"replace","replacement":"/"})").refusal.empty());
}

TEST_CASE("import_options on an STL is refused", "[McpGeometryImport][orcamcp]")
{
    const auto refusal = geometry_import_refusal(GeometryImportRequest{}, "", false);
    REQUIRE(refusal.has_value());
    CHECK(mentions(*refusal, "STEP"));
    CHECK(mentions(*refusal, "AMF"));
    CHECK(mentions(*refusal, "CAD 3MF"));
}

TEST_CASE("load_as on a 3MF is refused", "[McpGeometryImport][orcamcp]")
{
    GeometryImportRequest request;
    request.load_as = "assembly";
    CHECK(geometry_import_refusal(request, "3mf", true).has_value());
    GeometryImportRequest names;
    names.keep_names = false;
    CHECK_FALSE(geometry_import_refusal(names, "3mf", true).has_value());
    CHECK_FALSE(geometry_import_refusal(request, "step", false).has_value());
}

TEST_CASE("a 3MF project takes no import_options", "[McpGeometryImport][orcamcp]")
{
    GeometryImportRequest request;
    request.keep_names = true;
    CHECK(geometry_import_refusal(request, "3mf", false).has_value());
}

TEST_CASE("apply sets the dialog's fields", "[McpGeometryImport][orcamcp]")
{
    GeometryImportOptions options;
    options.remember = true;
    GeometryImportRequest request;
    request.load_as = "single_object";
    request.keep_names = false;
    apply_geometry_import_request(request, "amf", options);
    CHECK_FALSE(options.separate_objects);
    CHECK_FALSE(options.load_as_assembly);
    CHECK_FALSE(options.keep_names);
    CHECK_FALSE(options.show_dialog);
    CHECK_FALSE(options.remember);

    request.load_as = "separate_objects";
    apply_geometry_import_request(request, "step", options);
    CHECK(options.separate_objects);
}

TEST_CASE("apply sets the name rule", "[McpGeometryImport][orcamcp]")
{
    GeometryImportNamePolicy policy;
    policy.remove = true;
    GeometryImportRequest request;
    request.name_cleanup = "replace";
    request.replacement  = "-";
    apply_geometry_import_request(request, policy);
    CHECK_FALSE(policy.remove);
    CHECK(policy.replacement == "-");
    CHECK_FALSE(policy.show_dialog);

    request.name_cleanup = "remove";
    request.replacement.reset();
    apply_geometry_import_request(request, policy);
    CHECK(policy.remove);
}
