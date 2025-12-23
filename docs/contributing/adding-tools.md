# Adding New MCP Tools to OrcaMCP

This guide explains how to add new tools to the OrcaMCP server.

## Overview

Adding a new tool involves:
1. Registering the tool with its schema
2. Implementing the handler function
3. Testing the tool
4. Documenting the tool

## Step 1: Register the Tool

In `OrcaMCPServer.cpp`, find `register_builtin_tools()` and add your tool:

```cpp
void OrcaMCPServer::register_builtin_tools()
{
    // ... existing tools ...

    // Add your new tool
    register_tool({
        "my_new_tool",                        // Tool name
        "Description of what the tool does",  // Description for tools/list
        {                                     // JSON Schema for parameters
            {"type", "object"},
            {"properties", {
                {"param1", {
                    {"type", "string"},
                    {"description", "What param1 does"}
                }},
                {"param2", {
                    {"type", "integer"},
                    {"description", "What param2 does"},
                    {"default", 10}
                }},
                {"optional_param", {
                    {"type", "boolean"},
                    {"description", "Optional flag"}
                }}
            }},
            {"required", {"param1"}}          // List required parameters
        },
        [this](const nlohmann::json& params) -> nlohmann::json {
            return handle_my_new_tool(params);
        }
    });
}
```

## Step 2: Implement the Handler

Add your handler implementation:

```cpp
nlohmann::json OrcaMCPServer::handle_my_new_tool(const nlohmann::json& params)
{
    return run_on_main_thread<nlohmann::json>([&]() {
        // 1. Get required parameters
        std::string param1 = params.value("param1", "");
        if (param1.empty()) {
            throw std::runtime_error("param1 is required");
        }

        // 2. Get optional parameters with defaults
        int param2 = params.value("param2", 10);
        bool optional_param = params.value("optional_param", false);

        // 3. Get OrcaSlicer components
        Plater* plater = wxGetApp().plater();
        if (!plater) {
            throw std::runtime_error("No plater available");
        }

        // 4. Perform the operation
        // ... your implementation here ...

        // 5. Return result
        return nlohmann::json{
            {"status", "success"},
            {"result", "whatever you want to return"}
        };
    });
}
```

## Step 3: Add Header Declaration (if separate method)

If implementing as a separate method, add to `OrcaMCPServer.hpp`:

```cpp
class OrcaMCPServer {
private:
    // ... existing methods ...
    static nlohmann::json handle_my_new_tool(const nlohmann::json& params);
};
```

## Important Patterns

### Always Use run_on_main_thread

All handlers that access GUI must use `run_on_main_thread`:

```cpp
// CORRECT
return run_on_main_thread<nlohmann::json>([&]() {
    auto* plater = wxGetApp().plater();
    // Safe to use plater here
});

// WRONG - Will crash or cause race conditions
auto* plater = wxGetApp().plater();  // Called on HTTP thread!
```

### Parameter Validation

Validate early and throw clear errors:

```cpp
// Check required parameters
if (!params.contains("object_id")) {
    throw std::runtime_error("object_id is required");
}

int object_id = params["object_id"].get<int>();

// Validate range
if (object_id < 0 || object_id >= (int)model.objects.size()) {
    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id));
}
```

### Accessing OrcaSlicer Components

Common components you'll need:

```cpp
// Get the Plater (main build plate UI)
Plater* plater = wxGetApp().plater();

// Get the Model (all objects and data)
Model& model = plater->model();

// Get specific object
ModelObject* obj = model.objects[object_id];

// Get the PartPlate list (multi-plate support)
PartPlateList& plates = plater->get_partplate_list();

// Get current plate
PartPlate* current = plates.get_curr_plate();
int plate_idx = plates.get_curr_plate_index();

// Get PresetBundle (printer/filament/print settings)
PresetBundle& presets = *wxGetApp().preset_bundle;
```

### Adding Preview Support

If your tool modifies the scene, consider adding preview support:

```cpp
nlohmann::json handle_my_tool(const nlohmann::json& params) {
    return run_on_main_thread<nlohmann::json>([&]() {
        bool include_preview = params.value("include_preview", false);

        // ... do the operation ...

        nlohmann::json result = {{"status", "success"}};

        // Add preview if requested
        add_turntable_preview_if_requested(result, include_preview);

        return result;
    });
}
```

### Returning Object State

For transforms and modifications, return the new object state:

```cpp
// Get object state after modification
const Transform3d& trafo = obj->instances[0]->get_transformation().get_matrix();
Vec3d pos = trafo.translation();
Vec3d rot = trafo.rotation() * (180.0 / M_PI);  // Convert to degrees
Vec3d scale = obj->instances[0]->get_scaling_factor();

result["position"] = {pos.x(), pos.y(), pos.z()};
result["rotation_degrees"] = {rot.x(), rot.y(), rot.z()};
result["scale"] = {scale.x(), scale.y(), scale.z()};
```

