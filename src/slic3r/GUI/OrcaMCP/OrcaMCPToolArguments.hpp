// src/slic3r/GUI/OrcaMCP/OrcaMCPToolArguments.hpp
#pragma once
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

// Whether a tools/call's arguments fit its tool's inputSchema, checked before the handler runs.
//
// A handler reads the arguments it knows and ignored the rest, so a misspelled or guessed one did
// nothing, and the call still reported success: scale_object {"object_id": 0, "scale": 0.5} left the
// object as it was. Every schema says additionalProperties: false (tools/list adds it where a
// registration leaves it out), but nothing held the call to it. Now tools/call refuses, with
// JSON-RPC -32602, what the schema says a call cannot be:
//
//  - arguments that are not an object;
//  - an argument the schema does not declare, and a key a nested object does not declare when that
//    object's own schema says additionalProperties: false (e.g. set_object_config's settings items);
//  - a required argument, or a nested object's required key, left out.
//
// Nothing else: no type, range or enum is checked, which is the handlers' job. A nested value is
// looked into only when it is what its schema describes -- an object against `properties`, an array
// against `items` -- so a setting list sent as its JSON text, which the handlers also read, passes as
// it is. The bridge checks its own tools, start_orca and wait_for_slice, the same way.
//
// Unit-tested in tests/slic3rutils/test_mcp_tool_arguments.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Why `arguments` does not fit `schema`, the inputSchema tools/list publishes for `tool`, in one
// message that names the tool, what is wrong and what the object takes; nothing when it fits.
std::optional<std::string> tool_arguments_error(const std::string& tool, const nlohmann::json& schema,
                                                const nlohmann::json& arguments);

}}} // namespace Slic3r::GUI::OrcaMCP
