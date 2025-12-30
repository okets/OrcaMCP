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
- [x] OrcaMCP built successfully in dev environment
- [x] Build location: `C:\ThinkingHomes\OrcaMCP\build\src\Release\`
- [x] Python available in PATH

---

## 2. Application Launch & HTTP Server

### 2.1 Basic Launch
- [x] Application launches from dev build directory
- [x] Window title shows "OrcaMCP" (not "OrcaSlicer")
- [x] About dialog shows "OrcaMCP" and correct version
- [x] No errors/crashes on startup

### 2.2 HTTP Server Startup
- [x] MCP HTTP server starts on port 13618
- [x] Verify with: `curl http://localhost:13618/mcp` (or PowerShell equivalent)
- [x] Windows Firewall prompt appears (if first run)
- [x] After allowing firewall, HTTP server accessible
- [x] Server works with firewall set to "Private networks only"

---

## 3. MCP Tool Functionality

### 3.1 Core Tools
- [x] `get_server_info` - returns server version and status
- [x] `get_scene_info` - returns current scene data
- [x] `new_project` - clears scene (v2.3.2.10: dialog suppression added)
- [x] `load_project` - loads 3MF file (v2.3.2.10: dialog suppression)
- [x] `save_project` - saves current project (v2.3.2.10: dialog suppression)

### 3.2 Model Operations
- [x] `load_model` with Windows path: `C:\Users\Name\model.stl`
- [x] `load_model` with forward slashes: `C:/Users/Name/model.stl`
- [ ] `load_model` with UNC path: `\\server\share\model.stl` (if applicable)
- [ ] `load_model` with path containing spaces: `C:\Users\My Name\My Models\test.stl`
- [x] `auto_orient` - model reorients
- [x] `arrange_objects` - objects arranged on plate
- [x] `rename_object` - object renamed

### 3.3 Transform Operations
- [x] `move_object` - position changes
- [x] `rotate_object` - rotation applies
- [x] `scale_object` - scale changes
- [x] `mirror_object` - mirror applies
- [x] `flatten_object` - object flattened
- [x] `clone_object` - duplicate created
- [x] `delete_object` - object removed (v2.3.2.10: include_preview support)

### 3.4 Slicing & Export
- [x] `slice_all` - slicing starts (v2.3.2.10: dialog suppression)
- [x] `get_slicing_status` - returns progress/completion
- [x] `get_print_estimate` - returns time/filament estimate (poll until ready)
- [x] `export_gcode` with output_path - silent export (v2.3.2.10: NEW)
- [x] `export_3mf` with output_path - silent export
- [x] Exported files exist and are valid

### 3.5 Visualization (360 View)
- [x] `render_plate_view` - returns base64 image
- [x] `render_plate_view` with `save_to_file: true` - saves to temp
- [x] Image contains visible 3D model (not blank/black)
- [x] Turntable preview via `include_preview` parameter on load_model, etc.
- [x] `get_preview_base64` - returns base64 data URI
- [x] Temp files created in correct Windows temp directory

### 3.6 Presets & Configuration
- [x] `get_presets` - returns printer/filament/print presets
- [x] `select_preset` - preset changes (v2.3.2.10: dialog suppression)
- [x] `apply_config` - settings apply (v2.3.2.10: dialog suppression)
- [x] `get_valid_config_keys` - returns valid keys (29 per-object keys)

### 3.7 Printer Operations
- [x] `get_printers` - returns connected printers (empty if none)
- [ ] `select_printer` - printer selected (requires Bambu printer)
- [x] `send_to_printer` - dialog opens (v2.3.2.10: dialog suppression)

---

## 4. Path Handling

### 4.1 Path Format Compatibility
- [x] Forward slashes work: `C:/Users/Name/model.stl`
- [x] Backslashes work: `C:\Users\Name\model.stl`
- [x] Bridge normalizes paths automatically

---

## 5. Error Handling

### 5.1 Connection Errors
- [x] OrcaMCP not running: Graceful "not running" message
- [x] Bridge returns cached/minimal tools when offline

### 5.2 Active Warnings
- [x] `active_warnings` field present in tool responses
- [x] Warning count field always present (even when 0)

---

# Part 2: MCP Client Connectivity

## 6. MCP Client Auto-Configuration

### 6.1 Bridge Script
- [x] Bridge script exists at `scripts\orcamcp-bridge.py`
- [x] Bridge handles path normalization for Windows

### 6.2 Client Connection Setup
Test each client's "Connect" button in Preferences → MCP Clients:

| Client | Config Path | Connect | Verified |
|--------|-------------|---------|----------|
| Claude Desktop | `%APPDATA%\Claude\claude_desktop_config.json` | [x] | [x] |
| Claude Code | `%USERPROFILE%\.claude.json` | [x] | [x] |
| Cursor | `%USERPROFILE%\.cursor\mcp.json` | [x] | [x] |
| Windsurf | `%USERPROFILE%\.codeium\windsurf\mcp_config.json` | [x] | [x] |
| Cline | `%APPDATA%\Code\...\cline_mcp_settings.json` | [x] | [x] |
| Codex CLI | `%USERPROFILE%\.codex\config.toml` | [x] | [x] |
| GitHub Copilot | `%USERPROFILE%\.copilot\mcp-config.json` | [x] | [x] |

