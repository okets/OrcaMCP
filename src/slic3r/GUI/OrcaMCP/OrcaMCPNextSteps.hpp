// src/slic3r/GUI/OrcaMCP/OrcaMCPNextSteps.hpp
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "OrcaMCPMeshHealth.hpp"

namespace Slic3r {
class Model;
namespace GUI { namespace OrcaMCP {

// What a response suggests calling next, when what it found implies a follow-up: a loaded object with
// a stray shell, a slice that runs in the background. One shape everywhere, `next_steps`:
//
//   [{"tool": "<a tool name>", "arguments": {...}, "why": "<the facts from this response>"}]
//
// `arguments`, left out when the tool needs none, is accepted by that tool's schema. At most one step
// per tool: `arguments` names the first object it applies to and `why` names them all, so a scene of
// many flagged objects does not bury the rest of the response. The tests check every `tool` is a real
// tool and every `arguments` passes its schema (test_mcp_next_steps.cpp).
struct NextStep
{
    std::string    tool;
    std::string    why;
    nlohmann::json arguments; // null: the call needs none
};

// {tool, arguments (unless null), why}.
nlohmann::json next_step_json(const NextStep& step);

// Sets response["next_steps"] to `steps`; a response with none gets no field.
void add_next_steps(nlohmann::json& response, const std::vector<NextStep>& steps);

// "0", "0 and 3", "0, 3 and 7"; past `max_listed` ids, "... and N more".
std::string listed_ids(const std::vector<int>& ids, size_t max_listed = 10);

// The objects `object_indices` names (indices into model.objects) that need a closer look:
// get_mesh_health for one whose object-list row shows the warning icon (`health`, by object index:
// model_mesh_health), get_object_components for one with a model part made of more than one shell --
// a loose part or a stray fragment, which leaves no warning icon when it is closed. load_model asks
// about the objects it added, get_scene_info about every object.
std::vector<NextStep> mesh_next_steps(const Model& model, const std::vector<int>& object_indices, const std::vector<MeshHealth>& health);
// The same for every object of `model`.
std::vector<NextStep> mesh_next_steps(const Model& model, const std::vector<MeshHealth>& health);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
