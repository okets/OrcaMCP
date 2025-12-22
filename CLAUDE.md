# CLAUDE.md - OrcaMCP Project

This file provides guidance to Claude Code when working with the OrcaMCP project.

---

## CRITICAL: MIGRATION TASK IN PROGRESS

**This is a fresh fork of OrcaSlicer with a pending migration task.**

The goal is to add an MCP (Model Context Protocol) server that enables Claude Code CLI to control the slicer. The MCP server code exists in a separate project (JusPrin) and needs to be migrated here with renaming.

### Source Code Location
The MCP server source code is at:
```
/Users/hanan/Projects/JusPrin/src/slic3r/GUI/JusPrin/
```

You have READ access to this folder. Use it to copy and adapt the MCP server code.

---

## Migration Checklist

### Phase 1: Copy and Rename MCP Files
- [ ] Create `src/slic3r/GUI/OrcaMCP/` directory
- [ ] Copy and rename MCP server files (see table below)
- [ ] Apply find/replace renaming in all copied files
- [ ] Remove JusPrinChatPanel dependencies

### Phase 2: Modify Core Files
- [ ] Modify `src/slic3r/GUI/HttpServer.hpp` - Add ResponseJson, RequestHandlerFn
- [ ] Modify `src/slic3r/GUI/HttpServer.cpp` - Add read_body(), JSON response impl
- [ ] Modify `src/slic3r/GUI/GUI_App.cpp` - Add MCP route handler
- [ ] Modify `src/slic3r/CMakeLists.txt` - Add OrcaMCP source files

### Phase 3: Build and Test
- [ ] Build dependencies: `./build_release_macos.sh -d`
- [ ] Build slicer: `./build_release_macos.sh -s`
- [ ] Test: `curl http://localhost:13618/mcp`
- [ ] Test with Claude Code

---

## Files to Copy

| Source (JusPrin) | Destination (OrcaMCP) |
|------------------|----------------------|
| `GUI/JusPrin/JusPrinMCPServer.hpp` | `GUI/OrcaMCP/OrcaMCPServer.hpp` |
| `GUI/JusPrin/JusPrinMCPServer.cpp` | `GUI/OrcaMCP/OrcaMCPServer.cpp` |
| `GUI/JusPrin/JusPrinPlateUtils.hpp` | `GUI/OrcaMCP/OrcaMCPPlateUtils.hpp` |
| `GUI/JusPrin/JusPrinPlateUtils.cpp` | `GUI/OrcaMCP/OrcaMCPPlateUtils.cpp` |
| `GUI/JusPrin/JusPrinPresetConfigUtils.hpp` | `GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp` |
| `GUI/JusPrin/JusPrinPresetConfigUtils.cpp` | `GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp` |
| `scripts/jusprin-mcp-bridge.py` | `scripts/orcamcp-bridge.py` |

---

## Renaming Rules

Apply these find/replace operations to ALL copied files:

| Find | Replace |
|------|---------|
| `JusPrinMCPServer` | `OrcaMCPServer` |
| `JusPrinPlateUtils` | `OrcaMCPPlateUtils` |
| `JusPrinPresetConfigUtils` | `OrcaMCPPresetConfigUtils` |
| `slic3r_JusPrinMCPServer_hpp_` | `slic3r_OrcaMCPServer_hpp_` |
| `slic3r_GUI_JusPrinPlateUtils_hpp_` | `slic3r_GUI_OrcaMCPPlateUtils_hpp_` |
| `slic3r_GUI_JusPrinPresetConfigUtils_hpp_` | `slic3r_GUI_OrcaMCPPresetConfigUtils_hpp_` |
| `#include "JusPrinPlateUtils.hpp"` | `#include "OrcaMCPPlateUtils.hpp"` |
| `#include "JusPrinPresetConfigUtils.hpp"` | `#include "OrcaMCPPresetConfigUtils.hpp"` |
| `/tmp/jusprin_` | `/tmp/orcamcp_` |
| `"jusprin"` | `"orcamcp"` |
| `"JusPrin"` | `"OrcaSlicer"` (in user-facing strings) |

