# Windows Testing Checklist for OrcaMCP

Pre-release testing checklist for Windows builds of OrcaMCP.

**Testing Strategy:**
- **Part 1:** Development environment testing (catch code issues first)
- **Part 2:** MCP client connectivity (can use dev build)
- **Part 3:** Installer & distribution (test after code is verified)

---

# Part 1: Development Environment Testing

## 1. Test Environment Setup

Record test environment details:

| Item | Value |
|------|-------|
| Windows Version | |
| Build Number | |
| Architecture | x64 / ARM64 |
| Display Scaling | |
| Antivirus | |
| OrcaMCP Version | |
| Python Version | |

### Build Location
- [ ] OrcaMCP built successfully in dev environment
- [ ] Build location documented: `_______________________________`
- [ ] Python available in PATH

---

## 2. Application Launch & HTTP Server

### 2.1 Basic Launch
- [ ] Application launches from dev build directory
- [ ] Window title shows "OrcaMCP" (not "OrcaSlicer")
- [ ] About dialog shows "OrcaMCP" and correct version
- [ ] No errors/crashes on startup

### 2.2 HTTP Server Startup
- [ ] MCP HTTP server starts on port 13618
- [ ] Verify with: `curl http://localhost:13618/mcp` (or PowerShell equivalent)
- [ ] Windows Firewall prompt appears (if first run)
- [ ] After allowing firewall, HTTP server accessible
- [ ] Server works with firewall set to "Private networks only"

---

## 3. MCP Tool Functionality

### 3.1 Core Tools
- [ ] `get_server_info` - returns server version and status
- [ ] `get_scene_info` - returns current scene data
- [ ] `new_project` - clears scene
- [ ] `load_project` - loads 3MF file
- [ ] `save_project` - saves current project

### 3.2 Model Operations
- [ ] `load_model` with Windows path: `C:\Users\Name\model.stl`
- [ ] `load_model` with forward slashes: `C:/Users/Name/model.stl`
- [ ] `load_model` with UNC path: `\\server\share\model.stl` (if applicable)
- [ ] `load_model` with path containing spaces: `C:\Users\My Name\My Models\test.stl`
- [ ] `auto_orient` - model reorients
- [ ] `arrange_objects` - objects arranged on plate
- [ ] `rename_object` - object renamed

### 3.3 Transform Operations
- [ ] `move_object` - position changes
- [ ] `rotate_object` - rotation applies
- [ ] `scale_object` - scale changes
- [ ] `mirror_object` - mirror applies
- [ ] `flatten_object` - object flattened
- [ ] `clone_object` - duplicate created
- [ ] `delete_object` - object removed

### 3.4 Slicing & Export
- [ ] `slice_all` - slicing starts
- [ ] `get_slicing_status` - returns progress/completion
- [ ] `get_print_estimate` - returns time/filament estimate
- [ ] `export_gcode` with Windows path: `C:\Users\Name\output.gcode`
- [ ] `export_3mf` with Windows path
- [ ] Exported files exist and are valid

### 3.5 Visualization (360 View)
- [ ] `render_plate_view` - returns base64 image
- [ ] `render_plate_view` with `save_to_file: true` - saves to temp
- [ ] Image contains visible 3D model (not blank/black)
- [ ] Turntable preview generates multiple frames
- [ ] Temp files created in correct Windows temp directory

### 3.6 Presets & Configuration
- [ ] `get_presets` - returns printer/filament/print presets
- [ ] `select_preset` - preset changes
- [ ] `apply_config` - settings apply
- [ ] `get_valid_config_keys` - returns valid keys

### 3.7 Printer Operations
- [ ] `get_printers` - returns connected printers (if any)
- [ ] `select_printer` - printer selected
- [ ] `send_to_printer` - print job sent (requires connected printer)

---

## 4. Path Handling Edge Cases

### 4.1 Special Characters in Paths
- [ ] Spaces: `C:\Users\John Doe\models\test.stl`
- [ ] Parentheses: `C:\Models (backup)\test.stl`
- [ ] Unicode: `C:\Users\名前\models\test.stl`
- [ ] Ampersand: `C:\R&D\models\test.stl`
- [ ] Long paths (>260 chars): `C:\Very\Long\Path\...\model.stl`

### 4.2 Path Format Compatibility
- [ ] Forward slashes work: `C:/Users/Name/model.stl`
- [ ] Backslashes work: `C:\Users\Name\model.stl`
- [ ] Mixed slashes work: `C:\Users/Name\model.stl`
- [ ] Relative paths work from OrcaMCP working directory

---

## 5. Environment Variables

### 5.1 Bridge Configuration
Test with environment variables set:
- [ ] `ORCAMCP_HOST=localhost` - bridge connects
- [ ] `ORCAMCP_PORT=13618` - bridge connects to correct port
- [ ] `ORCAMCP_TIMEOUT=300` - long operations don't timeout
- [ ] `ORCAMCP_DEBUG=1` - debug output appears in stderr