## Testing Your Tool

### Manual Testing with curl

```bash
# Test your new tool
curl -s -X POST http://localhost:13618/mcp \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/call",
    "params": {
      "name": "my_new_tool",
      "arguments": {"param1": "test_value"}
    }
  }' | jq .
```

### Test via Claude Code

Add your tool to `.mcp.json` and test with natural language:
```
"Use my_new_tool with param1='test'"
```

### Edge Cases to Test

- Missing required parameters
- Invalid parameter types
- Invalid object IDs
- Empty model (no objects loaded)
- Multiple plates
- Large models (performance)

## Documentation

After implementing, update:

1. **docs/tools/reference.md** - Add tool to the reference
2. **CLAUDE.md** - Add to tools table if significant
3. **docs/tools/workflows.md** - Add workflow examples if applicable

## Code Style

Follow these conventions:

- **Snake_case** for function names: `handle_my_new_tool`
- **Clear error messages**: Include what was expected vs received
- **JSON keys**: Use snake_case to match existing tools
- **Comments**: Explain non-obvious logic

## Common Pitfalls

### 1. Invalid JSON Schema (CRITICAL)

Claude's API requires **JSON Schema draft 2020-12** compliance. Invalid schemas cause API errors like:
```
API Error: 400 "tools.X.custom.input_schema: JSON schema is invalid"
```

**Rules:**
- Every property MUST have a `type` defined
- Objects with `"type": "object"` MUST have `properties` or `additionalProperties`
- Arrays MUST have `items` defined

```cpp
// WRONG - value has no type
{"value", {}}
{"value", {{"description", "some value"}}}

// CORRECT
{"value", {{"type", "string"}, {"description", "some value"}}}

// WRONG - object without properties
{"position", {{"type", "object"}, {"description", "x,y,z position"}}}

// CORRECT - object with explicit properties
{"position", {
    {"type", "object"},
    {"description", "x,y,z position"},
    {"properties", {
        {"x", {{"type", "number"}}},
        {"y", {{"type", "number"}}},
        {"z", {{"type", "number"}}}
    }},
    {"additionalProperties", false}
}}

// WRONG - array without items
{"object_ids", {{"type", "array"}}}

// CORRECT
{"object_ids", {
    {"type", "array"},
    {"items", {{"type", "integer"}}}
}}
```

### 2. Forgetting run_on_main_thread
Symptoms: Random crashes, inconsistent behavior
Solution: Wrap ALL GUI operations

### 2. Capturing by reference in lambdas
```cpp
// CAREFUL with reference captures - params must outlive the lambda
return run_on_main_thread<nlohmann::json>([&params]() {
    // OK if run_on_main_thread blocks
});
```

### 3. Not handling empty model
```cpp
Model& model = plater->model();
if (model.objects.empty()) {
    return {{"error", "No objects in scene"}};
}
```

### 4. Not refreshing UI after changes
```cpp
// After modifying model, update the display
plater->update();
// or for more significant changes:
plater->changed_objects({object_id});
```

## Example: Complete Tool Implementation

Here's a complete example of a simple tool:

```cpp
// In register_builtin_tools():
register_tool({
    "get_object_volume",
    "Calculate the volume of an object in cubic millimeters",
    {
        {"type", "object"},
        {"properties", {
            {"object_id", {
                {"type", "integer"},
                {"description", "Index of the object (0-based)"}
            }}
        }},
        {"required", {"object_id"}}
    },
    [](const nlohmann::json& params) -> nlohmann::json {
        return run_on_main_thread<nlohmann::json>([&]() {
            // Validate
            if (!params.contains("object_id")) {
                throw std::runtime_error("object_id is required");
            }

            int object_id = params["object_id"].get<int>();

            // Get plater and model
            Plater* plater = wxGetApp().plater();
            if (!plater) {
                throw std::runtime_error("No plater available");
            }

            Model& model = plater->model();
            if (object_id < 0 || object_id >= (int)model.objects.size()) {
                throw std::runtime_error("Invalid object_id");
            }

            // Calculate volume
            ModelObject* obj = model.objects[object_id];
            double volume = 0.0;
            for (const ModelVolume* vol : obj->volumes) {
                if (vol->is_model_part()) {
                    volume += vol->get_volume();
                }
            }

            return nlohmann::json{
                {"status", "success"},
                {"object_id", object_id},
                {"volume_mm3", volume},
                {"volume_cm3", volume / 1000.0}
            };
        });
    }
});
```
