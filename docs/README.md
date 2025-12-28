# OrcaMCP Documentation

Welcome to the OrcaMCP documentation. This folder contains detailed documentation for developers and contributors.

## Quick Navigation

| Document | Description |
|----------|-------------|
| [CLAUDE.md](../CLAUDE.md) | **Start here** - AI agent quick reference guide |
| [Architecture](architecture/) | System design and technical decisions |
| [ADRs](adr/) | Architecture Decision Records |
| [Tools Reference](tools/) | Complete API reference for all 48 MCP tools |
| [Contributing](contributing/) | How to contribute to OrcaMCP |
| [Setup](setup/) | Building, configuration, and troubleshooting |
| [Release](release/) | Update process and release workflows |

## For AI Coding Agents

Start with [`CLAUDE.md`](../CLAUDE.md) in the project root. It provides:
- Project vision and architecture overview
- Quick start commands
- Key files reference
- Build commands
- How to add new tools

This `docs/` folder contains detailed documentation for deeper exploration.

## Documentation Structure

```
docs/
├── architecture/
│   ├── overview.md          # System architecture diagram & explanation
│   ├── threading-model.md   # GUI thread requirements
│   └── transport-layer.md   # HTTP + stdio bridge design
├── adr/
│   ├── README.md            # ADR index and template
│   └── 0001-*.md            # Individual decision records
├── tools/
│   ├── reference.md         # All 48 tools with parameters
│   └── workflows.md         # Common multi-tool patterns
├── contributing/
│   ├── adding-tools.md      # Guide for new MCP tools
│   └── code-style.md        # C++ and Python conventions
├── setup/
│   ├── building.md          # Cross-platform build instructions
│   ├── configuration.md     # .mcp.json, environment variables
│   └── troubleshooting.md   # Common issues and solutions
└── release/
    └── update-process.md    # Release workflow and update mechanism
```

## Key Concepts

### MCP (Model Context Protocol)
OrcaMCP implements the [Model Context Protocol](https://modelcontextprotocol.io/) to enable AI assistants to control OrcaSlicer through natural language.

### Architecture Summary
```
Claude Code CLI  ←→  orcamcp-bridge.py  ←→  OrcaSlicer HTTP Server (port 13618)
    (stdio)              (Python)                    (C++)
```

### The 48 Tools
OrcaMCP provides 48 tools across these categories:
- **Scene**: Project management (new, load, save, export)
- **Models**: Import and manipulation
- **Transforms**: Move, rotate, scale, mirror, cut
- **Plates**: Multi-plate management
- **Config**: Presets and settings
- **Slicing**: Generate G-code
- **Visualization**: Render plate views
- **Printers**: Send to physical printers