### 5.2 Python Path
- [ ] System Python (`python3`) works
- [ ] Python from PATH works
- [ ] Full path to Python works: `C:\Python312\python.exe`
- [ ] Python from Windows Store works

---

## 6. Error Handling

### 6.1 Connection Errors
- [ ] OrcaMCP not running: Graceful error message
- [ ] Wrong port: Clear error message
- [ ] Firewall blocking: Error indicates network issue

### 6.2 File Errors
- [ ] File not found: Clear error with path
- [ ] Permission denied: Error indicates permission issue
- [ ] Invalid file format: Error indicates format issue

### 6.3 Active Warnings
- [ ] `active_warnings` field present in tool responses
- [ ] Slicing conflicts reported in warnings
- [ ] Warning count updates after resolving issues

---

## 7. Performance

### 7.1 Response Times
- [ ] `get_scene_info` responds in <1 second
- [ ] `load_model` (small STL) responds in <2 seconds
- [ ] `slice_all` (simple model) completes in <30 seconds
- [ ] `render_plate_view` responds in <3 seconds

### 7.2 Resource Usage
- [ ] Memory usage stable during normal operation
- [ ] No memory leaks after repeated tool calls
- [ ] CPU usage reasonable when idle

---

## 8. Windows-Specific Features

### 8.1 File Association
- [ ] `.3mf` files open with OrcaMCP
- [ ] `.stl` files can be opened with OrcaMCP
- [ ] Right-click context menu works (if implemented)

### 8.2 High DPI
- [ ] UI scales correctly on 4K displays
- [ ] UI scales correctly at 150% scaling
- [ ] UI scales correctly at 200% scaling
- [ ] Preview images render at correct resolution

---

## 9. Multi-Instance & Ports

### 9.1 Single Instance
- [ ] Only one instance of OrcaMCP runs at a time
- [ ] Second launch focuses existing window

### 9.2 Port Conflicts
- [ ] Port 13618 already in use: OrcaMCP shows error or uses fallback
- [ ] Document: How does OrcaMCP handle port conflicts?

---

# Part 2: MCP Client Connectivity

## 10. MCP Client Auto-Configuration

### 10.1 Bridge Script Paths
- [ ] Bridge script exists at `scripts\orcamcp-bridge.py`
- [ ] Bridge script path with forward slashes works in config
- [ ] Bridge script path with backslashes works in config

### 10.2 Client Connection Setup
Test each client's "Connect" button in Preferences → MCP Clients:

| Client | Config Path | Connect | Disconnect | Verified |
|--------|-------------|---------|------------|----------|
| Claude Desktop | `%APPDATA%\Claude\claude_desktop_config.json` | [x] | [ ] | [x] |
| Claude Code | `%USERPROFILE%\.claude.json` | [x] | [ ] | [x] |
| Cursor | `%USERPROFILE%\.cursor\mcp.json` | [x] | [ ] | [x] |
| Windsurf | `%USERPROFILE%\.codeium\windsurf\mcp_config.json` | [x] | [ ] | [x] |
| Cline | `%APPDATA%\Code\User\globalStorage\saoudrizwan.claude-dev\settings\cline_mcp_settings.json` | [x] | [ ] | [x] |
| Codex CLI | `%USERPROFILE%\.codex\config.toml` | [x] | [ ] | [x] |
| GitHub Copilot | `%USERPROFILE%\.copilot\mcp-config.json` | [x] | [ ] | [x] |
| VS Code | `.vscode\mcp.json` (project-level) | N/A | N/A | [ ] |

### 10.3 Client Connection Test
For each connected client:
- [ ] Restart client application after connecting
- [ ] Client shows OrcaMCP/orca-slicer in MCP server list
- [ ] Run `get_server_info` - returns valid response
- [ ] Run `get_scene_info` - returns scene data

### 10.4 Graceful Offline Behavior
- [ ] Close OrcaMCP completely
- [ ] In MCP client, run `get_server_info`
- [ ] Response shows "OrcaMCP is not running" message (not error alert)
- [ ] Client does NOT show alarming red error indicator
- [ ] Run `tools/list` - returns cached/minimal tools list
- [ ] Reopen OrcaMCP, verify tools work again

---

# Part 3: Installer & Distribution

## 11. Installation & Branding

### 11.1 Installer Branding
- [ ] Installer window title shows "OrcaMCP" (not "OrcaSlicer")
- [ ] Welcome screen shows OrcaMCP name and logo
- [ ] License screen shows OrcaMCP branding
- [ ] Installation directory default: `C:\Program Files\OrcaMCP`
- [ ] Start Menu folder: `OrcaMCP`
- [ ] Desktop shortcut name: `OrcaMCP`
- [ ] Uninstaller entry shows "OrcaMCP" in Add/Remove Programs

