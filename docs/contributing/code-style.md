# OrcaMCP Code Style Guide

This guide covers coding conventions for OrcaMCP contributions.

## General Principles

From CLAUDE.md:
- **Single Responsibility**: Each method has one clear purpose
- **DRY (Don't Repeat Yourself)**: Validation logic is centralized
- **Clean Code**: Methods are small, focused, and well-named

## C++ Style

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Classes | PascalCase | `OrcaMCPServer` |
| Methods | snake_case | `handle_load_model` |
| Variables | snake_case | `object_id`, `plate_index` |
| Constants | UPPER_SNAKE_CASE | `MAX_TIMEOUT` |
| Namespaces | PascalCase | `Slic3r::GUI` |
| Files | PascalCase | `OrcaMCPServer.cpp` |

### Formatting

OrcaSlicer uses `.clang-format`. Key settings:
- **Indentation**: 4 spaces (no tabs)
- **Braces**: Same line for functions and control structures
- **Line length**: 120 characters max

```cpp
// Good
void OrcaMCPServer::handle_something(const json& params) {
    if (condition) {
        do_thing();
    } else {
        do_other_thing();
    }
}

// Avoid
void OrcaMCPServer::handle_something(const json& params)
{
    if (condition)
    {
        do_thing();
    }
}
```

### Include Order

1. Corresponding header (for .cpp files)
2. C system headers
3. C++ standard library
4. Third-party libraries (boost, nlohmann, etc.)
5. Project headers

```cpp
#include "OrcaMCPServer.hpp"

#include <cstdio>

#include <string>
#include <vector>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
```

### Error Handling

Use exceptions for error conditions:

```cpp
// Good: Clear, specific error
if (!params.contains("file_path")) {
    throw std::runtime_error("file_path is required");
}

// Good: Context in error message
if (object_id >= (int)model.objects.size()) {
    throw std::runtime_error("Invalid object_id: " + std::to_string(object_id) +
                             " (only " + std::to_string(model.objects.size()) + " objects)");
}

// Avoid: Generic errors
throw std::runtime_error("Error");
```

### JSON Handling

Use nlohmann::json:

```cpp
// Creating JSON
nlohmann::json result = {
    {"status", "success"},
    {"count", 42}
};

// Reading with defaults
std::string name = params.value("name", "default");
int count = params.value("count", 1);
bool flag = params.value("flag", false);

// Check existence
if (params.contains("optional_field")) {
    // Handle optional field
}
```

### Threading

Always use `run_on_main_thread` for GUI operations:

```cpp
// Good
nlohmann::json handle_tool(const nlohmann::json& params) {
    return run_on_main_thread<nlohmann::json>([&]() {
        // All GUI work here
        return result;
    });
}

// Bad: Direct GUI access from HTTP thread
nlohmann::json handle_tool(const nlohmann::json& params) {
    Plater* plater = wxGetApp().plater();  // WRONG!
    // ...
}
```

## Python Style (Bridge Script)

### PEP 8

The bridge script follows PEP 8:
- **Indentation**: 4 spaces
- **Line length**: 88 characters (black default)
- **Naming**: snake_case for functions and variables

```python
def send_request(request_data):
    """Forward JSON-RPC request to OrcaSlicer HTTP server."""
    url = f"http://{host}:{port}/mcp"
    req = urllib.request.Request(
        url,
        data=json.dumps(request_data).encode('utf-8'),
        headers={'Content-Type': 'application/json'},
        method='POST'
    )
    # ...
```

### Docstrings

Use docstrings for functions:

```python
def make_error_response(id_value, code, message):
    """
    Create a JSON-RPC 2.0 error response.

    Args:
        id_value: The request ID (or None for notifications)
        code: JSON-RPC error code
        message: Human-readable error message

    Returns:
        dict: JSON-RPC error response
    """
    return {
        "jsonrpc": "2.0",
        "id": id_value,
        "error": {"code": code, "message": message}
    }
```

## JSON API Conventions

### Request/Response Format

Tool arguments use snake_case:

```json
{
  "name": "move_object",
  "arguments": {
    "object_id": 0,
    "x": 10.0,
    "y": 20.0,
    "relative": true,
    "include_preview": false
  }
}
```

### Response Structure

Successful responses include status:

```json
{
  "status": "success",
  "object_id": 0,
  "position": [155.0, 155.0, 0.0]
}
```

Errors include clear messages:

```json
{
  "error": "Invalid object_id: 5 (only 3 objects in scene)"
}
```

### Boolean Parameters

Use actual booleans, not strings:

```json
// Good
{"include_preview": true}

// Avoid
{"include_preview": "true"}
```

### Array Parameters

Use JSON arrays:

```json
// Good
{"camera_position": [300, -200, 150]}

// Avoid
{"camera_position": "300,-200,150"}
```

## Documentation Conventions

### Code Comments

Comment non-obvious logic:

```cpp
// Convert from OrcaSlicer's radians to degrees for API response
Vec3d rot_deg = rotation_radians * (180.0 / M_PI);

// Object positions are center points, adjust for bounding box
double center_z = bbox.size().z() / 2.0;
```

### Tool Descriptions

Write descriptions for tools/list:

```cpp
register_tool({
    "move_object",
    "Move an object by the specified offset (relative) or to an absolute position. "
    "Returns the new position, rotation, and scale of the object.",
    // ...
});
```

## Git Commit Messages

Follow conventional commit style:

```
Add render_plate_view tool for visual inspection

- Implement multi-view camera rendering
- Support save_to_file option for token efficiency
- Add resolution parameter
```

Keep messages:
- First line: imperative, under 72 chars
- Body: explain what and why (not how)
- Reference issues if applicable

## Testing Checklist

Before submitting:
- [ ] Tool works with valid parameters
- [ ] Clear error for missing required params
- [ ] Clear error for invalid param types
- [ ] Works with empty scene (no crash)
- [ ] Works with multiple objects
- [ ] Works with multiple plates
- [ ] Documentation updated
