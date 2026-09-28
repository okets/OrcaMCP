#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPMeshHealth.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPNextSteps.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPToolArguments.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "mcp_tool_references.hpp"
#include "mesh_fixtures.hpp"

#include <set>
#include <string>
#include <vector>

// What a response suggests calling next (`next_steps`), built from what it found. An agent follows a
// step by its tool name and arguments, so every step must name a real tool with arguments its schema
// takes; the rest is which findings imply which step. None of this needs the app.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using namespace mesh_fixtures;
using Slic3r::GUI::OrcaMCPServer;
using json = nlohmann::json;

namespace {

bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A model of one object per mesh, named "object <i>", each with one instance.
struct Scene
{
    Model model;

    explicit Scene(std::vector<indexed_triangle_set> meshes)
    {
        for (size_t i = 0; i < meshes.size(); ++i) {
            ModelObject* object = model.add_object();
            object->name        = "object " + std::to_string(i);
            object->add_volume(TriangleMesh(std::move(meshes[i])));
            object->add_instance();
        }
    }

    std::vector<NextStep> steps(const std::vector<int>& indices) const
    {
        return mesh_next_steps(model, indices, model_mesh_health(model));
    }
    std::vector<NextStep> steps() const { return mesh_next_steps(model, model_mesh_health(model)); }
};

indexed_triangle_set clean_cube() { return its_make_cube(10.0, 10.0, 10.0); }

const NextStep* step_for(const std::vector<NextStep>& steps, const std::string& tool)
{
    for (const NextStep& step : steps)
        if (step.tool == tool)
            return &step;
    return nullptr;
}

// Why a step would send an agent nowhere: its tool is not one, or the tool's schema refuses its
// arguments. Nothing when it is a call an agent can make as it stands.
std::optional<std::string> unusable(const NextStep& step)
{
    const auto& tools = OrcaMCPServer::registered_tools();
    const auto  tool  = tools.find(step.tool);
    if (tool == tools.end())
        return step.tool + " is not a tool";
    const json schema = OrcaMCPServer::tool_list_entry(tool->second).at("inputSchema");
    return tool_arguments_error(step.tool, schema, step.arguments.is_null() ? json::object() : step.arguments);
}

} // namespace

TEST_CASE("A response with no next step gets no next_steps field", "[McpNextSteps][orcamcp]")
{
    json response = {{"status", "success"}};
    add_next_steps(response, {});
    CHECK_FALSE(response.contains("next_steps"));
}

TEST_CASE("A next step carries its tool and why, and arguments only when it has some", "[McpNextSteps][orcamcp]")
{
    json response = json::object();
    add_next_steps(response, {{"wait_for_slice", "the slice runs in the background", json()},
                              {"get_mesh_health", "object 0 has a hole", {{"object_id", 0}}}});
    REQUIRE(response.at("next_steps").size() == 2);
    CHECK(response["next_steps"][0] == json{{"tool", "wait_for_slice"}, {"why", "the slice runs in the background"}});
    CHECK(response["next_steps"][1] ==
          json{{"tool", "get_mesh_health"}, {"arguments", {{"object_id", 0}}}, {"why", "object 0 has a hole"}});
}

TEST_CASE("Ids are listed in words, and a long list is cut short", "[McpNextSteps][orcamcp]")
{
    CHECK(listed_ids({4}) == "4");
    CHECK(listed_ids({0, 3}) == "0 and 3");
    CHECK(listed_ids({0, 3, 7}) == "0, 3 and 7");
    CHECK(listed_ids({1, 2, 3, 4, 5}, 3) == "1, 2, 3 and 2 more");
}

TEST_CASE("An object with a hole is pointed at get_mesh_health, by its object_id", "[McpNextSteps][orcamcp]")
{
    const Scene scene({clean_cube(), cube_missing_facet()});
    const std::vector<NextStep> steps = scene.steps();
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].tool == "get_mesh_health");
    CHECK(steps[0].arguments == json{{"object_id", 1}});
    CHECK(mentions(steps[0].why, "object 1"));
    CHECK(mentions(steps[0].why, "warning icon"));
}