### 11.2 Installation Paths
- [ ] Default installation works: `C:\Program Files\OrcaMCP`
- [ ] Custom installation path works (with spaces): `C:\Users\Name\My Apps\OrcaMCP`
- [ ] Installation to non-C: drive works: `D:\OrcaMCP`
- [ ] Uninstall removes all files cleanly
- [ ] Uninstall removes registry entries
- [ ] Uninstall removes Start Menu entries

### 11.3 Post-Installation Launch
- [ ] Application launches from Start Menu shortcut
- [ ] Application launches from Desktop shortcut
- [ ] Application launches from installation directory
- [ ] Window title shows "OrcaMCP" (not "OrcaSlicer")
- [ ] About dialog shows "OrcaMCP" and correct version

---

## 12. SmartScreen Behavior (Unsigned Installer)

**Expected behavior for unsigned OrcaMCP installer:**

Unlike macOS (which blocks unsigned apps), Windows allows unsigned installers with warnings.
Users can bypass with 2 clicks. This is acceptable for open-source software.

### 12.1 Download & Launch Tests

| Step | Expected Behavior | Pass |
|------|-------------------|------|
| Download in Edge | "This file is not commonly downloaded" warning | [ ] |
| Download in Chrome | Similar warning or clean download | [ ] |
| Download in Firefox | Usually no warning | [ ] |
| Run installer | SmartScreen popup appears | [ ] |

### 12.2 SmartScreen Popup Test

When running the unsigned installer, verify this exact flow:

1. **Initial popup appears:**
   - [ ] Title: "Windows protected your PC"
   - [ ] Text: "Microsoft Defender SmartScreen prevented an unrecognized app from starting"
   - [ ] Publisher shows: "Unknown publisher"
   - [ ] Blue "Don't run" button visible
   - [ ] "More info" link visible

2. **After clicking "More info":**
   - [ ] App name shown (should include "OrcaMCP")
   - [ ] "Run anyway" button appears
   - [ ] Clicking "Run anyway" launches installer

3. **Installation proceeds:**
   - [ ] Installer runs normally after bypass
   - [ ] No additional SmartScreen warnings during install
   - [ ] Application launches without SmartScreen warning after install

### 12.3 Windows Defender Tests
- [ ] Defender does not flag installer as malware
- [ ] Defender does not flag installed application
- [ ] Real-time protection doesn't quarantine any files
- [ ] If flagged: Document the detection name and submit false positive report

### 12.4 Enterprise/Managed Environment (if testable)
- [ ] Document: Does Group Policy block unsigned installers?
- [ ] Document: Does Windows S Mode block installation?

### 12.5 Future: Signed Installer
If code signing is added later:
- [ ] SmartScreen passes without "Unknown publisher" warning
- [ ] Publisher name shows correctly (requires EV cert for immediate trust)
- [ ] Standard cert: May still warn until reputation builds

---

# Testing Results

## Issues Found

| # | Category | Description | Severity | Status |
|---|----------|-------------|----------|--------|
| 1 | Codex CLI | TOML path escape issue on Windows - backslashes interpreted as unicode escapes | Medium | Fixed |
| 2 | | | | |
| 3 | | | | |

### Issue #1: Codex CLI TOML Path Escaping (Windows-only)

**Problem:** Codex CLI uses TOML config format. On Windows, backslashes in paths are interpreted as escape sequences.

**Error:**
```
Error parsing user config file C:\Users\Hanan\.codex\config.toml: TOML parse error at line 4, column 14
  |
4 | args = ["C:\Users\Hanan\.orcamcp\orcamcp-bridge.py"]
  |              ^
too few unicode value digits, expected unicode hexadecimal value
```

**Cause:** In TOML, `\U` is interpreted as a Unicode escape sequence (expecting 8 hex digits).

**Workarounds:**
1. Use forward slashes: `args = ["C:/Users/Hanan/.orcamcp/orcamcp-bridge.py"]`
2. Use escaped backslashes: `args = ["C:\\Users\\Hanan\\.orcamcp\\orcamcp-bridge.py"]`
3. Use literal strings (single quotes): `args = ['C:\Users\Hanan\.orcamcp\orcamcp-bridge.py']`

**Fix Applied:** Modified `MCPClientConfig.cpp` to convert backslashes to forward slashes when writing TOML config for Codex CLI.

**Note:** Works correctly on macOS (no backslashes in paths).

**Manual Workaround (if using existing config):** Edit `~/.codex/config.toml` and change backslashes to forward slashes in the path.

---

## Sign-Off

| Role | Name | Date | Signature |
|------|------|------|-----------|
| Tester | | | |
| Developer | | | |

---

## References

- [MCP Auto-Configuration Guide](../MCP-AUTO-CONFIGURATION-GUIDE.md)
- [Troubleshooting Guide](../setup/troubleshooting.md)
- [macOS Code Signing](./macos-code-signing.md)
