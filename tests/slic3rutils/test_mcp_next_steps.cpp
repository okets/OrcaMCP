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

TEST_CASE("get_mesh_health points an object with open edges at repair_mesh, by its object_id", "[McpNextSteps][orcamcp]")
{
    const Scene scene({clean_cube(), cube_missing_facet()});
    const ModelObject& object = *scene.model.objects[1];

    const std::vector<NextStep> steps = mesh_repair_next_steps(object, 1, object_mesh_health(object));
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].tool == "repair_mesh");
    CHECK(steps[0].arguments == json{{"object_id", 1}});
    CHECK(mentions(steps[0].why, "object 1 (\"object 1\")"));
    CHECK(mentions(steps[0].why, "3 open edges"));
    CHECK(mentions(steps[0].why, "keep_painting"));
    CHECK(unusable(steps[0]) == std::nullopt);

    // get_mesh_health's answer carries it.
    const json report = mesh_health_report(object, 1).response;
    REQUIRE(report.contains("next_steps"));
    CHECK(report["next_steps"][0]["tool"] == "repair_mesh");
}

TEST_CASE("A closed object, even one with recorded repairs, is not pointed at repair_mesh", "[McpNextSteps][orcamcp]")
{
    RepairedMeshErrors reversed;
    reversed.facets_reversed = 1;
    Model        model;
    ModelObject* recorded = model.add_object();
    recorded->add_volume(TriangleMesh(clean_cube(), reversed));
    recorded->add_instance();
    REQUIRE(object_mesh_health(*recorded).warning);

    // A repair leaves a closed one-shell mesh, and the repairs it records, as they are.
    CHECK(mesh_repair_next_steps(*recorded, 0, object_mesh_health(*recorded)).empty());
    CHECK_FALSE(mesh_health_report(*recorded, 0).response.contains("next_steps"));
    const Scene clean({clean_cube()});
    CHECK(mesh_repair_next_steps(*clean.model.objects[0], 0, object_mesh_health(*clean.model.objects[0])).empty());
}

TEST_CASE("A slice that started is waited for with wait_for_slice", "[McpNextSteps][orcamcp]")
{
    const std::vector<NextStep> steps = slice_start_next_steps({SliceStart::started, "", ""}, std::nullopt);
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].tool == "wait_for_slice");
    CHECK(steps[0].arguments.is_null());
}

TEST_CASE("A slice refused while the pipeline is busy waits it out, then slices again", "[McpNextSteps][orcamcp]")
{
    const std::vector<NextStep> busy = slice_start_next_steps({SliceStart::not_started, "busy_slicing", "the pipeline is busy"}, std::nullopt);
    REQUIRE(busy.size() == 1);
    CHECK(busy[0].tool == "wait_for_slice");
    CHECK(mentions(busy[0].why, "slice_all again"));

    // An arrange or an orient is not the slicing pipeline: wait_for_slice would not wait for it.
    const std::vector<NextStep> job = slice_start_next_steps({SliceStart::not_started, "busy_job", "an arrange runs"}, std::nullopt);
    REQUIRE(job.size() == 1);
    CHECK(job[0].tool == "get_slicing_status");
    CHECK(mentions(job[0].why, "ui_job"));
}

TEST_CASE("Plates already sliced point at a sliced plate's estimate, by its plate_index", "[McpNextSteps][orcamcp]")
{
    // With plate 0 sliced and an empty plate 1 selected, get_print_estimate without plate_index read
    // plate 1, said "run slice_all", and slice_all said already_sliced again.
    const std::vector<NextStep> steps =
        slice_start_next_steps({SliceStart::not_started, "already_sliced", "nothing to do"}, /*sliced_plate=*/0);
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].tool == "get_print_estimate");
    CHECK(steps[0].arguments == json{{"plate_index", 0}});
    // No plate with a result: nothing to estimate.
    CHECK(slice_start_next_steps({SliceStart::not_started, "already_sliced", "nothing to do"}, std::nullopt).empty());
}

TEST_CASE("A slice the app refuses suggests no tool: its message says what to fix", "[McpNextSteps][orcamcp]")
{
    for (const char* reason : {"invalid", "nothing_to_slice", "unknown"}) {
        INFO("reason " << reason);
        CHECK(slice_start_next_steps({SliceStart::not_started, reason, "why"}, 0).empty());
    }
}

TEST_CASE("An export that started is waited for, and one that did not suggests nothing", "[McpNextSteps][orcamcp]")
{
    // export_started is not a failure: the file is being written in the background.
    const std::vector<NextStep> started = export_next_steps(true);
    REQUIRE(started.size() == 1);
    CHECK(started[0].tool == "wait_for_slice");
    CHECK(mentions(started[0].why, "written"));
    CHECK(export_next_steps(false).empty());
}

TEST_CASE("A flat render of a plate its objects are not on points at get_scene_info", "[McpNextSteps][orcamcp]")
{
    for (const size_t model_volumes : {size_t(0), size_t(4)}) {
        INFO("model volumes " << model_volumes);
        const std::vector<NextStep> steps = uniform_image_next_steps(model_volumes, /*drawn=*/0, /*plate_index=*/2);
        REQUIRE(steps.size() == 1);
        CHECK(steps[0].tool == "get_scene_info");
        CHECK(mentions(steps[0].why, "plate 2"));
    }
}

