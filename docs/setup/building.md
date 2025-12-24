# Building OrcaMCP

This guide covers building OrcaSlicer with OrcaMCP from source.

## Prerequisites

### macOS

```bash
# Xcode command line tools
xcode-select --install

# Homebrew packages
brew install cmake ninja ccache
```

### Linux (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build ccache \
    libgtk-3-dev libdbus-1-dev libglew-dev libssl-dev \
    libcurl4-openssl-dev libudev-dev libglu1-mesa-dev \
    gettext
```

### Windows

- Visual Studio 2019 or 2022 with C++ workload
- CMake (from cmake.org or Visual Studio)
- Git for Windows

## Quick Build

### macOS

```bash
# Clone repository
git clone https://github.com/okets/OrcaMCP.git
cd OrcaMCP

# Build everything (first time - builds dependencies too)
./build_release_macos.sh

# Build only slicer (after deps are built)
./build_release_macos.sh -s

# Build with Ninja for faster compilation
./build_release_macos.sh -s -x
```

The built app will be at: `build/arm64/src/Release/OrcaSlicer.app`

### Linux

```bash
# Clone repository
git clone https://github.com/okets/OrcaMCP.git
cd OrcaMCP

# Build
./build_release.sh

# Or with specific options
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Windows

```batch
REM Clone repository
git clone https://github.com/okets/OrcaMCP.git
cd OrcaMCP

REM Build with Visual Studio
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

## Build Options

| Option | Description |
|--------|-------------|
| `-s` | Skip dependency building (faster rebuild) |
| `-x` | Use Ninja generator instead of Make |
| `-d` | Debug build |

## Installing

### macOS

```bash
# Copy to Applications
cp -R build/arm64/src/Release/OrcaSlicer.app /Applications/OrcaMCP.app

# Or create symlink for development
ln -s $(pwd)/build/arm64/src/Release/OrcaSlicer.app /Applications/OrcaMCP.app
```

### Linux

```bash
# The executable is at:
./build/src/OrcaSlicer
```

### Windows

The executable is at:
```
build\src\Release\OrcaSlicer.exe
```

## Verifying MCP Server

After launching OrcaMCP, verify the MCP server is running:

```bash
# Check server info
curl -s http://localhost:13618/mcp | jq .

# Should return:
# {
#   "name": "orca-slicer",
#   "version": "1.0.0",
#   "protocol": "mcp",
#   "description": "OrcaSlicer 3D Slicer MCP Server..."
# }
```

## Troubleshooting Build Issues

### CMake Cache Issues

If you get generator conflicts:
```bash
rm -rf build/CMakeCache.txt build/CMakeFiles
./build_release_macos.sh -s -x
```

### Missing Dependencies

macOS:
```bash
brew install cmake ninja ccache
```

### OpenGL Issues (Linux)

```bash
sudo apt install libgl1-mesa-dev libglu1-mesa-dev
```

### Permission Denied

```bash
chmod +x build_release_macos.sh
```

## Development Workflow

For rapid iteration:

```bash
# 1. Make code changes
# 2. Rebuild (fast with Ninja)
./build_release_macos.sh -s -x

# 3. Test
curl -s http://localhost:13618/mcp | jq .

# 4. Run specific tool
curl -X POST http://localhost:13618/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

## Building Specific Components

### Just the MCP Server

MCP server files are in `src/slic3r/GUI/OrcaMCP/`. After modifying these, a full rebuild is needed since they're linked into the main executable.

### Dependencies Only

```bash
./build_release_macos.sh -d  # Builds deps only
```

## Cross-Platform Notes

### File Paths

Use forward slashes in code, even on Windows:
```cpp
std::string path = "some/path/file.stl";  // Works everywhere
```

### Line Endings

The repository uses LF line endings. Configure git:
```bash
git config core.autocrlf input  # On Windows
```

## See Also

- [Configuration Guide](configuration.md) - Set up Claude Code integration
- [Troubleshooting](troubleshooting.md) - Common issues
