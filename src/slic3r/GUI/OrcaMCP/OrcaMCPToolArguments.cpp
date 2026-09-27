#include "OrcaMCPToolArguments.hpp"

#include <algorithm>
#include <vector>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

using json = nlohmann::json;

// `schema`'s member `key` when it is of the kind wanted, else an empty one of that kind.
const json& member(const json& schema, const char* key, json::value_t kind)
{
    static const json no_object = json::object();
    static const json no_array  = json::array();
    const auto        it        = schema.find(key);
    if (it != schema.end() && it->type() == kind)
        return *it;
    return kind == json::value_t::object ? no_object : no_array;
}

const json& properties_of(const json& schema) { return member(schema, "properties", json::value_t::object); }
const json& required_of(const json& schema) { return member(schema, "required", json::value_t::array); }

// additionalProperties: false, and nothing else, forbids a key the schema does not declare. A schema
// in its place (remap_paint's mapping) says what any key's value is, so it takes any key.
bool forbids_undeclared(const json& schema)
{
    const auto it = schema.find("additionalProperties");
    return it != schema.end() && it->is_boolean() && !it->get<bool>();
}

bool describes_object(const json& schema)
{
    return schema.contains("properties") || schema.contains("required") || forbids_undeclared(schema);
}

std::string joined(const std::vector<std::string>& names, const char* separator, bool quoted)
{
    std::string out;
    for (const std::string& name : names)
        out += (out.empty() ? "" : separator) + (quoted ? "\"" + name + "\"" : name);
    return out;
}

// What an object takes: its required names first, in their order, then the others as declared.
std::vector<std::string> taken_names(const json& schema)
{
    std::vector<std::string> names;
    for (const json& name : required_of(schema))
        if (name.is_string())
            names.push_back(name.get<std::string>());
    for (const auto& [name, property] : properties_of(schema).items())
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    return names;
}

// One call's walk: the tool it is for, and every problem found so far.
struct Walk
{
    const std::string&       tool;
    std::vector<std::string> problems;
};

// "<where> has no argument "x" and is missing its required argument "y". Its arguments: ...". The
// arguments themselves are the tool's; a nested object's are its keys, and its path says which.
std::string problem(const Walk& walk, const std::string& path, const json& schema, const std::vector<std::string>& unknown,
                    const std::vector<std::string>& missing)
{
    const std::string noun  = path.empty() ? "argument" : "key";
    const auto        count = [&noun](size_t n) { return n == 1 ? noun : noun + "s"; };
    std::vector<std::string> parts;
    if (!unknown.empty())
        parts.push_back("has no " + count(unknown.size()) + " " + joined(unknown, ", ", true));
    if (!missing.empty())
        parts.push_back("is missing its required " + count(missing.size()) + " " + joined(missing, ", ", true));

    const std::vector<std::string> taken = taken_names(schema);
    const std::string              where = path.empty() ? walk.tool : walk.tool + ": " + path;
    return where + " " + joined(parts, " and ", false) + ". " +
           (taken.empty() ? "It takes no " + noun + "s." : "Its " + noun + "s: " + joined(taken, ", ", false) + ".");
}

void check_value(Walk& walk, const json& schema, const json& value, const std::string& path);

// `value`, an object, against its object schema; `path` is empty for the call's arguments themselves.
void check_object(Walk& walk, const json& schema, const json& value, const std::string& path)
{
    const json&              properties = properties_of(schema);
    std::vector<std::string> unknown;
    std::vector<std::string> missing;
    if (forbids_undeclared(schema))
        for (const auto& [key, given] : value.items())
            if (!properties.contains(key))
                unknown.push_back(key);
    for (const json& key : required_of(schema))
        if (key.is_string() && !value.contains(key.get<std::string>()))
            missing.push_back(key.get<std::string>());
    if (!unknown.empty() || !missing.empty())
        walk.problems.push_back(problem(walk, path, schema, unknown, missing));

    for (const auto& [key, property] : properties.items())
        if (const auto given = value.find(key); given != value.end())
            check_value(walk, property, *given, path.empty() ? key : path + "." + key);
}

// A nested value is looked into only when it is what its schema describes -- an object against its
// properties, an array against its items -- and its type is not checked. Every step down takes a
// step down the schema too, so how deep the walk goes is the schema's to say, never the value's.
void check_value(Walk& walk, const json& schema, const json& value, const std::string& path)
{
    if (!schema.is_object())
        return;
    if (value.is_object() && describes_object(schema)) {
        check_object(walk, schema, value, path);
    } else if (value.is_array()) {
        const auto items = schema.find("items");
        if (items == schema.end() || !items->is_object())
            return;
        for (size_t i = 0; i < value.size(); ++i)
            check_value(walk, *items, value[i], path + "[" + std::to_string(i) + "]");
    }
}

} // namespace

std::optional<std::string> tool_arguments_error(const std::string& tool, const nlohmann::json& schema,
                                                const nlohmann::json& arguments)
{
    if (!arguments.is_object())
        return tool + "'s arguments must be a JSON object of named arguments; got " + arguments.type_name() + ".";
    Walk walk{tool, {}};
    check_object(walk, schema, arguments, "");
    if (walk.problems.empty())
        return std::nullopt;
    return joined(walk.problems, " ", false);
}

}}} // namespace Slic3r::GUI::OrcaMCP
