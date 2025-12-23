# OrcaMCP Architecture Overview

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              AI Assistant                                    │
│                         (Claude Code, Claude Desktop)                        │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ MCP Protocol (JSON-RPC 2.0 over stdio)
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           orcamcp-bridge.py                                  │
│                                                                              │
│  • Reads JSON-RPC requests from stdin                                        │
│  • Forwards to OrcaSlicer HTTP server                                        │
│  • Writes responses to stdout                                                │
│  • Handles connection errors gracefully                                      │
│                                                                              │
│  Config: ORCAMCP_HOST, ORCAMCP_PORT, ORCAMCP_TIMEOUT, ORCAMCP_DEBUG         │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ HTTP POST to /mcp (JSON-RPC 2.0)
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         OrcaSlicer Application                               │
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                    HTTP Server (Port 13618)                          │    │
│  │                                                                      │    │
│  │  Routes:                                                             │    │
│  │  • /mcp          → OrcaMCPServer (MCP protocol)                     │    │
│  │  • /api/*        → BBL authentication (existing)                    │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                    │                                         │
│                                    ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                      OrcaMCPServer                                   │    │
│  │                                                                      │    │
│  │  • Static class (no instantiation)                                  │    │
│  │  • 44 registered MCP tools                                          │    │
│  │  • JSON-RPC 2.0 request/response handling                          │    │
│  │  • Main thread dispatch for GUI operations                          │    │
│  │                                                                      │    │
│  │  Methods: initialize, tools/list, tools/call                        │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                    │                                         │
│           ┌────────────────────────┼────────────────────────┐               │
│           ▼                        ▼                        ▼               │
│  ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐         │
│  │OrcaMCPPlateUtils│    │OrcaMCPPreset    │    │  Tool Handlers  │         │
│  │                 │    │ConfigUtils      │    │                 │         │
│  │• RenderPlateView│    │                 │    │• handle_load_   │         │
│  │• Turntable      │    │• PresetToJson   │    │  model          │         │
│  │  Preview        │    │• GetPresetsJson │    │• handle_slice_  │         │
│  │• Cleanup        │    │• SelectPreset   │    │  all            │         │
│  │  Previews       │    │• ApplyConfig    │    │• handle_move_   │         │
│  └─────────────────┘    └─────────────────┘    │  object         │         │
│                                                 │• ... (44 total) │         │
│                                                 └─────────────────┘         │
│                                    │                                         │
│                                    ▼                                         │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                     OrcaSlicer Core                                  │    │
│  │                                                                      │    │
│  │  ┌───────────┐  ┌───────────┐  ┌───────────┐  ┌───────────┐        │    │
│  │  │  Plater   │  │   Model   │  │  Preset   │  │  Slicer   │        │    │
│  │  │           │  │           │  │  Bundle   │  │  Engine   │        │    │
│  │  │• Objects  │  │• Geometry │  │           │  │           │        │    │
│  │  │• Plates   │  │• Volumes  │  │• Printer  │  │• G-code   │        │    │
│  │  │• Canvas3D │  │• Config   │  │• Filament │  │• Toolpath │        │    │
│  │  │• Sidebar  │  │• Instances│  │• Print    │  │• Estimates│        │    │
│  │  └───────────┘  └───────────┘  └───────────┘  └───────────┘        │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Component Responsibilities

### orcamcp-bridge.py (137 lines)
**Purpose**: Translate MCP stdio transport to HTTP

| Function | Description |
|----------|-------------|
| `main()` | Read stdin, process requests, write to stdout |
| `send_request()` | Forward JSON-RPC to HTTP endpoint |
| `make_error_response()` | Format JSON-RPC 2.0 error responses |

### OrcaMCPServer (3,698 lines)
**Purpose**: MCP protocol handler and tool registry

| Method | Description |
|--------|-------------|
| `init()` | Initialize server, register tools, cleanup previews |
| `handle_request()` | Route HTTP requests to handlers |
| `handle_initialize()` | MCP handshake |
| `handle_tools_list()` | Return registered tools with schemas |
| `handle_tools_call()` | Execute tool and format response |
| `register_tool()` | Add tool to registry with schema |
| `run_on_main_thread()` | Execute function on GUI thread |

### OrcaMCPPlateUtils (741 lines)
**Purpose**: Visualization and rendering

| Function | Description |
|----------|-------------|
| `RenderPlateView()` | Capture plate image from camera angles |
| `CaptureTurntablePreview()` | Generate 360° preview images |
| `RenderThumbnail()` | Internal OpenGL rendering |
| `CleanupPreviews()` | Remove old preview files |

### OrcaMCPPresetConfigUtils (184 lines)
**Purpose**: Preset and configuration management

| Function | Description |
|----------|-------------|
| `PresetToJson()` | Convert Preset object to JSON |
| `GetPresetsJson()` | List presets by type |
| `GetEditedPresetJson()` | Current preset with dirty options |
| `SelectPreset()` | Switch active preset |
| `ApplyConfig()` | Modify configuration values |

## Data Flow

### Request Flow
```
1. Claude Code sends MCP request to bridge stdin
2. Bridge reads JSON-RPC request
3. Bridge POSTs to http://localhost:13618/mcp
4. HTTP server routes to OrcaMCPServer::handle_request()
5. Server parses JSON-RPC, dispatches to tools/call
6. Tool handler executes via run_on_main_thread()
7. Result JSON wrapped in MCP response format
8. HTTP response returned to bridge
9. Bridge writes to stdout
10. Claude Code receives response
```

### Threading Flow
```
HTTP Worker Thread              Main Thread (GUI)
       │                              │
       │  ──── CallAfter() ────────►  │
       │                              │
       │  (blocked on future.get())   │  ◄── Tool executes
       │                              │
       │  ◄── promise.set_value() ──  │
       │                              │
       │  (returns response)          │
```

## Key Design Decisions

| Decision | Rationale | ADR |
|----------|-----------|-----|
| HTTP + stdio bridge | OrcaSlicer is GUI app without stdio | [0001](../adr/0001-http-transport.md) |
| Embedded server | Direct access to OrcaSlicer internals | [0002](../adr/0002-embedded-server.md) |
| JSON-RPC 2.0 | MCP protocol requirement | [0003](../adr/0003-json-rpc-protocol.md) |
| Main thread dispatch | wxWidgets/OpenGL thread safety | [0004](../adr/0004-main-thread-execution.md) |

## File Locations

```
src/slic3r/GUI/OrcaMCP/
├── OrcaMCPServer.hpp              # Class definition
├── OrcaMCPServer.cpp              # 44 tool implementations
├── OrcaMCPPlateUtils.hpp          # Rendering utilities header
├── OrcaMCPPlateUtils.cpp          # Rendering implementations
├── OrcaMCPPresetConfigUtils.hpp   # Config utilities header
└── OrcaMCPPresetConfigUtils.cpp   # Config implementations

scripts/
└── orcamcp-bridge.py              # stdio-to-HTTP bridge

src/slic3r/GUI/
├── GUI_App.cpp                    # MCP route registration (~line 5440)
├── HttpServer.hpp                 # HTTP server infrastructure
└── HttpServer.cpp                 # Request handling
```