### 6.3 Graceful Offline Behavior
- [x] OrcaMCP not running: Returns "not running" message
- [x] Bridge returns minimal tools list when offline
- [x] `start_orca` tool available to launch OrcaMCP

---

# Part 3: Installer Testing

*Note: Installer branding and SmartScreen testing are separate from MCP functionality testing. See installer-specific documentation.*

---

# Testing Results

## Issues Found

| # | Category | Description | Severity | Status |
|---|----------|-------------|----------|--------|
| 1 | Codex CLI | TOML path escape issue on Windows - backslashes interpreted as unicode escapes | Medium | Fixed |
| 2 | MCP Tools | `new_project` shows save confirmation dialog blocking automation | Medium | Fixed in v2.3.2.10 |
| 3 | MCP Tools | `export_gcode` filename template error shows blocking dialog | Medium | Fixed in v2.3.2.10 |
| 4 | MCP Tools | `export_gcode` missing silent export support (inconsistent with export_3mf) | Medium | Fixed in v2.3.2.10 |

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

### Issue #2: new_project Dialog Blocking (Fixed in v2.3.2.10)

**Problem:** When calling `new_project` with unsaved changes, OrcaMCP showed a "Save changes?" dialog that blocked MCP automation.

**Cause:** The `new_project` handler didn't have dialog suppression enabled like `load_model` and `load_project`.

**Fix Applied:** Added `set_mcp_dialog_suppression(true/false)` wrapper around `plater->new_project()` call in `OrcaMCPServer.cpp`.

**Behavior After Fix:** Dialog is auto-dismissed (choosing "No" to discard changes), and any messages are captured in the response's `info_messages` array.

### Issue #3: Comprehensive Dialog Suppression (Fixed in v2.3.2.10)

**Problem:** Multiple MCP endpoints could show blocking error dialogs (e.g., filename template parsing errors, config validation errors) that would halt MCP automation.

**Example Error:**
```
OrcaMCP error
Failed processing of the filename_format template.
Parsing error at line 1: Non-integer index is not allowed to address a vector variable.
{input_filename_base}_{layer_height}mm_{filament_type[initial_tool]}_{printer_model}_{print_time}.gcode
```

**Cause:** Many endpoints didn't have dialog suppression enabled, allowing error dialogs to block the MCP communication flow.

**Fix Applied:** Added `set_mcp_dialog_suppression(true/false)` wrappers to all endpoints that interact with UI or could trigger error dialogs:
- `slice_all`
- `export_gcode`
- `export_3mf`
- `save_project`
- `select_preset`
- `apply_config`
- `clone_preset`
- `save_preset`
- `delete_preset`
- `reset_preset`
- `send_to_printer`
- (In addition to already-suppressed: `load_model`, `load_project`, `new_project`)

**Behavior After Fix:** Error messages are captured and returned in `error_messages` or `info_messages` arrays instead of showing blocking dialogs. For errors, the response status is set to `"error"`.

### Issue #4: Silent G-code Export (Added in v2.3.2.10)

**Problem:** `export_gcode` always opened a file dialog, even when `output_path` was provided. This was inconsistent with `export_3mf` which supported silent export.

**Cause:** The upstream OrcaSlicer `Plater::export_gcode(bool)` method always shows a file dialog. There was no public API for silent export.

**Fix Applied:**
1. Added new method `Plater::export_gcode_to_file(const std::string& output_path)` in `Plater.hpp/cpp`
2. Updated MCP `export_gcode` handler to use silent export when `output_path` is provided

**Behavior After Fix:**
- With `output_path` → Silent export, no dialog, returns `{"status": "export_started", "output_path": "..."}`
- Without `output_path` → Opens file dialog (unchanged)

This makes `export_gcode` consistent with `export_3mf` behavior.

---

## Sign-Off

| Version | Date | Platform | Result | Notes |
|---------|------|----------|--------|-------|
| v2.3.2.10 | 2025-12-30 | Windows | ✓ Pass | Dialog suppression, silent G-code export, comprehensive testing |
| v2.3.2.9 | 2025-12-30 | Windows | ✓ Pass | Update from v2.3.2.8 successful, MCP connectivity verified |
| v2.3.2.8 | 2025-12-28 | Windows | ✓ Pass | Initial Windows release |

---

## References

- [MCP Auto-Configuration Guide](../MCP-AUTO-CONFIGURATION-GUIDE.md)
- [Troubleshooting Guide](../setup/troubleshooting.md)
- [macOS Code Signing](./macos-code-signing.md)