TEST_CASE("A part made of several shells is pointed at get_object_components", "[McpNextSteps][orcamcp]")
{
    // A closed stray fragment leaves no open edge, so no warning icon: this is the only pointer to it.
    const Scene scene({separate_cubes(3)});
    REQUIRE_FALSE(model_mesh_health(scene.model)[0].warning);

    const std::vector<NextStep> steps = scene.steps();
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].tool == "get_object_components");
    CHECK(steps[0].arguments == json{{"object_id", 0}});
    CHECK(mentions(steps[0].why, "3 shells"));
    CHECK(mentions(steps[0].why, "stray"));
}

TEST_CASE("A clean object needs no next step", "[McpNextSteps][orcamcp]")
{
    CHECK(Scene({clean_cube(), clean_cube()}).steps().empty());
}

TEST_CASE("Several flagged objects make one step per tool, naming every one", "[McpNextSteps][orcamcp]")
{
    const Scene scene({cube_missing_facet(), clean_cube(), cube_missing_facet(), separate_cubes(2), separate_cubes(2)});
    const std::vector<NextStep> steps = scene.steps();
    REQUIRE(steps.size() == 2);

    const NextStep* health = step_for(steps, "get_mesh_health");
    REQUIRE(health != nullptr);
    CHECK(health->arguments == json{{"object_id", 0}});
    CHECK(mentions(health->why, "objects 0 and 2"));

    const NextStep* components = step_for(steps, "get_object_components");
    REQUIRE(components != nullptr);
    CHECK(components->arguments == json{{"object_id", 3}});
    CHECK(mentions(components->why, "objects 3 and 4"));
}

TEST_CASE("Only the objects asked about are pointed at", "[McpNextSteps][orcamcp]")
{
    // load_model asks about the objects it added, not the scene's older ones.
    const Scene scene({cube_missing_facet(), clean_cube()});
    CHECK(scene.steps({1}).empty());
    CHECK(scene.steps({0}).size() == 1);
}

TEST_CASE("A modifier's shells are not a stray part", "[McpNextSteps][orcamcp]")
{
    // get_object_components lists model parts only; a modifier of two boxes is nothing to find.
    Scene        scene({clean_cube()});
    ModelVolume* modifier = scene.model.objects[0]->add_volume(TriangleMesh(separate_cubes(2)));
    modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    CHECK(step_for(scene.steps(), "get_object_components") == nullptr);
}

TEST_CASE("Every next step names a real tool, with arguments its schema accepts", "[McpNextSteps][orcamcp]")
{
    std::vector<NextStep> steps = Scene({cube_missing_facet(), separate_cubes(2)}).steps();
    REQUIRE(steps.size() == 2);

    const mcp_tool_references::ToolNames names(OrcaMCPServer::registered_tools());
    json                                 response = json::object();
    add_next_steps(response, steps);
    const auto unknown = mcp_tool_references::unknown_in_json(response, names);
    INFO("next steps name tools that do not exist:\n" << mcp_tool_references::describe(unknown));
    CHECK(unknown.empty());

    for (const NextStep& step : steps) {
        INFO("step " << next_step_json(step).dump());
        CHECK(unusable(step) == std::nullopt);
    }
}

TEST_CASE("The name check catches a next step whose tool is misspelt", "[McpNextSteps][orcamcp]")
{
    const mcp_tool_references::ToolNames names(OrcaMCPServer::registered_tools());
    json                                 response = json::object();
    add_next_steps(response, {{"get_mesh_heath", "a typo", {{"object_id", 0}}}});
    std::set<std::string> unknown;
    for (const auto& reference : mcp_tool_references::unknown_in_json(response, names))
        unknown.insert(reference.token);
    CHECK(unknown == std::set<std::string>{"get_mesh_heath"});
    CHECK(unusable({"get_mesh_health", "a wrong argument", {{"object", 0}}}).has_value());
}
