<div align="center">

<picture>
  <img alt="OrcaMCP logo" src="resources/images/OrcaSlicer.png" width="15%" height="15%">
</picture>

# OrcaMCP

**AI-Powered Slicing**

*Natural language control for OrcaSlicer through the Model Context Protocol (MCP)*

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)](https://github.com/okets/OrcaMCP)
[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](LICENSE.txt)
[![MCP Protocol](https://img.shields.io/badge/MCP-2024--11--05-purple)](https://modelcontextprotocol.io/)

</div>

## What is OrcaMCP?

OrcaMCP adds an MCP server to OrcaSlicer, enabling AI assistants like Claude Code to control the entire 3D printing workflow through natural language:

- **Load and manipulate 3D models** - Import STL/OBJ files, transform, arrange, cut
- **Configure print settings** - Change layer height, infill, supports via simple commands
- **Slice and export G-code** - Full slicing pipeline controlled programmatically
- **Visualize the build plate** - Render preview images for AI inspection
- **Send to printers** - Direct integration with OctoPrint/Klipper and Bambu printers

### Why OrcaMCP?

Traditional 3D printing requires manual interaction with slicer software. OrcaMCP enables:

```
You: "Load benchy.stl, orient it for minimal supports, use 0.2mm layers with 20% infill,
      and slice it. Then show me what it looks like."

Claude: [Executes load_model, auto_orient, apply_config, slice_all, render_plate_view]
        "Here's your benchy positioned for optimal printing. Estimated print time: 2h 15m"
```

## Quick Start

### 1. Launch OrcaMCP

Build from source (see [Building](docs/setup/building.md)) or use a pre-built release.

### 2. Verify MCP Server

```bash
curl -s http://localhost:13618/mcp | jq .
```

Should return:
```json
{
  "name": "orca-slicer",
  "version": "1.0.0",
  "protocol": "mcp"
}
```

### 3. Configure Claude Code

The repository includes `.mcp.json` for automatic configuration. Just open the project folder in Claude Code.

Or add to `~/.claude.json`:
```json
{
  "mcpServers": {
    "orca-slicer": {
      "command": "python3",
      "args": ["/path/to/OrcaMCP/scripts/orcamcp-bridge.py"]
    }
  }
}
```

### 4. Start Using Natural Language

```
"What tools are available?"
"Load the file ~/Downloads/model.stl"
"Show me the build plate"
"Slice it and export to ~/Desktop/output.gcode"
```

## Architecture

```
┌─────────────────┐     stdio     ┌──────────────────┐     HTTP      ┌─────────────┐
│   Claude Code   │ ◄───────────► │ orcamcp-bridge   │ ◄───────────► │  OrcaMCP    │
│      CLI        │               │    (Python)      │               │ Port 13618  │
└─────────────────┘               └──────────────────┘               └─────────────┘
```

See [Architecture Overview](docs/architecture/overview.md) for details.

## Available Tools (70)

A selection, by category. The full list with parameters is in the tools reference.

| Category | Tools |
|----------|-------|
| **Scene** | `get_scene_info`, `new_project`, `load_project`, `save_project`, `export_3mf` |
| **Models** | `load_model`, `auto_orient`, `arrange_objects`, `set_object_printable` |
| **Transforms** | `move_object`, `rotate_object`, `scale_object`, `mirror_object`, `flatten_object`, `cut_object` |
| **Config** | `get_presets`, `select_preset`, `apply_config`, `get_edited_presets` |
| **Per-Object** | `get_object_config`, `set_object_config`, `reset_object_config` |
| **Filaments & colour** | `get_filaments`, `set_object_filament`, `set_mixed_filament`, `suggest_color_mix`, `get_color_palette`, `get_flush_volumes` |
| **Slicing** | `slice_all`, `get_slicing_status`, `export_gcode`, `get_print_estimate` |
| **Visualization** | `render_plate_view`, `set_gcode_view_type` |
| **Printers** | `get_printers`, `select_printer`, `discover_printers`, `send_to_printer`, `get_printer_status`, `printer_control`, `match_project_to_printer` |
| **History** | `undo`, `redo` |

See [Tools Reference](docs/tools/reference.md) for complete documentation.

### Colour mixing

On a printer with more than one extruder, a filament slot can be a *mix* of loaded
filaments. `suggest_color_mix` takes a colour you name and works out which loaded
filaments to combine to reach it, and `get_color_palette` generates a set of mixes
you can choose from — so an assistant can propose a palette for a model rather than
leaving you to work out ratios by hand.

## Printer support

Sending a job works through OrcaSlicer's own print-host support, so anything it
handles works here. Some printers additionally have a live device console and
model-specific tooling:

| Printer | Notes |
|---------|-------|
| [FlashForge Creator 5 / 5 Pro](docs/printers/flashforge-creator-5.md) | Live console, 4-slot material station, colour-aware material mapping |


## Documentation

| Document | Description |
|----------|-------------|
| [CLAUDE.md](CLAUDE.md) | **AI Agent Quick Reference** - Start here for coding agents |
| [Architecture](docs/architecture/) | System design, threading model, transport layer |
| [ADRs](docs/adr/) | Architecture Decision Records |
| [Tools Reference](docs/tools/reference.md) | Every tool, with parameters and examples |
| [Workflows](docs/tools/workflows.md) | Common task patterns |
| [Contributing](docs/contributing/) | Adding tools, code style |
| [Setup](docs/setup/) | Building, configuration, troubleshooting |

## Example Workflows

### Basic Print Workflow
```json
load_model → arrange_objects → slice_all → get_slicing_status → export_gcode
```

### Visual Inspection
```json
render_plate_view (with save_to_file=true) → Read the image file
```

### Change Settings
```json
apply_config (with settings array) → slice_all
```

See [Workflows Guide](docs/tools/workflows.md) for more examples.

## Building from Source

### macOS
```bash
git clone https://github.com/okets/OrcaMCP.git
cd OrcaMCP
./build_release_macos.sh -s -x
```

### Linux
```bash
git clone https://github.com/okets/OrcaMCP.git
cd OrcaMCP
./build_release.sh
```

See [Building Guide](docs/setup/building.md) for detailed instructions.

## Configuration

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `ORCAMCP_HOST` | `localhost` | OrcaSlicer HTTP host |
| `ORCAMCP_PORT` | `13618` | OrcaSlicer HTTP port |
| `ORCAMCP_TIMEOUT` | `120` | Request timeout (seconds) |
| `ORCAMCP_DEBUG` | (unset) | Enable debug logging |

See [Configuration Guide](docs/setup/configuration.md) for more options.

## Project Origin

OrcaMCP is a fork of [OrcaSlicer](https://github.com/SoftFever/OrcaSlicer) with an embedded MCP server for AI integration.

## License

OrcaMCP is licensed under the GNU Affero General Public License v3.0 (AGPL-3.0), the same license as OrcaSlicer.

See [LICENSE.txt](LICENSE.txt) for details.

## Acknowledgments

- **[OrcaSlicer](https://github.com/SoftFever/OrcaSlicer)** - The excellent slicer this project is based on
- **[Anthropic](https://anthropic.com)** - Model Context Protocol specification
- **[PrusaSlicer](https://github.com/prusa3d/PrusaSlicer)** & **[BambuStudio](https://github.com/bambulab/BambuStudio)** - Upstream slicer projects

## Contributing

Contributions are welcome! See [Contributing Guide](docs/contributing/) for:
- [Adding New Tools](docs/contributing/adding-tools.md)
- [Code Style](docs/contributing/code-style.md)

## Support

- **Issues**: [GitHub Issues](https://github.com/okets/OrcaMCP/issues)
- **Documentation**: [docs/](docs/)