TEST_CASE("A flat render that missed the plate's objects renders the plate without views", "[McpNextSteps][orcamcp]")
{
    const std::vector<NextStep> steps = uniform_image_next_steps(/*model_volumes=*/4, /*drawn=*/3, /*plate_index=*/1);
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].tool == "render_plate_view");
    CHECK(steps[0].arguments == json{{"plate_index", 1}, {"save_to_file", true}});
}

TEST_CASE("Enforcers painted while supports are off point at support where painted, for that object", "[McpNextSteps][orcamcp]")
{
    // enable_support alone, with an (auto) support_type, generates support over the whole object: more
    // than an enforcer asks. A (manual) type of the same style generates support only where painted.
    const std::vector<NextStep> off = support_paint_next_steps(/*object_id=*/3, /*support_enabled=*/false,
                                                               /*enforcers_painted=*/true, "normal(auto)");
    REQUIRE(off.size() == 1);
    CHECK(off[0].tool == "set_object_config");
    CHECK(off[0].arguments == json{{"object_id", 3},
                                   {"settings", {{{"key", "enable_support"}, {"value", "1"}},
                                                 {{"key", "support_type"}, {"value", "normal(manual)"}}}}});
    CHECK(mentions(off[0].why, "only where painted"));

    const std::vector<NextStep> tree = support_paint_next_steps(3, false, true, "tree(auto)");
    REQUIRE(tree.size() == 1);
    CHECK(tree[0].arguments.at("settings").at(1).at("value") == "tree(manual)");
    const std::vector<NextStep> manual = support_paint_next_steps(3, false, true, "tree(manual)");
    REQUIRE(manual.size() == 1);
    CHECK(manual[0].arguments.at("settings").at(1).at("value") == "tree(manual)");
}

TEST_CASE("Blockers, erased support paint, or supports already on point nowhere", "[McpNextSteps][orcamcp]")
{
    // Turning supports on for an object painted only with blockers is the opposite of what they ask.
    CHECK(support_paint_next_steps(3, /*support_enabled=*/false, /*enforcers_painted=*/false, "normal(auto)").empty());
    CHECK(support_paint_next_steps(3, /*support_enabled=*/true, /*enforcers_painted=*/true, "normal(auto)").empty());
}

TEST_CASE("A split to parts points at get_object_components, to find a fragment among the pieces", "[McpNextSteps][orcamcp]")
{
    const std::vector<NextStep> steps = split_parts_next_steps(4, 1, {1, 2, 3});
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].tool == "get_object_components");
    CHECK(steps[0].arguments == json{{"object_id", 4}});
    CHECK(mentions(steps[0].why, "volumes 1, 2 and 3"));
    CHECK(split_parts_next_steps(4, 1, {1}).empty());
}

TEST_CASE("A new primitive is pointed at move_object, and a modifier at its settings", "[McpNextSteps][orcamcp]")
{
    const std::vector<NextStep> primitive = new_volume_next_steps(2, 3, "modifier", /*beside_object=*/true);
    REQUIRE(primitive.size() == 2);
    CHECK(primitive[0].tool == "move_object");
    CHECK(primitive[0].arguments == json{{"object_id", 2}, {"volume_id", 3}});
    CHECK(primitive[1].tool == "set_object_config");
    CHECK(primitive[1].arguments == json{{"object_id", 2}, {"volume_id", 3}});
    // A part from a file stands where its file puts it; a retyped negative volume needs neither.
    CHECK(new_volume_next_steps(2, 3, "part", false).empty());
    CHECK(new_volume_next_steps(2, 3, "negative_volume", false).empty());
}

TEST_CASE("An assembly is pointed at get_object_info, to list its volumes", "[McpNextSteps][orcamcp]")
{
    const std::vector<NextStep> steps = assembled_next_steps(5);
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].tool == "get_object_info");
    CHECK(steps[0].arguments == json{{"object_id", 5}});
}

TEST_CASE("Every next step names a real tool, with arguments its schema accepts", "[McpNextSteps][orcamcp]")
{
    std::vector<NextStep> steps = Scene({cube_missing_facet(), separate_cubes(2)}).steps();
    REQUIRE(steps.size() == 2);
    for (const char* reason : {"", "busy_slicing", "busy_job", "already_sliced"})
        for (NextStep& step : slice_start_next_steps({*reason ? SliceStart::not_started : SliceStart::started, reason, ""}, 0))
            steps.push_back(std::move(step));
    for (NextStep& step : export_next_steps(true))
        steps.push_back(std::move(step));
    for (const size_t drawn : {size_t(0), size_t(2)})
        for (NextStep& step : uniform_image_next_steps(2, drawn, 0))
            steps.push_back(std::move(step));
    for (NextStep& step : support_paint_next_steps(0, false, true, "normal(auto)"))
        steps.push_back(std::move(step));
    const Scene hole({cube_missing_facet()});
    for (NextStep& step : mesh_repair_next_steps(*hole.model.objects[0], 0, object_mesh_health(*hole.model.objects[0])))
        steps.push_back(std::move(step));
    for (NextStep& step : split_parts_next_steps(0, 0, {0, 1}))
        steps.push_back(std::move(step));
    for (NextStep& step : new_volume_next_steps(0, 1, "modifier", true))
        steps.push_back(std::move(step));
    for (NextStep& step : assembled_next_steps(0))
        steps.push_back(std::move(step));
    REQUIRE(steps.size() == 15);

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