---

## Lines to DELETE (JusPrinChatPanel Dependencies)

In `OrcaMCPPlateUtils.cpp` and `OrcaMCPPresetConfigUtils.cpp`, DELETE any lines containing:
```cpp
jusprinChatPanel()->SendNativeErrorOccurredEvent
```

These are calls to a chat UI that doesn't exist in OrcaMCP.

---

## HttpServer Modifications

The upstream OrcaSlicer HttpServer lacks POST body reading and JSON responses. You need to add these.

### HttpServer.hpp - Add after ResponseRedirect class:

```cpp
class ResponseJson : public Response
{
    const std::string json_str;
    int status_code;

public:
    ResponseJson(const std::string& json, int status = 200) : json_str(json), status_code(status) {}
    ~ResponseJson() override = default;
    void write_response(std::stringstream& ssOut) override;
};

// Request handler type that includes method, URL, and body
using RequestHandlerFn = std::function<std::shared_ptr<Response>(const std::string& method, const std::string& url, const std::string& body)>;
```

### HttpServer.hpp - Update declarations:

```cpp
void set_request_handler(const RequestHandlerFn& request_handler);
void set_request_handler(const std::function<std::shared_ptr<Response>(const std::string&)>& request_handler);
static std::shared_ptr<Response> bbl_auth_handle_request(const std::string& method, const std::string& url, const std::string& body);
```

### HttpServer.hpp - Add to session class:

```cpp
std::string body;
void read_body();
```

### HttpServer.cpp - Reference Implementation

Copy the following implementations from JusPrin's HttpServer.cpp:
- `ResponseJson::write_response()`
- `session::read_body()`
- Updated `session::process_request()` with body parameter
- Updated `set_request_handler()` overloads

The JusPrin HttpServer.cpp is at:
```
/Users/hanan/Projects/JusPrin/src/slic3r/GUI/HttpServer.cpp
```

---

## CMakeLists.txt Modification

**File:** `src/slic3r/CMakeLists.txt`

Find the `set(SLIC3R_GUI_SOURCES` section and add:

```cmake
    GUI/OrcaMCP/OrcaMCPServer.hpp
    GUI/OrcaMCP/OrcaMCPServer.cpp
    GUI/OrcaMCP/OrcaMCPPlateUtils.hpp
    GUI/OrcaMCP/OrcaMCPPlateUtils.cpp
    GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp
    GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp
```

---

## GUI_App.cpp Modification

**File:** `src/slic3r/GUI/GUI_App.cpp`

### Add include at top:
```cpp
#include "OrcaMCP/OrcaMCPServer.hpp"
```

### Find and update `start_http_server()` method:

```cpp
void GUI_App::start_http_server()
{
    if (!m_http_server.is_started()) {
        m_http_server.set_request_handler([](const std::string& method, const std::string& url, const std::string& body)
            -> std::shared_ptr<HttpServer::Response> {
            // Route /mcp requests to MCP server
            if (url.find("/mcp") != std::string::npos) {
                return OrcaMCPServer::handle_request(method, url, body);
            }
            // Fall back to default handler
            return HttpServer::bbl_auth_handle_request(method, url, body);
        });
        m_http_server.start();
    }
}
```

---

## Bridge Script (orcamcp-bridge.py)

Update these in the copied script:

```python
# Environment variables
ORCAMCP_HOST = os.environ.get("ORCAMCP_HOST", "localhost")
ORCAMCP_PORT = int(os.environ.get("ORCAMCP_PORT", "13618"))
ORCAMCP_URL = f"http://{ORCAMCP_HOST}:{ORCAMCP_PORT}/mcp"
TIMEOUT = int(os.environ.get("ORCAMCP_TIMEOUT", "120"))

# Debug logging
def log_debug(message: str):
    if os.environ.get("ORCAMCP_DEBUG"):
        print(f"[orcamcp-bridge] {message}", file=sys.stderr)
```

---

## .mcp.json Configuration

Create `.mcp.json` in project root:

```json
{
  "mcpServers": {
    "orcamcp": {
      "command": "python3",
      "args": ["./scripts/orcamcp-bridge.py"]
    }
  }
}
```

---

## Testing Checklist

After building, verify:

1. **App launches:** Run OrcaSlicer, check for errors
2. **HTTP endpoint:** `curl http://localhost:13618/mcp` returns server info JSON
3. **Initialize:** POST `{"jsonrpc":"2.0","method":"initialize","id":1}` works
4. **Tools list:** POST `{"jsonrpc":"2.0","method":"tools/list","id":2}` returns 44 tools
5. **get_scene_info:** Works and returns bed dimensions
6. **load_model:** Successfully loads an STL file
7. **slice_all:** Starts slicing
8. **render_plate_view:** Returns base64 image

---

## Overview

OrcaSlicer is an open-source 3D slicer application forked from Bambu Studio, built using C++ with wxWidgets for the GUI and CMake as the build system.

## Build Commands

### Building on macOS
```bash
# Build everything (dependencies and slicer)
./build_release_macos.sh

# Build only dependencies (first time)
./build_release_macos.sh -d

# Build only slicer (after deps are built)
./build_release_macos.sh -s

# Use Ninja generator for faster builds
./build_release_macos.sh -x

# Build for specific architecture
./build_release_macos.sh -a arm64    # or x86_64 or universal
```

### Building on Windows
```bash
# Build everything
build_release_vs2022.bat

# Build with debug symbols
build_release_vs2022.bat debug

# Build only dependencies
build_release_vs2022.bat deps

# Build only slicer (after deps are built)
build_release_vs2022.bat slicer
```

### Building on Linux
```bash
# First time setup - install system dependencies
./build_linux.sh -u

# Build dependencies and slicer
./build_linux.sh -dsi

# Individual options:
./build_linux.sh -d    # dependencies only
./build_linux.sh -s    # slicer only
./build_linux.sh -i    # build AppImage
```

### Build System
- Uses CMake with minimum version 3.13
- Primary build directory: `build/`
- Dependencies are built in `deps/build/`

---

## MCP Server Architecture (After Migration)

The MCP server exposes 44 tools for slicer automation:

### Tool Categories
- **Scene Management:** get_scene_info, new_project, load_project, save_project
- **Plate Management:** add_plate, select_plate, delete_plate
- **Model Operations:** load_model, auto_orient, arrange_objects
- **Transforms:** move_object, rotate_object, scale_object, mirror_object, flatten_object, clone_object, cut_object, delete_object
- **Configuration:** get_presets, select_preset, apply_config, get_valid_config_keys
- **Per-Object Settings:** get_object_config, set_object_config, reset_object_config
- **Layer Ranges:** get_object_layer_ranges, set_object_layer_range, delete_object_layer_range
- **Slicing:** slice_all, get_slicing_status, export_gcode, get_print_estimate
- **Visualization:** render_plate_view
- **Printer:** get_printers, select_printer, send_to_printer
- **Adaptive Layers:** apply_adaptive_layer_height, clear_adaptive_layer_height
- **History:** undo, redo

### Communication Flow
```
Claude Code CLI → stdio → orcamcp-bridge.py → HTTP POST → OrcaSlicer:13618/mcp → OrcaMCPServer
```

---

## Code Style Standards

- **Single Responsibility:** Each method has one clear purpose
- **DRY:** Validation logic is centralized
- **Clean Code:** Methods are small, focused, and well-named
- **C++17 standard** with selective C++20 features
- **Naming:** PascalCase for classes, snake_case for functions/variables

---

## Key Files Reference

| File | Purpose |
|------|---------|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp` | MCP server class definition |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` | 44 tool implementations (~3,700 lines) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPlateUtils.cpp` | Plate rendering, turntable previews |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp` | Preset/config management |
| `src/slic3r/GUI/HttpServer.hpp` | HTTP server with JSON responses |
| `src/slic3r/GUI/GUI_App.cpp` | MCP route registration |
| `scripts/orcamcp-bridge.py` | stdio-to-HTTP bridge |
