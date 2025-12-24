# Contributing to OrcaMCP

Thank you for your interest in contributing to OrcaMCP! This guide will help you get started.

## Quick Links

- [Adding New MCP Tools](docs/contributing/adding-tools.md) - Step-by-step guide for new tools
- [Code Style Guide](docs/contributing/code-style.md) - Naming conventions and formatting
- [Building from Source](docs/setup/building.md) - Build instructions for all platforms
- [Architecture Overview](docs/architecture/overview.md) - How the system works

## Ways to Contribute

### Report Bugs

Found a bug? [Open an issue](https://github.com/okets/OrcaMCP/issues/new?template=bug_report.yml) with:
- OrcaMCP version (from `Help` -> `About`)
- Operating system and version
- Steps to reproduce
- Expected vs actual behavior
- Log files and project file (see bug template)

### Suggest Features

Have an idea? [Open a feature request](https://github.com/okets/OrcaMCP/issues/new?template=feature_request.yml) describing:
- The problem you're trying to solve
- Your proposed solution
- Alternative approaches you've considered

### Submit Code

1. **Fork the repository**
2. **Create a feature branch**: `git checkout -b feature/my-new-feature`
3. **Make your changes** following our [code style](docs/contributing/code-style.md)
4. **Test thoroughly** (see testing checklist below)
5. **Commit with clear messages**: `git commit -m "Add my-new-feature"`
6. **Push to your fork**: `git push origin feature/my-new-feature`
7. **Open a Pull Request**

## Development Setup

### Prerequisites

- C++17 compatible compiler
- CMake 3.16+
- Python 3.8+ (for bridge script)
- Platform-specific dependencies (see [building guide](docs/setup/building.md))

### Build

```bash
# macOS
./build_release_macos.sh -s -x

# Linux
./build_release.sh

# Windows
# See docs/setup/building.md
```

### Test Your Changes

```bash
# Start OrcaMCP
open /Applications/OrcaMCP.app  # macOS

# Verify MCP server is running
curl -s http://localhost:13618/mcp | jq .

# Test with curl
curl -s -X POST http://localhost:13618/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_scene_info","arguments":{}}}' | jq .
```

## Code Guidelines

### Core Principles

- **Single Responsibility**: Each method has one clear purpose
- **DRY**: Don't repeat yourself - centralize validation and shared logic
- **Clean Code**: Methods should be small, focused, and well-named

### C++ Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Classes | PascalCase | `OrcaMCPServer` |
| Methods | snake_case | `handle_load_model` |
| Variables | snake_case | `object_id` |
| Constants | UPPER_SNAKE_CASE | `MAX_TIMEOUT` |

### Important Patterns

All MCP tool handlers must use `run_on_main_thread()`:

```cpp
nlohmann::json handle_my_tool(const nlohmann::json& params) {
    return run_on_main_thread<nlohmann::json>([&]() {
        // GUI operations go here
        return result;
    });
}
```

See [Adding Tools Guide](docs/contributing/adding-tools.md) for complete details.

## Testing Checklist

Before submitting a PR:

- [ ] Tool works with valid parameters
- [ ] Clear error for missing required parameters
- [ ] Clear error for invalid parameter types
- [ ] Works with empty scene (no crash)
- [ ] Works with multiple objects
- [ ] Works with multiple plates
- [ ] Documentation updated (if adding new features)

## Pull Request Guidelines

### PR Title

Use clear, descriptive titles:
- `Add volume calculation tool`
- `Fix crash when loading large STL files`
- `Update render_plate_view to support higher resolutions`

### PR Description

Include:
- **What**: Brief description of changes
- **Why**: Problem being solved or feature being added
- **How**: High-level implementation approach
- **Testing**: How you verified it works

### Review Process

1. All PRs require at least one review
2. CI must pass (builds, linting)
3. Address review feedback promptly
4. Squash commits if requested

## License

By contributing to OrcaMCP, you agree that your contributions will be licensed under the [AGPL-3.0 License](LICENSE.txt), the same license as OrcaSlicer and this project.

## Attribution

OrcaMCP is built on:
- **[OrcaSlicer](https://github.com/SoftFever/OrcaSlicer)** - The excellent slicer we extend
- **[PrusaSlicer](https://github.com/prusa3d/PrusaSlicer)** & **[BambuStudio](https://github.com/bambulab/BambuStudio)** - Upstream projects

When contributing, please respect the upstream projects' contributions and licenses.

## Questions?

- Check the [documentation](docs/)
- Search [existing issues](https://github.com/okets/OrcaMCP/issues)
- Open a new issue for discussions

Thank you for helping make OrcaMCP better!
