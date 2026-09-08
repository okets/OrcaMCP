# Creator 5 Pro Support: Upstream Sync, Multi-Material Tools, Flashforge Integration, Release

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring OrcaMCP up to upstream OrcaSlicer 2.5.0-dev (which already ships the Flashforge Creator 5 / 5 Pro profiles and the color-mixing engine), expose the multi-material and toolchanger settings through MCP tools, add an open Flashforge Creator 5 integration (direct send, status, control, device page) and ship a release that other Flashforge users can install.

**Architecture:** Three stages, each leaving the branch buildable and releasable. Stage 1 merges `upstream/main` into `mcp`, then adds MCP tools on top of upstream's mixed-filament and multi-extruder config surface, plus a schema regeneration script and a pytest harness so tool drift is caught. Stage 2 extends upstream's existing `Flashforge` print host (which already speaks the printer's HTTP API on port 8898) with status and control endpoints, exposes them as MCP tools, makes `send_to_printer` upload without a dialog, and registers a `FlashforgePrinterAgent` so OrcaSlicer's own Device tab renders the printer. Stage 3 re-applies the fork's CI rebranding onto upstream's reworked workflows, fixes docs and packaging, and cuts the release.

**Tech Stack:** C++17 (wxWidgets GUI, nlohmann::json, Boost, libcurl via `Slic3r::Http`), CMake/CPack, Python 3 (bridge, pytest), GitHub Actions, Catch2.

**Spec:** This plan is derived from the conversation of 2026-09-07 (assessment + Q&A). Key facts established there are restated inline where a task depends on them.

## Global Constraints

- Branch to work on: `mcp` (default branch of `okets/OrcaMCP`). Upstream remote is `upstream` = `OrcaSlicer/OrcaSlicer`, ref `upstream/main` (already fetched; head `37e1582c4c`, 2026-09-07).
- Work **in place on a branch**, not in a git worktree: `build/` and `deps/build/` are untracked, multi-GB, and take hours to rebuild. A worktree would force a full deps rebuild.
- Version string convention: upstream version + `.N-dev` suffix. Upstream is `2.5.0-dev`, so this cycle is `SoftFever_VERSION "2.5.0.1-dev"` and the release tag is `v2.5.0.1-dev`. The tag MUST equal `v` + `SoftFever_VERSION` exactly (release.yml downloads artifacts by that name).
- Keep `SLIC3R_APP_NAME "OrcaMCP"` and `SLIC3R_APP_KEY "OrcaMCP"` in `version.inc` through every merge.
- Never vendor, download, or link Flashforge's closed network plugin (`FLASHNETWORK*.DAT` / `fnet_*`). All printer communication goes through the open HTTP API on port 8898 (`/detail`, `/control`, `/gcodeList`, `/gcodeThumb`, `/printGcode`, `/uploadGcode`), authenticated by `serialNumber` + `checkCode`.
- MCP tool conventions (from `docs/contributing/adding-tools.md`): `register_tool({name, description, input_schema, handler})`; extract params on the HTTP thread, capture by value, run GUI work inside `run_on_main_thread(...)` which must return `nlohmann::json`; errors either `throw std::runtime_error` or return `{"status":"error","message":...}`; path params must be named `file_path`, `output_path`, or `path` for Windows normalization in the bridge.
- Every new tool: regenerate `scripts/tools_schema.py` (Task 1.4 script), add an entry to `docs/tools/reference.md`, and keep the tool count claims in README/CLAUDE.md/docs in sync (Task 3.3).
- Testing rule from CLAUDE.md: verify MCP behaviour through the `mcp__orca-slicer__*` tools, not curl. Set `ORCAMCP_APP_PATH` to the freshly built app so `start_orca` launches the right binary.
- Coding standards: single responsibility, DRY, small well-named functions, PascalCase classes, snake_case functions.
- Commit after every task with a message ending in `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.

---

## File Structure

### Stage 1

| Path | Action | Responsibility |
|---|---|---|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp` | Create | Shared helpers moved out of `OrcaMCPServer.cpp`: `run_on_main_thread`, `get_active_warnings_json`, `McpDialogSuppressionGuard` (RAII) |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` | Modify | Include the common header, delete the moved definitions, call `register_filament_tools()`, extend `get_valid_config_keys` with a `toolchanger` category and fix the inverted enum check, make `apply_config` report invalid keys |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp` | Modify | Declare `register_filament_tools()` |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.hpp/.cpp` | Create | Pure helpers: filament slot listing, mixed-slot read/validate, flush matrix get/set, project-config apply + refresh |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp` | Create | Tool registrations: `get_filaments`, `set_mixed_filament`, `delete_mixed_filament`, `set_object_filament`, `get_flush_volumes`, `set_flush_volumes`, `auto_calc_flush_volumes`, `get_toolchanger_config` |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp/.cpp` | Modify | `ApplyConfig` returns applied/invalid key lists; new `project` preset type writes `PresetBundle::project_config` |
| `src/slic3r/GUI/Plater.hpp/.cpp` | Modify | Extract `Sidebar::apply_mixed_filament(const MixedFilamentResult&, int edit_cfg_idx)` from the dialog-driven add/edit paths so MCP and GUI share one implementation |
| `src/slic3r/CMakeLists.txt` | Modify | Add the new `.cpp`/`.hpp` files to `SLIC3R_GUI_SOURCES` |
| `scripts/regen_tools_schema.py` | Create | Fetch `tools/list` from the running server and rewrite `scripts/tools_schema.py` |
| `scripts/tests/test_tools_schema.py` | Create | pytest: static invariants of `tools_schema.py`; live drift check against the server when it is up |
| `scripts/tools_schema.py` | Regenerate | Static tool list used by the bridge on cold start |

### Stage 2

| Path | Action | Responsibility |
|---|---|---|
| `src/slic3r/Utils/FlashforgeApi.hpp/.cpp` | Create | Pure, testable functions: `parse_detail()` → `FlashforgePrinterStatus`, `make_control_payload()`, `make_print_gcode_payload()`, `make_credentials_payload()` |
| `src/slic3r/Utils/Flashforge.hpp/.cpp` | Modify | New non-virtual methods on the host: `fetch_status`, `send_control`, `pause_job/resume_job/cancel_job`, `set_temperatures`, `set_light`, `list_gcode_files`, `print_gcode_file` |
| `tests/slic3rutils/test_flashforge_api.cpp` + `tests/slic3rutils/CMakeLists.txt` | Create/Modify | Catch2 tests for `FlashforgeApi` |
| `resources/profiles/Flashforge/machine/Flashforge Creator 5*.json` | Modify | `host_type` `octoprint` → `flashforge` on all 8 Creator 5 / 5 Pro nozzle and base files |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp/.cpp` | Create | Resolve the active print-host config (selected physical printer or edited printer preset), build a `PrintHost`, material auto-mapping, status JSON shaping |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp` | Create | Tool registrations: `discover_printers`, `add_physical_printer`, `select_printer` (extended), `get_printer_status`, `printer_control`, `list_printer_files`, `print_printer_file`; `send_to_printer` moves here and gains a direct-upload path |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` | Modify | Remove the old `get_printers`/`select_printer`/`send_to_printer` bodies, call `register_printer_tools()` |
| `src/slic3r/Utils/FlashforgePrinterAgent.hpp/.cpp` | Create | `IPrinterAgent` implementation so OrcaSlicer's Device tab shows the Creator 5 (status, temps, progress, pause/resume/stop, camera URL) |
| `scripts/orcamcp-bridge.py` | Modify | Add `gcode_path` handling only if a new tool needs a path param not already covered (Task 2.6 checks) |

### Stage 3

| Path | Action | Responsibility |
|---|---|---|
| `.github/workflows/release.yml` | Modify | Artifact names with arch suffixes, `draft` expression fix, compare URL fix |
| `.github/workflows/build_orca.yml` | Modify | Re-apply fork rebranding on upstream's multi-arch workflow: OrcaMCP DMG/AppImage/installer names, notarization gate for `okets/OrcaMCP`, `-j 3`, `-istrlL`, MCP scripts copy |
| `.github/workflows/build_all.yml` | Modify | `mcp` branch triggers, flatpak disabled, unit-test artifact names matched |
| `.github/workflows/build_deps.yml` | Modify | `-1` → `-j 3` |
| `.github/workflows/winget_updater.yml` | Delete | Points at `SoftFever.OrcaSlicer` |
| `CMakeLists.txt` | Modify | CPack block rebranded with arch suffix (done in Stage 1 merge; re-verified here) |
| `README.md`, `CLAUDE.md`, `docs/README.md`, `docs/tools/reference.md`, `docs/setup/configuration.md`, `PROJECT_ROADMAP.md` | Modify | Tool counts, new tools, Creator 5 section, downloads, install-bundle bridge path, fixed build commands, fixed "Adding New Tools" snippet |
| `version.inc` | Modify | `2.5.0.1-dev` |

---

# Stage 1: Sync with upstream and expose multi-material / toolchanger tooling

### Task 1.1: Merge `upstream/main` into `mcp`

**Files:**
- Modify: the 10 conflicting files listed below, plus any compile-fix fallout.

**Interfaces:**
- Produces: a buildable `mcp` branch at upstream 2.5.0-dev with the fork's MCP layer intact; `version.inc` = `2.5.0.1-dev`.

- [ ] **Step 1: Create the sync branch and start the merge**

```bash
cd /Users/hanan/Projects/OrcaMCP
git status --porcelain            # must be empty
git checkout -b sync-upstream-2.5 mcp
git fetch upstream
git merge --no-ff upstream/main   # expect: CONFLICT in 10 files
git diff --name-only --diff-filter=U
```

Expected conflict list (verified with `git merge-tree` on 2026-09-07):

```
.github/ISSUE_TEMPLATE/bug_report.yml
.github/workflows/build_orca.yml
CMakeLists.txt
README.md
build_release_vs.bat
resources/web/data/text.js
src/CMakeLists.txt
src/slic3r/GUI/GUI_App.cpp
src/slic3r/GUI/Preferences.hpp
version.inc
```

- [ ] **Step 2: Resolve each conflict with the rule below**

| File | Resolution |
|---|---|
| `.github/ISSUE_TEMPLATE/bug_report.yml` | `git checkout --ours` (fork's template) |
| `README.md` | `git checkout --ours` (rewritten in Stage 3) |
| `.github/workflows/build_orca.yml` | `git checkout --theirs`. Stage 3 re-applies the fork's rebranding on top of upstream's multi-arch workflow. Release CI is knowingly broken until Task 3.1. |
| `build_release_vs.bat` | `git checkout --theirs`, then re-apply the fork hunk shown by `git diff main mcp -- build_release_vs.bat` (if it is only the app name, it is superseded by `version.inc` and can be dropped). |
| `resources/web/data/text.js` | `git checkout --theirs`, then re-apply the fork's `homepage_connectai` strings from `git diff main mcp -- resources/web/data/text.js`. |
| `version.inc` | Take upstream's file, then set `SLIC3R_APP_NAME "OrcaMCP"`, `SLIC3R_APP_KEY "OrcaMCP"`, `SoftFever_VERSION "2.5.0.1-dev"`. Keep upstream's `SLIC3R_VERSION "02.08.01.55"`. |
| `CMakeLists.txt` | Take upstream's, then re-apply two fork hunks: (a) the `file(COPY ... scripts/orcamcp-bridge.py ... scripts/tools_schema.py DESTINATION "${SLIC3R_RESOURCES_DIR}/scripts")` block at the old lines 76-80; (b) the CPack block, rebranded on top of upstream's arch-suffixed version (see Step 3). |
| `src/CMakeLists.txt` | Take upstream's, then re-apply the fork hunks: `OUTPUT_NAME "orca-mcp"` for `OrcaSlicer` and `OrcaSlicer_app_gui`, `set(SLIC3R_APP_CMD "orca-mcp")`, `ln -sf OrcaSlicer orca-mcp`, `MACOSX_BUNDLE_BUNDLE_NAME "OrcaMCP"`, and the two `install(DIRECTORY ${CMAKE_SOURCE_DIR}/scripts/ ... FILES_MATCHING PATTERN "*.py" PATTERN "*.json")` rules. Use `git diff main mcp -- src/CMakeLists.txt` as the source of truth. |
| `src/slic3r/GUI/GUI_App.cpp` | Take upstream's, then re-apply the four MCP hunks from `git diff main mcp -- src/slic3r/GUI/GUI_App.cpp`: (1) includes of `OrcaMCP/OrcaMCPServer.hpp` and `OrcaMCP/MCPClientConfig.hpp`; (2) in `post_init()`: `start_http_server();` followed by best-effort `MCPClientConfig::ensure_bridge_script_copied(bridge_error);`; (3) the `homepage_connectai` command in `handle_web_request`; (4) `start_http_server()` routing `/mcp` to `OrcaMCPServer::handle_request(method, url, body)` guarded by `if (!m_http_server.is_started())`. |
| `src/slic3r/GUI/Preferences.hpp` | Take upstream's, then re-apply the fork members from `git diff main mcp -- src/slic3r/GUI/Preferences.hpp`: `#include "Widgets/Button.hpp"`, the `PreferencesDialog(wxWindow*, size_t open_on_tab = 0, const std::string& highlight_option = {})` constructor, `m_initial_tab`, `m_highlight_option`, `struct MCPClientUIElements`, `m_mcp_client_ui`, `create_mcp_clients_page(wxFlexGridSizer*)`, `refresh_mcp_client_buttons()`. |

- [ ] **Step 3: Re-apply the CPack block in `CMakeLists.txt` with upstream's arch suffix**

Upstream's block now appends `_x64`/`_arm64` to `CPACK_PACKAGE_FILE_NAME`. Keep that suffix variable and rebrand the rest:

```cmake
set (CPACK_PACKAGE_NAME "OrcaMCP")
set (CPACK_PACKAGE_VENDOR "OrcaMCP")
set (CPACK_PACKAGE_FILE_NAME "OrcaMCP_Windows_Installer_V${SoftFever_VERSION}${ORCA_WIN_ARCH_SUFFIX}")
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "OrcaMCP - AI-powered 3D slicer with MCP support")
set (CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/okets/OrcaMCP")
set (CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/resources/images\\\\OrcaSlicer.ico")
set (CPACK_NSIS_INSTALLED_ICON_NAME  "$INSTDIR\\\\orca-mcp.exe")
set (CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
    CreateShortCut \\\"$DESKTOP\\\\OrcaMCP.lnk\\\" \\\"$INSTDIR\\\\orca-mcp.exe\\\"
")
set (CPACK_PACKAGE_INSTALL_REGISTRY_KEY "OrcaMCP")
set (CPACK_PACKAGE_EXECUTABLES "orca-mcp;OrcaMCP")
set (CPACK_CREATE_DESKTOP_LINKS "orca-mcp")
set (CPACK_WIX_UPGRADE_GUID "058245e8-20e0-4a95-9ab7-1acfe17ad511")
set (CPACK_GENERATOR NSIS)
```

`ORCA_WIN_ARCH_SUFFIX` is whatever variable name upstream uses for the suffix in its version of the block. Read upstream's block (`git show upstream/main:CMakeLists.txt | grep -n CPACK_PACKAGE_FILE_NAME`) and use the same expression verbatim.

- [ ] **Step 4: Finish the merge commit**

```bash
git add -A
git diff --cached --name-only --diff-filter=U   # must print nothing
git commit -m "Merge upstream OrcaSlicer 2.5.0-dev into mcp

Brings Flashforge Creator 5 / 5 Pro profiles and the color mixing feature.
Fork rebranding re-applied in CMake, GUI_App and Preferences; CI workflows
taken from upstream and re-branded in a later commit.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

- [ ] **Step 5: Rebuild deps if upstream changed them, then build the slicer**

```bash
git diff --stat a3f229f406 upstream/main -- deps/ | tail -1
# If any deps/ file changed: rebuild deps first (long; run in background).
./build_release_macos.sh -d -x -j 8
# Then the slicer:
./build_release_macos.sh -s -x -j 8
```

Expected: `build/arm64/OrcaSlicer/OrcaSlicer.app` exists and is newer than the merge commit.

- [ ] **Step 6: Fix compile errors from the auto-merged files**

Most likely spots (auto-merged, not conflicted): `src/slic3r/GUI/HttpServer.{hpp,cpp}` (fork added `ResponseJson`, `get_method()`, POST body reading, `RequestHandlerFn`), `src/slic3r/GUI/Plater.cpp` (fork exposes `send_gcode_legacy`), `src/slic3r/GUI/NotificationManager.{hpp,cpp}` (`get_active_warnings`), `src/slic3r/GUI/MsgDialog.cpp` (suppression check in `ShowModal`), `src/slic3r/GUI/GUI.{hpp,cpp}`. For each error, compare `git diff main mcp -- <file>` with upstream's current shape and re-fit the fork hunk. Rebuild until clean.

- [ ] **Step 7: Smoke test the MCP server through the bridge**

```bash
export ORCAMCP_APP_PATH=/Users/hanan/Projects/OrcaMCP/build/arm64/OrcaSlicer/OrcaSlicer.app
```

Then, using the MCP tools: call `mcp__orca-slicer__start_orca`, wait for the window, call `mcp__orca-slicer__get_server_info` (expect `version` containing `2.5.0.1-dev`), `mcp__orca-slicer__get_scene_info` (expect `status: success`), and `mcp__orca-slicer__get_presets` with `type: "printer"` and confirm `Flashforge Creator 5 Pro 0.4 nozzle` appears in the list.

- [ ] **Step 8: Commit any compile fixes**

```bash
git add -A
git commit -m "Fix fork hunks after upstream 2.5.0-dev merge

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 1.2: Extract shared MCP helpers into `OrcaMCPCommon.hpp` and add an RAII dialog-suppression guard

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` (remove the file-static definitions of `run_on_main_thread`, `get_active_warnings_json`, `add_turntable_preview_if_requested`; include the new header)
- Modify: `src/slic3r/CMakeLists.txt` (add the header to `SLIC3R_GUI_SOURCES`)

**Interfaces:**
- Produces:
  - `template<typename Func> nlohmann::json run_on_main_thread(Func&& func)` (unchanged semantics: blocks the HTTP thread, rethrows exceptions).
  - `nlohmann::json get_active_warnings_json(Slic3r::GUI::Plater* plater)`.
  - `void add_turntable_preview_if_requested(nlohmann::json& result, bool include_preview, int view_count = 4, int resolution = 256)`.
  - `struct McpDialogSuppressionGuard` — constructor calls `set_mcp_dialog_suppression(true)` and `clear_mcp_suppressed_messages()`; `messages()` returns `get_mcp_suppressed_messages()`; destructor calls `set_mcp_dialog_suppression(false)`.

- [ ] **Step 1: Create the header**

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp
#pragma once
#include <future>
#include <functional>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/NotificationManager.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Runs `func` on the wx main thread and blocks the calling HTTP worker until it returns.
// `func` must return nlohmann::json. Exceptions propagate to the caller.
template<typename Func>
nlohmann::json run_on_main_thread(Func&& func)
{
    std::promise<nlohmann::json> promise;
    auto future = promise.get_future();
    wxGetApp().CallAfter([&promise, func = std::forward<Func>(func)]() {
        try {
            promise.set_value(func());
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    return future.get();
}

// Always returns {"count": N, "warnings": [{level, message, type}...]}.
nlohmann::json get_active_warnings_json(Plater* plater);

void add_turntable_preview_if_requested(nlohmann::json& result, bool include_preview, int view_count = 4, int resolution = 256);

// RAII: suppress modal dialogs for the lifetime of the guard and collect their messages.
struct McpDialogSuppressionGuard
{
    McpDialogSuppressionGuard()  { clear_mcp_suppressed_messages(); set_mcp_dialog_suppression(true); }
    ~McpDialogSuppressionGuard() { set_mcp_dialog_suppression(false); }
    std::vector<std::string> messages() const { return get_mcp_suppressed_messages(); }
};

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 2: Move the two non-template definitions**

Cut `get_active_warnings_json` (OrcaMCPServer.cpp ~251-273) and `add_turntable_preview_if_requested` (~275-287) out of `OrcaMCPServer.cpp` into a new `OrcaMCPCommon.cpp` under the same namespace, delete the file-static `run_on_main_thread` (~232-248), and add `#include "OrcaMCPCommon.hpp"` plus `using namespace Slic3r::GUI::OrcaMCP;` near the top of `OrcaMCPServer.cpp`. Add `GUI/OrcaMCP/OrcaMCPCommon.hpp` and `GUI/OrcaMCP/OrcaMCPCommon.cpp` to `SLIC3R_GUI_SOURCES` in `src/slic3r/CMakeLists.txt` next to the other OrcaMCP entries.

- [ ] **Step 3: Build and re-run the smoke test from Task 1.1 Step 7**

```bash
./build_release_macos.sh -s -x -j 8
```

Expected: clean build; `get_scene_info` still returns `active_warnings.count`.

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPCommon.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp src/slic3r/CMakeLists.txt
git commit -m "OrcaMCP: move shared helpers to OrcaMCPCommon, add dialog suppression guard

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 1.3: Make `apply_config` honest and add `project` preset support

Upstream's multi-material keys live in three places: printer preset (`nozzle_diameter`, `extruder_offset`, `retract_length_toolchange`, `retract_restart_extra_toolchange`, `machine_tool_change_time`, `change_filament_gcode`, `single_extruder_multi_material`, `physical_extruder_map`, `master_extruder_id`, `purge_in_prime_tower`, `wipe_tower_type`), print preset (`enable_prime_tower`, `prime_tower_width`, `prime_volume`, `wipe_tower_filament`, `toolchange_ordering`, `enable_mixed_color_sublayer`), and **project config** (`filament_map`, `filament_map_mode`, `flush_volumes_matrix`, `flush_multiplier`, `filament_colour`, the seven `filament_mixed_*` arrays). `apply_config` today only reaches the first two through `Tab::get_config()`, and silently reports success on invalid keys.

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp`, `.cpp` (`ApplyConfig` ~130-161, `UpdatePresetTabs` ~118-128)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` (`apply_config` ~977-1028; `get_valid_config_keys` ~1610-1729)

**Interfaces:**
- Produces:
  ```cpp
  struct ApplyConfigResult { std::vector<std::string> applied; std::vector<std::string> invalid; std::string error; };
  ApplyConfigResult OrcaMCPPresetConfigUtils::ApplyConfig(const nlohmann::json& item); // item = {"type": "print"|"filament"|"printer"|"project", "settings": {...}, "filament_index"?: int}
  void OrcaMCPPresetConfigUtils::RefreshAfterProjectConfigChange();
  ```
- `apply_config` tool response gains `applied_keys`, `invalid_keys`, and `status` = `success` | `partial` | `error`.
- `get_valid_config_keys` gains `category: "toolchanger"` and `category: "project"`; enum values are emitted when `enum_keys_map` **is** set (bug fix).

- [ ] **Step 1: Change `ApplyConfig` to return a result and support `project`**

```cpp
// OrcaMCPPresetConfigUtils.cpp
ApplyConfigResult OrcaMCPPresetConfigUtils::ApplyConfig(const nlohmann::json& item)
{
    ApplyConfigResult result;
    const std::string type = item.value("type", "");
    DynamicPrintConfig* config = nullptr;
    if (type == "project") {
        config = &wxGetApp().preset_bundle->project_config;
    } else {
        Preset::Type preset_type = PresetTypeFromString(type);   // existing helper in this file
        if (preset_type == Preset::TYPE_INVALID) { result.error = "Unknown preset type: " + type; return result; }
        Tab* tab = wxGetApp().get_tab(preset_type);
        if (!tab) { result.error = "No tab for preset type: " + type; return result; }
        config = tab->get_config();
    }
    ConfigSubstitutionContext context(ForwardCompatibilitySubstitutionRule::Enable);
    for (auto& [key, value] : item.at("settings").items()) {
        const std::string value_str = value.is_string() ? value.get<std::string>() : value.dump();
        if (print_config_def.get(key) == nullptr) { result.invalid.push_back(key); continue; }
        try {
            config->set_deserialize(key, value_str, context);
            result.applied.push_back(key);
        } catch (const std::exception&) {
            result.invalid.push_back(key);
        }
    }
    if (type == "project") RefreshAfterProjectConfigChange();
    return result;
}

void OrcaMCPPresetConfigUtils::RefreshAfterProjectConfigChange()
{
    Plater* plater = wxGetApp().plater();
    plater->update_filament_colors_in_full_config();
    wxGetApp().sidebar().update_dynamic_filament_list();
    wxGetApp().sidebar().update_mixed_filament_list();
    plater->update_project_dirty_from_presets();
    wxPostEvent(&wxGetApp().sidebar(), SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, &wxGetApp().sidebar()));
}
```

If `update_mixed_filament_list` or `update_dynamic_filament_list` are private on `Sidebar` (check `src/slic3r/GUI/Plater.hpp`), make them public: they are already called across the file and have no invariants beyond being on the main thread.

- [ ] **Step 2: Update the `apply_config` tool to surface the result**

In the loop over `configs` in `OrcaMCPServer.cpp`, collect `applied`/`invalid`/`error` per item; set `status` to `"error"` if any `error` is non-empty, `"partial"` if any `invalid` is non-empty, else `"success"`; return `applied_keys`, `invalid_keys`, `info_messages` (from `McpDialogSuppressionGuard::messages()`), and `active_warnings`. Extend the tool's `type` property description to `"print | filament | printer | project"`.

- [ ] **Step 3: Add the `toolchanger` and `project` categories to `get_valid_config_keys` and fix the enum bug**

```cpp
static const std::set<std::string> toolchanger_keys = {
    "nozzle_diameter", "extruder_offset", "extruder_colour", "extruder_type",
    "retract_length_toolchange", "retract_restart_extra_toolchange", "machine_tool_change_time",
    "change_filament_gcode", "single_extruder_multi_material", "manual_filament_change",
    "physical_extruder_map", "master_extruder_id", "printer_extruder_id",
    "purge_in_prime_tower", "wipe_tower_type", "enable_filament_ramming",
    "enable_prime_tower", "prime_tower_width", "prime_volume", "wipe_tower_filament", "toolchange_ordering",
    "prime_tower_brim_width", "prime_tower_skip_points", "wipe_tower_no_sparse_layers",
    "enable_mixed_color_sublayer", "filament_toolchange_delay", "filament_prime_volume", "filament_change_length"
};
static const std::set<std::string> project_keys = {
    "filament_colour", "filament_map", "filament_map_mode", "filament_nozzle_map", "filament_volume_map",
    "flush_volumes_matrix", "flush_volumes_vector", "flush_multiplier", "flush_multiplier_fast", "prime_volume_mode",
    "wipe_tower_x", "wipe_tower_y",
    "filament_is_mixed", "filament_mixed_components", "filament_mixed_sublayer_ratios",
    "filament_mixed_gradient", "filament_mixed_gradient_range", "filament_mixed_gradient_curve", "filament_mixed_gradient_per_part"
};
```

Route `category == "toolchanger"` / `"project"` through these sets, and change the enum condition at the existing `if (!opt_def.enum_keys_map)` to `if (opt_def.enum_keys_map)`. Also add `"extruder"` to the hardcoded `per_object_keys` set if it is missing.

- [ ] **Step 4: Build, then verify live**

Using MCP tools:
1. `apply_config` with `configs: [{"type":"print","settings":{"no_such_key":"1","enable_prime_tower":"1"}}]` → expect `status: "partial"`, `invalid_keys: ["no_such_key"]`, `applied_keys: ["enable_prime_tower"]`.
2. `get_valid_config_keys` with `category: "toolchanger"` → expect `retract_length_toolchange` present with `type: "floats"`.
3. `get_valid_config_keys` with `category: "project"` → expect `filament_map_mode` with non-empty `enum_values`.
4. Select printer preset `Flashforge Creator 5 Pro 0.4 nozzle` via `select_preset`, then `apply_config` `{"type":"project","settings":{"filament_map_mode":"Manual","filament_map":"1,2,3,4"}}` → `status: "success"`.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp
git commit -m "OrcaMCP: apply_config reports invalid keys, supports project config; toolchanger key category

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 1.4: Schema regeneration script and pytest harness

**Files:**
- Create: `scripts/regen_tools_schema.py`
- Create: `scripts/tests/test_tools_schema.py`, `scripts/tests/__init__.py` (empty)
- Regenerate: `scripts/tools_schema.py`

**Interfaces:**
- Produces: `python3 scripts/regen_tools_schema.py` rewrites `scripts/tools_schema.py` from the live server; `python3 -m pytest scripts/tests -q` passes offline (static checks) and additionally checks drift when the server is reachable.

- [ ] **Step 1: Write the failing test**

```python
# scripts/tests/test_tools_schema.py
import json, os, sys, urllib.request, importlib
import pytest

SCRIPTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, SCRIPTS)

def _load_static():
    mod = importlib.import_module("tools_schema")
    importlib.reload(mod)
    return mod.FULL_TOOLS_LIST

def _server_tools():
    url = f"http://{os.environ.get('ORCAMCP_HOST','localhost')}:{os.environ.get('ORCAMCP_PORT','13618')}/mcp"
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=2) as resp:
            return json.load(resp)["result"]["tools"]
    except Exception:
        return None

def test_static_list_has_required_fields():
    tools = _load_static()
    assert len(tools) >= 50
    names = [t["name"] for t in tools]
    assert len(names) == len(set(names)), "duplicate tool names"
    for t in tools:
        assert t["description"], t["name"]
        assert t["inputSchema"]["type"] == "object", t["name"]
        assert "properties" in t["inputSchema"], t["name"]

def test_static_list_matches_running_server():
    live = _server_tools()
    if live is None:
        pytest.skip("OrcaMCP server not reachable")
    static = {t["name"]: t for t in _load_static()}
    live_map = {t["name"]: t for t in live}
    assert set(static) == set(live_map), f"drift: run scripts/regen_tools_schema.py; diff={set(static) ^ set(live_map)}"
    for name, tool in live_map.items():
        assert static[name]["inputSchema"] == tool["inputSchema"], name
```

- [ ] **Step 2: Run it to see it fail on drift**

```bash
python3 -m pytest scripts/tests -q
```

Expected with OrcaMCP running: `test_static_list_matches_running_server` FAILS (the static list is already missing `export_3mf`, `get_preview_base64`, `set_gcode_view_type`, `set_object_printable`).

- [ ] **Step 3: Write the regeneration script**

```python
#!/usr/bin/env python3
"""Regenerate scripts/tools_schema.py from the running OrcaMCP server."""
import json, os, sys, urllib.request

HOST = os.environ.get("ORCAMCP_HOST", "localhost")
PORT = os.environ.get("ORCAMCP_PORT", "13618")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools_schema.py")

def fetch_tools():
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}).encode()
    req = urllib.request.Request(f"http://{HOST}:{PORT}/mcp", data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.load(resp)["result"]["tools"]

def main():
    tools = sorted(fetch_tools(), key=lambda t: t["name"])
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("# Full tools list from OrcaMCP server\n")
        f.write("# This ensures schemas are always correct and match the server\n")
        f.write("# Auto-generated by scripts/regen_tools_schema.py - do not edit by hand\n\n")
        f.write("FULL_TOOLS_LIST = ")
        f.write(json.dumps(tools, indent=4, ensure_ascii=False).replace(": true", ": True").replace(": false", ": False").replace(": null", ": None"))
        f.write("\n")
    print(f"wrote {len(tools)} tools to {OUT}")

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Regenerate and re-run the tests**

```bash
python3 scripts/regen_tools_schema.py     # expect: wrote 53 tools (more after later tasks)
python3 -m pytest scripts/tests -q        # expect: 2 passed
python3 -c "import sys; sys.path.insert(0,'scripts'); import tools_schema; print(len(tools_schema.FULL_TOOLS_LIST))"
```

- [ ] **Step 5: Commit**

```bash
git add scripts/regen_tools_schema.py scripts/tests scripts/tools_schema.py
git commit -m "OrcaMCP: tools_schema regeneration script and pytest drift check

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 1.5: Extract `Sidebar::apply_mixed_filament` from the dialog paths

Upstream's add path is the file-static `create_mixed_filament_from_result` (`Plater.cpp` ~4630-4746) and the edit path is inline in `Sidebar::edit_mixed_filament` (~4768-4924). Both are driven by `MixedFilamentDialog`. MCP needs the same logic without a dialog.

**Files:**
- Modify: `src/slic3r/GUI/Plater.hpp` (Sidebar public section), `src/slic3r/GUI/Plater.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // Returns the 0-based filament config index of the created/edited mixed slot, or -1 with `error` set.
  int Sidebar::apply_mixed_filament(const MixedFilamentResult& result, int edit_cfg_idx, std::string& error);
  ```
  `MixedFilamentResult` is upstream's struct (`MixedFilamentDialog.hpp:33-43`): `components` (1-based physical indices), `ratios` (percent, sum 100), `gradient_enabled`, `gradient_direction` (0 = A→B, 1 = B→A), `per_part_gradient`, `gradient_curve`.

- [ ] **Step 1: Refactor**

Move the body of `create_mixed_filament_from_result` into `Sidebar::apply_mixed_filament` with `edit_cfg_idx == -1`, and move the in-place write block of `edit_mixed_filament` (the part after the dialog returns, ~4860-4924) into the `edit_cfg_idx >= 0` branch. Validation that previously returned silently must now set `error`:
- fewer than 2 physical filaments → `"At least two physical filaments are required"`
- slot count would exceed `EnforcerBlockerType::ExtruderMax` → `"Maximum filament slot count reached"`
- component out of range or pointing at a mixed slot → `"Component N is not a physical filament"`
- gradient with != 2 components → `"Gradient mixing requires exactly two components"`

Keep the two existing refresh sequences exactly as they are (add: `on_filament_added`, `on_filament_count_change`, print tab `update`, `export_selections`, `update_mixed_filament_list`, `update_project_dirty_from_presets`, `EVT_SCHEDULE_BACKGROUND_PROCESS`; edit: `update_mixed_filament_list`, `update_dynamic_filament_list`, `update_project_dirty_from_presets`, `EVT_SCHEDULE_BACKGROUND_PROCESS`). `add_mixed_filament()` and `edit_mixed_filament()` become: open dialog → on OK call `apply_mixed_filament(result, idx, err)` → `show_error` if `err` non-empty.

- [ ] **Step 2: Build and verify the GUI path still works**

Open OrcaSlicer, select the Creator 5 Pro printer, add two filaments, open the sidebar "Mixed Filaments" add dialog, create a 70/30 mix. Expect a fifth slot with a blended color, no crash, slice button enabled.

- [ ] **Step 3: Commit**

```bash
git add src/slic3r/GUI/Plater.hpp src/slic3r/GUI/Plater.cpp
git commit -m "Sidebar: extract apply_mixed_filament from mixed filament dialog paths

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 1.6: Filament and mixed-filament MCP tools

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.hpp`, `.cpp`
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp` (declare `static void register_filament_tools();`), `OrcaMCPServer.cpp` (call it at the end of `register_builtin_tools()`), `src/slic3r/CMakeLists.txt`

**Interfaces:**
- Consumes: `Sidebar::apply_mixed_filament` (Task 1.5); `OrcaMCPPresetConfigUtils::RefreshAfterProjectConfigChange` (Task 1.3); upstream accessors `PresetBundle::get_printer_extruder_count()`, `num_physical_filaments()`, `is_mixed_filament(idx)`, `Plater::mixed_filament_config_indices()`, `Sidebar::delete_mixed_filament_at(panel_idx)`; `FilamentMixer.hpp` parsers `parse_mixed_components`, `parse_mixed_ratios`.
- Produces tools:

`get_filaments` (no params) →
```json
{
  "status": "success",
  "extruder_count": 4,
  "physical_count": 4,
  "mixed_sublayer_enabled": false,
  "filament_map_mode": "Auto For Flush",
  "filament_map": [1,2,3,4],
  "filaments": [
    {"slot": 1, "preset": "Flashforge PLA @FF C5P", "type": "PLA", "color": "#FF0000", "is_mixed": false},
    {"slot": 5, "preset": "Flashforge PLA @FF C5P", "type": "PLA", "color": "#B34D00", "is_mixed": true,
     "components": [1, 3], "ratios": [0.7, 0.3], "gradient": false, "gradient_range": null, "per_part_gradient": false}
  ],
  "active_warnings": {"count": 0, "warnings": []}
}
```

`set_mixed_filament` params: `components` (array of 1-based ints, 2 or 3), `ratios` (array of ints, percent, sum 100), optional `slot` (1-based existing mixed slot to edit), `gradient` (bool), `gradient_direction` (`"a_to_b"` | `"b_to_a"`), `per_part_gradient` (bool) → `{"status":"success","slot": 5, "filaments": [...same as get_filaments...]}` or `{"status":"error","message":...}`.

`delete_mixed_filament` params: `slot` (1-based) → `{"status":"success","filaments":[...]}`.

`set_object_filament` params: `object_id` (int), `filament` (1-based slot, may be a mixed slot), optional `volume_id` (int) → `{"status":"success","object_id":0,"volume_id":null,"filament":5}`.

- [ ] **Step 1: Write the utils header**

```cpp
// OrcaMCPFilamentUtils.hpp
#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include "slic3r/GUI/MixedFilamentDialog.hpp"   // MixedFilamentResult

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Snapshot of every filament slot (physical first, then mixed). Main thread only.
nlohmann::json describe_filaments();

// Builds a MixedFilamentResult from tool params; returns false and fills `error` on bad input.
bool mixed_result_from_params(const nlohmann::json& params, MixedFilamentResult& out, std::string& error);

// 1-based slot -> 0-based config index, validating range. Returns -1 and sets `error` when invalid.
int slot_to_config_index(int slot, std::string& error);

// Assigns a filament slot to an object or one of its volumes. Returns false and sets `error` on failure.
bool set_object_filament(int object_id, int volume_id /* -1 = object */, int slot, std::string& error);

}}}
```

- [ ] **Step 2: Implement the utils**

```cpp
// OrcaMCPFilamentUtils.cpp
#include "OrcaMCPFilamentUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

static std::string opt_str_at(const DynamicPrintConfig& cfg, const char* key, size_t idx)
{
    const auto* opt = cfg.option<ConfigOptionStrings>(key);
    return (opt && idx < opt->values.size()) ? opt->values[idx] : std::string();
}
static bool opt_bool_at(const DynamicPrintConfig& cfg, const char* key, size_t idx)
{
    const auto* opt = cfg.option<ConfigOptionBools>(key);
    return opt && idx < opt->values.size() && opt->values[idx];
}

nlohmann::json describe_filaments()
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    const DynamicPrintConfig& proj = pb->project_config;
    const DynamicPrintConfig& full = pb->full_config();
    nlohmann::json list = nlohmann::json::array();
    const size_t n = pb->filament_presets.size();
    for (size_t i = 0; i < n; ++i) {
        nlohmann::json f = {
            {"slot", int(i + 1)},
            {"preset", pb->filament_presets[i]},
            {"type", full.get_filament_type(std::string(), int(i))},
            {"color", opt_str_at(proj, "filament_colour", i)},
            {"is_mixed", pb->is_mixed_filament(i)}
        };
        if (pb->is_mixed_filament(i)) {
            f["components"] = parse_mixed_components(opt_str_at(proj, "filament_mixed_components", i));
            f["ratios"]     = parse_mixed_ratios(opt_str_at(proj, "filament_mixed_sublayer_ratios", i));
            f["gradient"]   = opt_bool_at(proj, "filament_mixed_gradient", i);
            const std::string range = opt_str_at(proj, "filament_mixed_gradient_range", i);
            f["gradient_range"] = range.empty() ? nlohmann::json(nullptr) : nlohmann::json(parse_mixed_ratios(range));
            f["per_part_gradient"] = opt_bool_at(proj, "filament_mixed_gradient_per_part", i);
        }
        list.push_back(f);
    }
    const auto* fmap = proj.option<ConfigOptionInts>("filament_map");
    nlohmann::json out = {
        {"extruder_count", pb->get_printer_extruder_count()},
        {"physical_count", int(pb->num_physical_filaments())},
        {"mixed_sublayer_enabled", full.opt_bool("enable_mixed_color_sublayer")},
        {"filament_map_mode", proj.opt_serialize("filament_map_mode")},
        {"filament_map", fmap ? nlohmann::json(fmap->values) : nlohmann::json::array()},
        {"filaments", list}
    };
    return out;
}

bool mixed_result_from_params(const nlohmann::json& params, MixedFilamentResult& out, std::string& error)
{
    if (!params.contains("components") || !params["components"].is_array() || params["components"].size() < 2 || params["components"].size() > 3) {
        error = "components must be an array of 2 or 3 physical filament slots (1-based)"; return false;
    }
    if (!params.contains("ratios") || !params["ratios"].is_array() || params["ratios"].size() != params["components"].size()) {
        error = "ratios must be an array of the same length as components, in percent"; return false;
    }
    int sum = 0;
    for (const auto& r : params["ratios"]) { if (!r.is_number()) { error = "ratios must be numbers"; return false; } sum += r.get<int>(); }
    if (sum != 100) { error = "ratios must sum to 100"; return false; }
    out.components.clear(); out.ratios.clear();
    for (const auto& c : params["components"]) out.components.push_back(c.get<unsigned int>());
    for (const auto& r : params["ratios"])     out.ratios.push_back(r.get<int>());
    out.gradient_enabled   = params.value("gradient", false);
    out.gradient_direction = params.value("gradient_direction", "a_to_b") == "b_to_a" ? 1 : 0;
    out.per_part_gradient  = params.value("per_part_gradient", false);
    if (out.gradient_enabled && out.components.size() != 2) { error = "gradient requires exactly two components"; return false; }
    return true;
}

int slot_to_config_index(int slot, std::string& error)
{
    const int n = int(wxGetApp().preset_bundle->filament_presets.size());
    if (slot < 1 || slot > n) { error = "slot out of range 1.." + std::to_string(n); return -1; }
    return slot - 1;
}

bool set_object_filament(int object_id, int volume_id, int slot, std::string& error)
{
    Plater* plater = wxGetApp().plater();
    Model& model = plater->model();
    if (object_id < 0 || object_id >= int(model.objects.size())) { error = "Invalid object_id"; return false; }
    if (slot_to_config_index(slot, error) < 0) return false;
    ModelObject* obj = model.objects[object_id];
    plater->take_snapshot(_u8L("Change Filaments"));
    if (volume_id < 0) {
        obj->config.set("extruder", slot);
    } else {
        if (volume_id >= int(obj->volumes.size())) { error = "Invalid volume_id"; return false; }
        ModelVolume* vol = obj->volumes[volume_id];
        if (!vol->is_model_part() && !vol->is_modifier()) { error = "Only model parts and modifiers accept a filament"; return false; }
        vol->config.set("extruder", slot);
    }
    wxGetApp().obj_list()->changed_object(object_id);
    wxGetApp().obj_list()->update_filament_colors();
    plater->update();
    return true;
}

}}}
```

- [ ] **Step 3: Register the tools**

```cpp
// OrcaMCPFilamentTools.cpp
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPFilamentUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

static nlohmann::json with_filaments(nlohmann::json result)
{
    result["filaments"] = describe_filaments()["filaments"];
    result["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
    return result;
}

void OrcaMCPServer::register_filament_tools()
{
    register_tool({
        "get_filaments",
        "List all filament slots (physical and mixed/virtual), extruder count, and filament-to-extruder map.",
        {{"type", "object"}, {"properties", nlohmann::json::object()}},
        [](const nlohmann::json&) -> nlohmann::json {
            return run_on_main_thread([]() {
                nlohmann::json r = describe_filaments();
                r["status"] = "success";
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });

    register_tool({
        "set_mixed_filament",
        "Create or edit a mixed (virtual) filament slot that alternates two or three physical filaments by layer ratio. Requires a multi-filament printer profile.",
        {
            {"type", "object"},
            {"properties", {
                {"components", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "2-3 physical filament slots, 1-based"}}},
                {"ratios", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Percent per component, must sum to 100"}}},
                {"slot", {{"type", "integer"}, {"description", "Existing mixed slot to edit (1-based). Omit to create."}}},
                {"gradient", {{"type", "boolean"}, {"description", "Z gradient between the two components"}}},
                {"gradient_direction", {{"type", "string"}, {"enum", {"a_to_b", "b_to_a"}}}},
                {"per_part_gradient", {{"type", "boolean"}}}
            }},
            {"required", {"components", "ratios"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            MixedFilamentResult req; std::string err;
            if (!mixed_result_from_params(params, req, err)) return {{"status", "error"}, {"message", err}};
            const int slot = params.value("slot", -1);
            return run_on_main_thread([req, slot]() -> nlohmann::json {
                std::string error;
                int edit_idx = -1;
                if (slot > 0) {
                    edit_idx = slot_to_config_index(slot, error);
                    if (edit_idx < 0) return {{"status", "error"}, {"message", error}};
                    if (!wxGetApp().preset_bundle->is_mixed_filament(edit_idx)) return {{"status", "error"}, {"message", "slot is not a mixed filament"}};
                }
                int idx = wxGetApp().sidebar().apply_mixed_filament(req, edit_idx, error);
                if (idx < 0) return {{"status", "error"}, {"message", error}};
                return with_filaments({{"status", "success"}, {"slot", idx + 1}});
            });
        }
    });

    register_tool({
        "delete_mixed_filament",
        "Delete a mixed (virtual) filament slot.",
        {{"type", "object"}, {"properties", {{"slot", {{"type", "integer"}, {"description", "Mixed slot, 1-based"}}}}}, {"required", {"slot"}}},
        [](const nlohmann::json& params) -> nlohmann::json {
            const int slot = params["slot"];
            return run_on_main_thread([slot]() -> nlohmann::json {
                std::string error;
                int idx = slot_to_config_index(slot, error);
                if (idx < 0) return {{"status", "error"}, {"message", error}};
                auto mixed = wxGetApp().plater()->mixed_filament_config_indices();
                auto it = std::find(mixed.begin(), mixed.end(), size_t(idx));
                if (it == mixed.end()) return {{"status", "error"}, {"message", "slot is not a mixed filament"}};
                wxGetApp().sidebar().delete_mixed_filament_at(size_t(it - mixed.begin()));
                return with_filaments({{"status", "success"}});
            });
        }
    });

    register_tool({
        "set_object_filament",
        "Assign a filament slot (physical or mixed) to an object, or to one part/modifier of it.",
        {
            {"type", "object"},
            {"properties", {
                {"object_id", {{"type", "integer"}}},
                {"filament", {{"type", "integer"}, {"description", "Filament slot, 1-based"}}},
                {"volume_id", {{"type", "integer"}, {"description", "Part index within the object; omit for the whole object"}}}
            }},
            {"required", {"object_id", "filament"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            const int object_id = params["object_id"], filament = params["filament"], volume_id = params.value("volume_id", -1);
            return run_on_main_thread([=]() -> nlohmann::json {
                std::string error;
                if (!set_object_filament(object_id, volume_id, filament, error)) return {{"status", "error"}, {"message", error}};
                nlohmann::json r = {{"status", "success"}, {"object_id", object_id}, {"filament", filament}};
                r["volume_id"] = volume_id < 0 ? nlohmann::json(nullptr) : nlohmann::json(volume_id);
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
        }
    });
}
```

If `delete_mixed_filament_at` or `apply_mixed_filament` are not public on `Sidebar`, make them public in `Plater.hpp`.

- [ ] **Step 4: Wire up, build, regenerate schema**

Add the three new files to `SLIC3R_GUI_SOURCES`; call `register_filament_tools();` as the last line of `register_builtin_tools()`. Build. Restart OrcaSlicer via `start_orca`. Run `python3 scripts/regen_tools_schema.py` and `python3 -m pytest scripts/tests -q`.

- [ ] **Step 5: Verify live with MCP tools**

1. `select_preset` printer `Flashforge Creator 5 Pro 0.4 nozzle`; `get_filaments` → `extruder_count: 4`, `physical_count: 4`.
2. `set_mixed_filament` `{"components":[1,2],"ratios":[70,30]}` → `status: success`, `slot: 5`; `get_filaments` shows slot 5 `is_mixed: true`, `ratios: [0.7,0.3]`.
3. `set_mixed_filament` `{"slot":5,"components":[1,2],"ratios":[50,50],"gradient":true}` → success; `get_filaments` slot 5 `gradient: true`.
4. `load_model` any STL; `set_object_filament` `{"object_id":0,"filament":5}` → success; `get_object_config` shows `extruder: "5"`.
5. `slice_all` → `status: success` (gradient without sublayer produces a warning in `active_warnings`, not an error).
6. `delete_mixed_filament` `{"slot":5}` → success; `get_filaments` has 4 slots.
7. Error path: `set_mixed_filament` `{"components":[1,9],"ratios":[50,50]}` → `status: error`.

- [ ] **Step 6: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP src/slic3r/CMakeLists.txt scripts/tools_schema.py
git commit -m "OrcaMCP: get_filaments, set_mixed_filament, delete_mixed_filament, set_object_filament tools

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 1.7: Flush-volume and toolchanger-config tools

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.hpp/.cpp`, `OrcaMCPFilamentTools.cpp`

**Interfaces:**
- Consumes: `get_flush_volumes_matrix` / `set_flush_volumes_matrix` templates (`PrintConfig.hpp` ~2406-2430), `Sidebar::auto_calc_flushing_volumes(int modify_id)`, `RefreshAfterProjectConfigChange`.
- Produces tools:

`get_flush_volumes` → `{"status":"success","extruder_count":4,"filament_count":4,"flush_multiplier":[0.3,0.3,0.3,0.3],"matrices":[{"extruder":0,"matrix":[[0,280,280,280],[280,0,280,280],...]}]}`

`set_flush_volumes` params: `matrix` (NxN array of arrays, N = filament_count), optional `extruder` (0-based, default 0), optional `flush_multiplier` (number) → `{"status":"success"}`.

`auto_calc_flush_volumes` (no params) → `{"status":"success"}` after recalculating every physical filament's row/column.

`get_toolchanger_config` → the current values of every key in the `toolchanger_keys` set from Task 1.3 plus `filament_map`, `filament_map_mode`, `extruder_count`, each serialized with `opt_serialize`, grouped as `{"printer": {...}, "print": {...}, "project": {...}}`. Writes go through `apply_config` (already supports all three groups after Task 1.3).

- [ ] **Step 1: Implement the flush helpers**

```cpp
nlohmann::json describe_flush_volumes()
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    const size_t extruders = size_t(pb->get_printer_extruder_count());
    const size_t n = pb->num_physical_filaments();
    const auto* mat = pb->project_config.option<ConfigOptionFloats>("flush_volumes_matrix");
    nlohmann::json matrices = nlohmann::json::array();
    for (size_t e = 0; e < extruders; ++e) {
        std::vector<double> block = get_flush_volumes_matrix(mat->values, e, extruders);
        const size_t side = size_t(std::sqrt(double(block.size())) + 0.001);
        nlohmann::json rows = nlohmann::json::array();
        for (size_t r = 0; r < side; ++r)
            rows.push_back(std::vector<double>(block.begin() + r * side, block.begin() + (r + 1) * side));
        matrices.push_back({{"extruder", int(e)}, {"matrix", rows}});
    }
    const auto* mult = pb->project_config.option<ConfigOptionFloats>("flush_multiplier");
    return {{"extruder_count", int(extruders)}, {"filament_count", int(n)},
            {"flush_multiplier", mult ? nlohmann::json(mult->values) : nlohmann::json::array()},
            {"matrices", matrices}};
}

bool set_flush_volumes(const nlohmann::json& matrix, int extruder, std::string& error)
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    const size_t extruders = size_t(pb->get_printer_extruder_count());
    if (extruder < 0 || size_t(extruder) >= extruders) { error = "extruder out of range"; return false; }
    auto* mat = pb->project_config.option<ConfigOptionFloats>("flush_volumes_matrix", true);
    const size_t side = size_t(std::sqrt(double(mat->values.size() / extruders)) + 0.001);
    if (!matrix.is_array() || matrix.size() != side) { error = "matrix must be " + std::to_string(side) + "x" + std::to_string(side); return false; }
    std::vector<double> block;
    for (const auto& row : matrix) {
        if (!row.is_array() || row.size() != side) { error = "matrix rows must have " + std::to_string(side) + " entries"; return false; }
        for (const auto& v : row) block.push_back(v.get<double>());
    }
    set_flush_volumes_matrix(mat->values, block, size_t(extruder), extruders);
    OrcaMCPPresetConfigUtils::RefreshAfterProjectConfigChange();
    return true;
}
```

- [ ] **Step 2: Register the four tools** in `register_filament_tools()` following the same pattern as Task 1.6 (params on HTTP thread, body in `run_on_main_thread`). `auto_calc_flush_volumes` loops `for (int i = 0; i < physical_count; ++i) wxGetApp().sidebar().auto_calc_flushing_volumes(i);` then calls `RefreshAfterProjectConfigChange()`. `get_toolchanger_config` reads `wxGetApp().preset_bundle->printers.get_edited_preset().config`, `prints.get_edited_preset().config`, and `project_config`, and emits `opt_serialize(key)` for each key that `config.has(key)`.

- [ ] **Step 3: Build, regenerate schema, verify live**

1. `get_flush_volumes` on the Creator 5 Pro profile → 4 matrices of 4x4 (or one, if upstream stores a single block for `single_extruder_multi_material = 0` printers; report what you see and keep the response shape).
2. `set_flush_volumes` with a 4x4 matrix of 100s off-diagonal → success; `get_flush_volumes` reflects it.
3. `auto_calc_flush_volumes` → success; values change from 100.
4. `get_toolchanger_config` → `printer.retract_length_toolchange == "3,3,3,3"`, `printer.machine_tool_change_time == "7"`, `print.enable_prime_tower` present.
5. `apply_config` `{"type":"printer","settings":{"retract_length_toolchange":"2,2,2,2"}}` → success; `get_toolchanger_config` reflects it.

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP scripts/tools_schema.py
git commit -m "OrcaMCP: flush volume and toolchanger config tools

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 1.7b: Color recipe tools (suggest a mix for a target color, enumerate a palette)

Upstream already ships the engine: `src/libslic3r/ColorDecomposeRecipe.hpp` (`recommend_from_physical_filaments(target_rgb, physical_filaments, preferred_material_type)` → `ColorDecomposeRecipeResult{valid, matched_color_hex, components[{color_hex, ratio, filament_index}]}`, `lookup_measured_blend_color(hexes, ratios)`, `color_decompose_hex_to_rgb`) and `src/libslic3r/FilamentMixer.hpp` (`blend_color(hex_a, hex_b, ratio_b)`, `blend_color_multi(hexes, ratios)`). The GUI's "Mixing Recommendations" grid in `MixedFilamentDialog::rebuild_recommendation_items()` enumerates same-type pairs/triples; reuse its enumeration rules, not its widgets.

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.hpp/.cpp` (add `physical_filaments_for_recipe()`, `predicted_mix_color(components, ratios)`, `enumerate_mix_palette(max_count, max_components)`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp` (register two tools)

**Interfaces:**
- Consumes: Task 1.6 `describe_filaments()`, `mixed_result_from_params()`, `Sidebar::apply_mixed_filament`.
- Produces tools:

`suggest_color_mix` params: `target_color` (hex `#RRGGBB`), optional `material_type` (e.g. `PLA`; default = type of filament 1), optional `create` (bool, default false) →
```json
{"status":"success","target_color":"#8040C0","recipe":{"components":[1,2],"ratios":[60,40],"predicted_color":"#7E3FBE","measured":false},
 "delta_e":3.2,"slot":null}
```
`delta_e` = CIE76 distance in Lab between target and predicted (compute in utils using the same rgb→Lab conversion; expose a small helper). With `create: true`, the recipe is applied through `apply_mixed_filament` and `slot` is the new 1-based slot. `status: "error"` with a message when no same-type pair exists or the recipe is invalid.

`get_color_palette` params: optional `max_count` (default 12, cap 48), optional `max_components` (2 or 3, default 2), optional `material_type` →
```json
{"status":"success","palette":[{"components":[1,3],"ratios":[70,30],"predicted_color":"#C86A3C","measured":true}, ...]}
```
Enumerate every same-type pair (and triple when `max_components == 3`) of physical filaments at ratios {70/30, 50/50, 30/70} (triples: one dominant at 50/25/25 per component), predict each color with `lookup_measured_blend_color` first and `blend_color(_multi)` as fallback, drop candidates within delta_e < 5 of a physical filament or of an earlier candidate, sort by hue, truncate to `max_count`. This is the "10-12 mixes to choose from" list an agent can show a user before painting.

- [ ] **Step 1: Implement the three utils** (pure, main-thread only for reading the preset bundle), with the Lab helper factored so both tools share it.
- [ ] **Step 2: Register the two tools** following the Task 1.6 pattern; `suggest_color_mix` with `create: true` reuses `mixed_result_from_params` + `apply_mixed_filament`.
- [ ] **Step 3: Build, regenerate schema, verify live** on the Creator 5 Pro profile with four PLA slots loaded (magenta, blue, yellow, grey as read from the printer's material station): `suggest_color_mix {"target_color":"#800080"}` → components include the magenta and blue slots, delta_e reported; `get_color_palette {}` → between 6 and 12 distinct entries with predicted colors; `suggest_color_mix {"target_color":"#40FF40","create":true}` → a new slot appears in `get_filaments`.
- [ ] **Step 4: Commit** `OrcaMCP: suggest_color_mix and get_color_palette tools`.

---

### Task 1.8: Four-tool end-to-end slice check and Stage 1 wrap-up

- [ ] **Step 1: Run the multi-material workflow entirely through MCP**

`new_project` → `select_preset` printer `Flashforge Creator 5 Pro 0.4 nozzle` → `get_filaments` (4 slots) → `load_model` an STL → `clone_object` three times → `set_object_filament` objects 0..3 to slots 1..4 → `apply_config` `{"type":"print","settings":{"enable_prime_tower":"1"}}` → `arrange_objects` → `slice_all` → `get_print_estimate`. Expect `status: success` and a non-zero `total_toolchanges`-style count if `get_print_estimate` exposes it; otherwise confirm the G-code preview shows four colors via `render_plate_view`.

- [ ] **Step 2: Export the G-code and check toolchange lines**

`export_gcode` with `output_path` in the scratchpad, then:

```bash
grep -c -E "^T[0-3]" <exported>.gcode     # expect > 3
grep -m3 -E "^T[0-3]" <exported>.gcode
```

Stock Creator 5 firmware intercepts bare `Tn` lines (see `Monstrofil/creator5-toolchange`), so bare `T1` is what the upstream profile intends. Record the result in the commit message.

- [ ] **Step 3: Merge the sync branch into `mcp` and push**

```bash
git checkout mcp
git merge --ff-only sync-upstream-2.5
git push origin mcp
```

Note: `build_all.yml` will run on push and is expected to fail until Task 3.1. That is acceptable for this stage.

---

# Stage 2: Flashforge Creator 5 Pro integration

### Task 2.1: Correct the Creator 5 profiles' host type

**Files:**
- Modify: `resources/profiles/Flashforge/machine/Flashforge Creator 5.json`, `Flashforge Creator 5 Pro.json`, and the six nozzle variants (`0.4`, `0.6`, `0.8` for each).

- [ ] **Step 1: Change `host_type`**

```bash
cd resources/profiles/Flashforge/machine
grep -l '"host_type": "octoprint"' "Flashforge Creator 5"*.json
sed -i '' 's/"host_type": "octoprint"/"host_type": "flashforge"/' "Flashforge Creator 5"*.json
grep -H '"host_type"' "Flashforge Creator 5"*.json
```

Expected: every Creator 5 file now says `flashforge`. Leave `printhost_authorization_type: "key"` as is (the "API key" field holds the check code).

- [ ] **Step 2: Verify the profile validator still passes and the GUI shows the right physical-printer fields**

```bash
./build/arm64/OrcaSlicer/OrcaSlicer_profile_validator.app/Contents/MacOS/OrcaSlicer_profile_validator -p resources/profiles -v Flashforge
```

Expected: exit 0. Then in the GUI: Printer → Add physical printer → host type pre-selected "Flashforge", fields "Serial Number" and "API Key / Password" visible.

- [ ] **Step 3: Commit, and open an upstream PR with the same one-line change**

```bash
git add resources/profiles/Flashforge/machine
git commit -m "Flashforge Creator 5: default host_type to flashforge (local API on port 8898)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

Upstream PR is optional but cheap: same diff against `OrcaSlicer/OrcaSlicer`.

---

### Task 2.2: Pure Flashforge API helpers with Catch2 tests

The printer's HTTP API (confirmed by upstream `Flashforge.cpp` and the MIT `ff-5mp-api-py` client, which lists Creator 5 / 5 Pro as supported):

| Endpoint | Body | Notes |
|---|---|---|
| `POST /detail` | `{"serialNumber","checkCode"}` | Response `{"code":0,"message":"Success","detail":{...}}` |
| `POST /control` | `{"serialNumber","checkCode","payload":{"cmd":<cmd>,"args":{...}}}` | `code` 0 on success |
| `POST /gcodeList` | `{"serialNumber","checkCode"}` | `gcodeList: [names]` |
| `POST /gcodeThumb` | `{"serialNumber","checkCode","fileName"}` | base64 image |
| `POST /printGcode` | `{"serialNumber","checkCode","fileName","levelingBeforePrint","flowCalibration":false,"useMatlStation","gcodeToolCnt","materialMappings":[...]}` | starts a file already on the printer |
| `POST /uploadGcode` | multipart `gcodeFile` + headers `serialNumber, checkCode, fileSize, printNow, levelingBeforePrint, flowCalibration, firstLayerInspection, timeLapseVideo, useMatlStation, gcodeToolCnt, materialMappings(base64 JSON)` | already implemented upstream |

Control commands and args:

| `cmd` | `args` |
|---|---|
| `jobCtl_cmd` | `{"jobID":"","action":"pause"|"continue"|"cancel"}` |
| `lightControl_cmd` | `{"status":"open"|"close"}` |
| `temperatureCtl_cmd` | `{"platform":T,"rightNozzle":T,"leftNozzle":T,"chamber":T,"nozzles":[T0,T1,T2,T3]}`; `-200` means "no change"; `nozzles` is the Creator 5 per-tool array |
| `printerCtl_cmd` | `{"zAxisCompensation":z,"speed":pct,"chamberFan":pct,"coolingFan":pct,"coolingLeftFan":0}` |
| `streamCtrl_cmd` | `{"action":"open"|"close"}` |
| `circulateCtl_cmd` | `{"internal":"open"|"close","external":"open"|"close"}` |

`/detail` fields the parser must read (camelCase, in `detail`): `status` (`ready|busy|heating|printing|paused|completed|error|cancel`), `printFileName`, `printProgress` (0..1), `printDuration` (s), `estimatedTime` (s), `platTemp`, `platTargetTemp`, `chamberTemp`, `chamberTargetTemp`, `nozzleTemps[]`, `nozzleTargetTemps[]`, `nozzleCnt`, `lightStatus` (`open|close`), `doorStatus`, `errorCode`, `firmwareVersion`, `name`, `model`, `pid` (40 = Creator 5, 41 = Creator 5 Pro), `ipAddr`, `cameraStreamUrl`, `hasMatlStation`, `matlStationInfo.slotInfos[] {slotId, hasFilament, materialName, materialColor}`, `currentPrintSpeed`, `zAxisCompensation`, `cumulativePrintTime`, `cumulativeFilament`. Legacy single-nozzle printers report `rightTemp`/`rightTargetTemp` instead of the arrays; the parser falls back to those.

**Files:**
- Create: `src/slic3r/Utils/FlashforgeApi.hpp`, `.cpp`
- Create: `tests/slic3rutils/test_flashforge_api.cpp`; Modify: `tests/slic3rutils/CMakeLists.txt` (add the file to `add_executable(${_TEST_NAME}_tests ...)`)
- Modify: `src/libslic3r/CMakeLists.txt` or `src/slic3r/CMakeLists.txt` to compile `FlashforgeApi.cpp` next to `Flashforge.cpp`

**Interfaces:**
```cpp
namespace Slic3r { namespace FlashforgeApi {
struct NozzleTemp { double current{0}; double target{0}; };
struct MaterialSlot { int slot_id{0}; bool has_filament{false}; std::string material_name, material_color; };
struct PrinterStatus {
    std::string state;                 // normalized: ready|busy|heating|printing|paused|completed|error|cancelled|unknown
    std::string print_file; double progress{0}; long duration_s{0}; long remaining_s{0};
    double bed_temp{0}, bed_target{0}, chamber_temp{0}, chamber_target{0};
    std::vector<NozzleTemp> nozzles;
    bool light_on{false}; std::string door; std::string error_code;
    std::string name, model, firmware, ip, camera_stream_url; int pid{0};
    bool has_material_station{false}; std::vector<MaterialSlot> slots;
    nlohmann::json raw;                // the untouched `detail` object
};
// Returns false with `error` set if the body is not a successful API response.
bool parse_detail(const std::string& body, PrinterStatus& out, std::string& error);
nlohmann::json make_credentials_payload(const std::string& serial, const std::string& check_code);
nlohmann::json make_control_payload(const std::string& serial, const std::string& check_code, const std::string& cmd, const nlohmann::json& args);
nlohmann::json make_temperature_args(std::optional<double> bed, std::optional<double> chamber, const std::vector<std::optional<double>>& nozzles); // -200 for absent
nlohmann::json make_print_gcode_payload(const std::string& serial, const std::string& check_code, const std::string& file_name, bool leveling, const nlohmann::json& material_mappings);
constexpr int kTempNoChange = -200;
constexpr int kPidCreator5 = 40, kPidCreator5Pro = 41;
}}
```

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/slic3rutils/test_flashforge_api.cpp
#include <catch2/catch_test_macros.hpp>
#include "slic3r/Utils/FlashforgeApi.hpp"
using namespace Slic3r::FlashforgeApi;

static const char* kDetail = R"({"code":0,"message":"Success","detail":{
  "status":"printing","printFileName":"benchy.gcode","printProgress":0.42,"printDuration":600,"estimatedTime":1400,
  "platTemp":60.2,"platTargetTemp":60,"chamberTemp":35,"chamberTargetTemp":0,
  "nozzleTemps":[215,30,30,210],"nozzleTargetTemps":[215,0,0,210],"nozzleCnt":4,
  "lightStatus":"open","doorStatus":"close","errorCode":"","firmwareVersion":"1.9.2","name":"C5P","model":"Creator 5 Pro","pid":41,
  "ipAddr":"192.168.1.50","cameraStreamUrl":"http://192.168.1.50:8080/?action=stream",
  "hasMatlStation":true,"matlStationInfo":{"slotCnt":4,"slotInfos":[{"slotId":1,"hasFilament":true,"materialName":"PLA","materialColor":"#FF0000"}]}}})";

TEST_CASE("parse_detail reads a Creator 5 Pro status", "[flashforge]") {
    PrinterStatus s; std::string err;
    REQUIRE(parse_detail(kDetail, s, err));
    CHECK(s.state == "printing");
    CHECK(s.print_file == "benchy.gcode");
    CHECK(s.progress == Catch::Approx(0.42));
    CHECK(s.remaining_s == 1400);
    CHECK(s.nozzles.size() == 4);
    CHECK(s.nozzles[3].target == 210);
    CHECK(s.light_on);
    CHECK(s.pid == kPidCreator5Pro);
    CHECK(s.slots.size() == 1);
    CHECK(s.slots[0].material_color == "#FF0000");
    CHECK(s.raw["nozzleCnt"] == 4);
}

TEST_CASE("parse_detail falls back to rightTemp for single nozzle printers", "[flashforge]") {
    PrinterStatus s; std::string err;
    REQUIRE(parse_detail(R"({"code":0,"detail":{"status":"ready","rightTemp":25,"rightTargetTemp":0,"platTemp":24,"platTargetTemp":0}})", s, err));
    REQUIRE(s.nozzles.size() == 1);
    CHECK(s.nozzles[0].current == 25);
}

TEST_CASE("parse_detail rejects error codes and garbage", "[flashforge]") {
    PrinterStatus s; std::string err;
    CHECK_FALSE(parse_detail(R"({"code":401,"message":"check code error"})", s, err));
    CHECK(err.find("401") != std::string::npos);
    CHECK_FALSE(parse_detail("not json", s, err));
}

TEST_CASE("control payload shapes", "[flashforge]") {
    auto p = make_control_payload("SN1", "CC1", "jobCtl_cmd", {{"jobID", ""}, {"action", "pause"}});
    CHECK(p["serialNumber"] == "SN1");
    CHECK(p["payload"]["cmd"] == "jobCtl_cmd");
    CHECK(p["payload"]["args"]["action"] == "pause");

    auto t = make_temperature_args(60.0, std::nullopt, {215.0, std::nullopt, std::nullopt, std::nullopt});
    CHECK(t["platform"] == 60);
    CHECK(t["chamber"] == kTempNoChange);
    CHECK(t["nozzles"] == nlohmann::json({215, kTempNoChange, kTempNoChange, kTempNoChange}));
    CHECK(t["rightNozzle"] == 215);   // first tool mirrored into the legacy field

    auto g = make_print_gcode_payload("SN1", "CC1", "a.gcode", true, nlohmann::json::array({{{"toolId", 0}, {"slotId", 1}}}));
    CHECK(g["fileName"] == "a.gcode");
    CHECK(g["levelingBeforePrint"] == true);
    CHECK(g["useMatlStation"] == true);
    CHECK(g["gcodeToolCnt"] == 1);
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 2>&1 | tail -5
```

Expected: compile error, `FlashforgeApi.hpp` not found.

- [ ] **Step 3: Implement `FlashforgeApi.cpp`**

Implement each function against the tables above. `parse_detail`: parse with `nlohmann::json::parse(body, nullptr, false)`; reject `is_discarded()`; read `code` (accept int, bool, or numeric string; absent means 0) and set `error = "Flashforge API error <code>: <message>"` when non-zero; take `detail` if present else the root; fill fields with `value(key, default)`; normalize `status` to lower case and map `"cancel"` → `"cancelled"`; `remaining_s = estimatedTime`; nozzles from `nozzleTemps`/`nozzleTargetTemps` (zip, pad targets with 0), else from `rightTemp`/`rightTargetTemp`; `light_on = lightStatus == "open"`; slots from `matlStationInfo.slotInfos`; `has_material_station = hasMatlStation || !slots.empty()`; `raw = detail`. `make_temperature_args`: `platform`, `chamber`, `nozzles` (pad to 4 with `kTempNoChange`), `rightNozzle = nozzles[0]`, `leftNozzle = kTempNoChange`. `make_print_gcode_payload`: `useMatlStation = !mappings.empty()`, `gcodeToolCnt = mappings.size()`, `flowCalibration = false`.

- [ ] **Step 4: Run the tests**

```bash
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 && ./build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests "[flashforge]"
```

Expected: all passed. (Adjust the binary path to wherever `orcaslicer_copy_test_dlls` places it under `build/arm64/tests/`.)

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/Utils/FlashforgeApi.hpp src/slic3r/Utils/FlashforgeApi.cpp tests/slic3rutils src/slic3r/CMakeLists.txt
git commit -m "Flashforge: pure API helpers (detail parser, control payloads) with Catch2 tests

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2.3: Status and control methods on the `Flashforge` print host

**Files:**
- Modify: `src/slic3r/Utils/Flashforge.hpp`, `.cpp`

**Interfaces:**
- Consumes: existing private `request_local_api_json(path, body, response_body, error_msg)` and `make_http_url(path)`; `FlashforgeApi` from Task 2.2.
- Produces (all `const`, all safe to call off the main thread, all return `false` and fill `msg` on failure):
  ```cpp
  bool fetch_status(FlashforgeApi::PrinterStatus& out, wxString& msg) const;
  bool send_control(const std::string& cmd, const nlohmann::json& args, wxString& msg) const;
  bool pause_job(wxString& msg) const;  bool resume_job(wxString& msg) const;  bool cancel_job(wxString& msg) const;
  bool set_light(bool on, wxString& msg) const;
  bool set_temperatures(std::optional<double> bed, std::optional<double> chamber, const std::vector<std::optional<double>>& nozzles, wxString& msg) const;
  bool list_gcode_files(std::vector<std::string>& files, wxString& msg) const;
  bool print_gcode_file(const std::string& file_name, bool leveling, const nlohmann::json& material_mappings, wxString& msg) const;
  bool has_local_api_credentials() const { return !m_serial_number.empty() && !m_check_code.empty(); }
  ```

- [ ] **Step 1: Implement**

```cpp
bool Flashforge::fetch_status(FlashforgeApi::PrinterStatus& out, wxString& msg) const
{
    if (!has_local_api_credentials()) { msg = _L("Flashforge local API requires both serial number and access code."); return false; }
    std::string body;
    if (!request_local_api_json("detail", FlashforgeApi::make_credentials_payload(m_serial_number, m_check_code).dump(), body, msg)) return false;
    std::string err;
    if (!FlashforgeApi::parse_detail(body, out, err)) { msg = from_u8(err); return false; }
    return true;
}

bool Flashforge::send_control(const std::string& cmd, const nlohmann::json& args, wxString& msg) const
{
    if (!has_local_api_credentials()) { msg = _L("Flashforge local API requires both serial number and access code."); return false; }
    std::string body;
    return request_local_api_json("control", FlashforgeApi::make_control_payload(m_serial_number, m_check_code, cmd, args).dump(), body, msg);
}

bool Flashforge::pause_job(wxString& msg) const  { return send_control("jobCtl_cmd", {{"jobID", ""}, {"action", "pause"}}, msg); }
bool Flashforge::resume_job(wxString& msg) const { return send_control("jobCtl_cmd", {{"jobID", ""}, {"action", "continue"}}, msg); }
bool Flashforge::cancel_job(wxString& msg) const { return send_control("jobCtl_cmd", {{"jobID", ""}, {"action", "cancel"}}, msg); }
bool Flashforge::set_light(bool on, wxString& msg) const { return send_control("lightControl_cmd", {{"status", on ? "open" : "close"}}, msg); }
bool Flashforge::set_temperatures(std::optional<double> bed, std::optional<double> chamber, const std::vector<std::optional<double>>& nozzles, wxString& msg) const
{ return send_control("temperatureCtl_cmd", FlashforgeApi::make_temperature_args(bed, chamber, nozzles), msg); }

bool Flashforge::list_gcode_files(std::vector<std::string>& files, wxString& msg) const
{
    std::string body;
    if (!request_local_api_json("gcodeList", FlashforgeApi::make_credentials_payload(m_serial_number, m_check_code).dump(), body, msg)) return false;
    auto j = nlohmann::json::parse(body, nullptr, false);
    files.clear();
    for (const auto& f : j.value("gcodeList", nlohmann::json::array())) files.push_back(f.is_string() ? f.get<std::string>() : f.value("gcodeFileName", ""));
    return true;
}

bool Flashforge::print_gcode_file(const std::string& file_name, bool leveling, const nlohmann::json& material_mappings, wxString& msg) const
{
    std::string body;
    return request_local_api_json("printGcode", FlashforgeApi::make_print_gcode_payload(m_serial_number, m_check_code, file_name, leveling, material_mappings).dump(), body, msg);
}
```

Also add `.timeout_max(15)` to `request_local_api_json`'s `Http` chain so a sleeping printer cannot hang an MCP call for the default libcurl timeout.

- [ ] **Step 2: Build and verify against the real printer**

Add the Creator 5 Pro as a physical printer in the GUI (host = printer IP, serial + check code from the printer's touchscreen network page), press "Test" → expect the upstream success message. Then temporarily verify one control call from the GUI is not possible yet; verification of these methods happens through Task 2.5's tools.

- [ ] **Step 3: Commit**

```bash
git add src/slic3r/Utils/Flashforge.hpp src/slic3r/Utils/Flashforge.cpp
git commit -m "Flashforge host: status, job control, light, temperature, file list and print-file methods

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2.4: Printer resolution utils and physical-printer management tools

> **Ruling (2026-09-08):** OrcaSlicer 2.5 stores print-host settings in the *edited printer preset* (`PhysicalPrinterDialog` edits `printers.get_edited_preset().config`, `OnOK` saves a user printer preset; `Plater::send_gcode_legacy` reads the edited preset). `PhysicalPrinterCollection` is vestigial. Therefore: `resolve_print_host_config` = edited printer preset config; `add_physical_printer` writes the host keys into the edited preset and saves it as a user printer preset named `name` (via `Tab::save_preset`); `select_printer {physical_printer}` selects that preset; `get_printers.physical_printers` lists printer presets with a non-empty `print_host`. Ignore the `PhysicalPrinterCollection` calls sketched below.

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp`, `.cpp`
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp`
- Modify: `OrcaMCPServer.hpp` (`static void register_printer_tools();`), `OrcaMCPServer.cpp` (delete the old `get_printers`, `select_printer`, `send_to_printer` registrations; call `register_printer_tools()`), `src/slic3r/CMakeLists.txt`

**Interfaces:**
- Produces (utils):
  ```cpp
  // The config OrcaSlicer would use for "Send": the selected physical printer if one is selected, else the edited printer preset. Main thread.
  bool resolve_print_host_config(DynamicPrintConfig& out, std::string& host_type_name, std::string& error);
  // PrintHost::get_print_host on a copy of that config (caller owns). Main thread for the resolve, any thread afterwards.
  std::unique_ptr<PrintHost> make_print_host(const DynamicPrintConfig& cfg);
  nlohmann::json status_to_json(const FlashforgeApi::PrinterStatus& s);
  // Auto-map project filaments to material station slots by normalized material name; returns the materialMappings array.
  nlohmann::json auto_material_mappings(const std::vector<FlashforgeApi::MaterialSlot>& slots, const std::vector<std::pair<int,std::string>>& tool_types);
  ```
- Produces tools:

`get_printers` — same shape as today (Bambu devices + `physical_printers[]` + `current_print_host`) plus `selected_physical_printer` (name or null). Move the existing body verbatim.

`select_printer` — accepts **either** `dev_id` (Bambu, existing behaviour) **or** `physical_printer` (name as shown in `get_printers`) → selects it via `preset_bundle->physical_printers.select_printer(name)` and updates the printer tab/combo the way `PlaterPresetComboBox` does on selection (find the call site with `git grep -n "physical_printers.select_printer" src/slic3r/GUI`), returning `{"status":"success","physical_printer":name,"host_type":"flashforge","print_host":"192.168.1.50"}`.

`discover_printers` params: optional `timeout_ms` (default 5000) → `{"status":"success","printers":[{"name","serial_number","ip_address"}]}` via `Flashforge::discover_printers` (UDP, runs on the HTTP thread, no GUI).

`add_physical_printer` params: `name`, `host`, `host_type` (`flashforge|moonraker|octoprint|prusalink|duet|repetier|mks|elegoolink|crealityprint`), optional `serial_number`, `api_key`, `printer_preset` (defaults to the edited printer preset name) → creates/overwrites a `PhysicalPrinter` with `print_host`, `host_type`, `printhost_apikey`, `flashforge_serial_number`, `printhost_authorization_type = "key"`, `preset_name(s)`, saves it via `physical_printers.save_printer(printer, "")`, selects it, and returns the `select_printer` response. Mirror `PhysicalPrinterDialog::OnOK` (`src/slic3r/GUI/PhysicalPrinterDialog.cpp`) for the exact save + refresh sequence; do not open the dialog.

- [ ] **Step 1: Write the utils and the four tools** per the interfaces above. `resolve_print_host_config`:

```cpp
bool resolve_print_host_config(DynamicPrintConfig& out, std::string& host_type_name, std::string& error)
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb->physical_printers.has_selection()) {
        out = pb->physical_printers.get_selected_printer_config();   // full printer preset + physical overrides
    } else {
        out = pb->printers.get_edited_preset().config;
    }
    if (out.opt_string("print_host").empty()) { error = "No print host configured. Use add_physical_printer first."; return false; }
    host_type_name = out.opt_serialize("host_type");
    return true;
}
```

`PhysicalPrinterCollection::get_selected_printer_config()` exists upstream (used by `Plater::send_gcode_legacy`); confirm the name with `git grep -n "get_selected_printer_config" src/libslic3r/Preset.hpp`.

- [ ] **Step 2: Build, regenerate schema, verify live**

1. `discover_printers` → the Creator 5 Pro appears with its serial.
2. `add_physical_printer` `{"name":"C5P","host":"<ip>","host_type":"flashforge","serial_number":"<sn>","api_key":"<check code>"}` → success; the GUI's printer combo shows `C5P`.
3. `get_printers` → `selected_physical_printer: "C5P"`.
4. `select_printer` `{"physical_printer":"C5P"}` → success.

- [ ] **Step 3: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP src/slic3r/CMakeLists.txt scripts/tools_schema.py
git commit -m "OrcaMCP: printer tools split out; discover_printers, add_physical_printer, physical select_printer

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2.5: Printer status, control and file tools

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp`, `OrcaMCPPrinterUtils.cpp`

**Interfaces:**
- Consumes: `Flashforge::fetch_status/send_control/...` (Task 2.3), `resolve_print_host_config`, `make_print_host`, `status_to_json`.
- Produces tools:

`get_printer_status` (no params) →
```json
{"status":"success","host_type":"flashforge","print_host":"192.168.1.50","online":true,
 "printer":{"state":"printing","print_file":"benchy.gcode","progress":0.42,"duration_s":600,"remaining_s":1400,
  "temperatures":{"bed":{"current":60.2,"target":60},"chamber":{"current":35,"target":0},
                  "nozzles":[{"tool":0,"current":215,"target":215},{"tool":1,"current":30,"target":0}]},
  "light_on":true,"door":"close","error_code":"","name":"C5P","model":"Creator 5 Pro","firmware":"1.9.2",
  "camera_stream_url":"http://192.168.1.50:8080/?action=stream",
  "material_station":{"present":true,"slots":[{"slot_id":1,"has_filament":true,"material":"PLA","color":"#FF0000"}]},
  "raw":{...}}}
```
For a Moonraker host, return `online` from `PrintHost::test()` and `printer: null` with `note: "Status details are only implemented for Flashforge hosts"`. For no host: `status: error`.

`printer_control` params: `action` (`pause|resume|cancel|light_on|light_off|set_temperature`), for `set_temperature`: optional `bed`, `chamber`, `nozzles` (array of `{"tool":int,"temp":number}`) → `{"status":"success","action":...}` or error with the printer's message.

`list_printer_files` → `{"status":"success","files":["benchy.gcode", ...]}`.

`print_printer_file` params: `file_name`, optional `leveling_before_print` (default false), optional `material_mappings` (array of `{"tool_id":0,"slot_id":1}`), optional `auto_map` (default true: build mappings from the current project's filament types vs the material station) → `{"status":"success","file_name":...,"material_mappings":[...]}`.

- [ ] **Step 1: Implement**

Pattern for each tool: on the HTTP thread, call `run_on_main_thread` only to `resolve_print_host_config` (fast, needs GUI state), then perform the network call **outside** `run_on_main_thread` so the GUI never blocks on the printer:

```cpp
static bool resolve_flashforge(std::unique_ptr<PrintHost>& host, Flashforge*& ff, nlohmann::json& error_response)
{
    DynamicPrintConfig cfg; std::string type, err;
    nlohmann::json resolved = run_on_main_thread([&]() -> nlohmann::json {
        bool ok = resolve_print_host_config(cfg, type, err);
        return {{"ok", ok}};
    });
    if (!resolved["ok"].get<bool>()) { error_response = {{"status", "error"}, {"message", err}}; return false; }
    host = make_print_host(cfg);
    ff = dynamic_cast<Flashforge*>(host.get());
    if (!ff) { error_response = {{"status", "error"}, {"message", "Selected printer host type is " + type + "; this tool requires a Flashforge host"}}; return false; }
    return true;
}
```

`auto_material_mappings` reuses upstream's normalization rules from `FlashforgePrintHostSendDialog::normalize_material` (PLA/PLA+/PLA-CF → PLA, PETG/PETG-CF → PETG, ABS/ASA → ABS, TPU, SILK): expose that function as a free function in `PrintHostDialogs.hpp` (`std::string flashforge_normalize_material(const std::string&)`) rather than duplicating it. The project's tool types come from `wxGetApp().plater()->get_partplate_list().get_curr_plate()->slice_filaments_info` + `full_config().get_filament_type(...)`, gathered on the main thread.

- [ ] **Step 2: Build, regenerate schema, verify against the printer**

1. `get_printer_status` while idle → `printer.state == "ready"`, four nozzles, `material_station.slots` length 4.
2. `printer_control` `{"action":"light_off"}` → printer light goes off; `light_on` → back on.
3. `printer_control` `{"action":"set_temperature","nozzles":[{"tool":0,"temp":60}]}` → tool 0 target 60 in the next `get_printer_status`; then `{"nozzles":[{"tool":0,"temp":0}]}`.
4. `list_printer_files` → non-empty array.
5. Start a short print from the touchscreen; `printer_control` `pause` → state `paused`; `resume` → `printing`; `cancel` → `cancelled`/`ready`.
6. Error path: wrong check code in `add_physical_printer`, then `get_printer_status` → `status: error` with the printer's message.

- [ ] **Step 3: Commit**

```bash
git add src/slic3r/GUI/OrcaMCP scripts/tools_schema.py src/slic3r/GUI/PrintHostDialogs.hpp src/slic3r/GUI/PrintHostDialogs.cpp
git commit -m "OrcaMCP: get_printer_status, printer_control, list_printer_files, print_printer_file

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2.6: `send_to_printer` uploads directly, without a dialog

Today the tool only opens OrcaSlicer's send dialog (`status: dialog_opened`). On upstream, `Plater::send_gcode_legacy` (Plater.cpp, search `void Plater::send_gcode_legacy`) exports the current plate to a temp file, builds a `PrintHostJob`, fetches Flashforge material slots, shows `FlashforgePrintHostSendDialog`, copies `pDlg->extendedInfo()` into `upload_job.upload_data.extended_info`, and enqueues `wxGetApp().printhost_job_queue().enqueue(std::move(upload_job))`.

**Files:**
- Modify: `src/slic3r/GUI/Plater.hpp`, `.cpp` — split `send_gcode_legacy` into the dialog part and a new public `bool Plater::send_gcode_direct(int plate_idx, const std::map<std::string,std::string>& extended_info, PrintHostPostUploadAction post_action, std::string& error)` that does everything except the dialog (export, job build, enqueue). `send_gcode_legacy` calls it after the dialog.
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp`

**Interfaces:**
- Produces `send_to_printer` params: `all_plates` (existing), new `direct` (bool, default `true`), `start_print` (bool, default `true`), `leveling_before_print` (bool, default `false`), `use_material_station` (bool, default `true` on Flashforge hosts with a material station), `material_mappings` (array of `{"tool_id","slot_id"}`; omitted → auto-map), `file_name` (optional upload name) → `{"status":"queued","host_type":"flashforge","file_name":"benchy.gcode","material_mappings":[...],"note":"Upload progress is shown in OrcaSlicer; poll get_printer_status."}`. With `direct: false` the old dialog behaviour is kept for every host type.

- [ ] **Step 1: Refactor `Plater::send_gcode_legacy`** into `send_gcode_direct` + dialog wrapper. Keep behaviour identical for the GUI (the `extended_info` map for Flashforge is exactly `levelingBeforePrint`, `timeLapseVideo`, `useMatlStation` as `"1"/"0"`, `gcodeToolCnt`, and `materialMappings` as a JSON string of `[{toolId, slotId, materialName, toolMaterialColor, slotMaterialColor}]`).

- [ ] **Step 2: Implement the direct path in the tool**

On the main thread: check `!plater->is_background_process_slicing()` and that the plate is sliced (`plater->is_preview_shown()` or `get_curr_plate()->is_slice_result_valid()`), resolve the host config, gather project tool types. Off the main thread: if Flashforge with material station and no explicit mappings → `fetch_status` and `auto_material_mappings`. Back on the main thread: build `extended_info` and call `send_gcode_direct(plate_idx, extended_info, start_print ? StartPrint : None, error)`. Wrap the main-thread parts in `McpDialogSuppressionGuard` and surface `info_messages`.

- [ ] **Step 3: Build, regenerate schema, verify end to end**

Slice the four-object plate from Task 1.8, then `send_to_printer` `{}` → `status: queued`; OrcaSlicer shows the upload notification; the printer starts the job; `get_printer_status` reports `printing` with `print_file` equal to the returned `file_name`. Then `printer_control` `cancel`. Also verify `send_to_printer` `{"direct": false}` still opens the dialog.

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/Plater.hpp src/slic3r/GUI/Plater.cpp src/slic3r/GUI/OrcaMCP scripts/tools_schema.py
git commit -m "OrcaMCP: send_to_printer uploads directly with material-station mapping

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2.7: `FlashforgePrinterAgent` so the Device tab shows the Creator 5

Upstream renders non-Bambu printers in its Device tab through `IPrinterAgent` (`src/slic3r/Utils/IPrinterAgent.hpp`), with `MoonrakerPrinterAgent` (`src/slic3r/Utils/MoonrakerPrinterAgent.hpp/.cpp`) as the reference implementation: it polls the printer and pushes a Bambu-shaped JSON payload (`nozzle_temper`, `nozzle_target_temper`, `bed_temper`, `bed_target_temper`, `chamber_temper`, `mc_percent`, `mc_remaining_time`, `gcode_state`, `subtask_name`, `print_error`, `lights_report`, `ipcam`) that the existing `MonitorPanel` already knows how to draw. This is the "printer page" done without Flash Studio's closed plugin.

**Files:**
- Create: `src/slic3r/Utils/FlashforgePrinterAgent.hpp`, `.cpp`
- Modify: wherever `MoonrakerPrinterAgent` is instantiated / chosen by host type (`git grep -n "MoonrakerPrinterAgent(" src/slic3r` and `git grep -n "htMoonraker" src/slic3r/GUI/DeviceManager.cpp src/slic3r/GUI/GUI_App.cpp`), add the `htFlashforge` case.
- Modify: `src/slic3r/CMakeLists.txt`

**Interfaces:**
- Consumes: `Flashforge::fetch_status/pause_job/resume_job/cancel_job/set_light` (Task 2.3).
- Produces: `class FlashforgePrinterAgent : public IPrinterAgent` implementing the same set of virtuals as `MoonrakerPrinterAgent` (copy its header, keep every override, replace Moonraker HTTP calls). Mapping from `FlashforgeApi::PrinterStatus` to the payload:

| payload key | source |
|---|---|
| `nozzle_temper` / `nozzle_target_temper` | `nozzles[active]`, where active = first nozzle whose target > 0, else nozzle 0; also emit `extruder: [{temp, target}...]` for all four |
| `bed_temper` / `bed_target_temper` | `bed_temp` / `bed_target` |
| `chamber_temper` | `chamber_temp` |
| `mc_percent` | `int(progress * 100)` |
| `mc_remaining_time` | `remaining_s / 60` |
| `gcode_state` | `printing→RUNNING`, `paused→PAUSE`, `completed→FINISH`, `error→FAILED`, `ready|busy|heating|cancelled→IDLE` |
| `subtask_name` | `print_file` |
| `print_error` | `error_code` (0 when empty) |
| `lights_report` | `[{"node":"chamber_light","mode": light_on ? "on" : "off"}]` |
| `ipcam.rtsp_url` | `camera_stream_url` |

Commands: `pause_print → pause_job`, `resume_print → resume_job`, `stop_print → cancel_job`, `set_light → set_light`; unsupported commands return `ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED`. Poll interval 2 s while printing, 5 s otherwise, on the agent's worker thread (mirror Moonraker's timer).

- [ ] **Step 1: Copy `MoonrakerPrinterAgent.hpp` to `FlashforgePrinterAgent.hpp`**, rename the class, delete Moonraker-specific members, add `std::unique_ptr<Flashforge> m_host;` constructed from the resolved physical-printer config.

- [ ] **Step 2: Implement the poll + mapping + commands** per the table. Keep a unit-testable free function `nlohmann::json flashforge_status_to_bambu_payload(const FlashforgeApi::PrinterStatus&)` in `FlashforgeApi.cpp` and add a Catch2 case for the `gcode_state` mapping and the `mc_percent` rounding to `test_flashforge_api.cpp`.

- [ ] **Step 3: Register the agent for `htFlashforge`** at the same place Moonraker is chosen.

- [ ] **Step 4: Build and verify**

Run `slic3rutils_tests "[flashforge]"` (expect all passed). Open OrcaSlicer, select `C5P`, open the Device tab: temperatures and state update; start a print from `send_to_printer`; progress bar moves; pause/resume/stop buttons work; the light toggle works; the camera link opens the stream URL in the browser (or the embedded player if `MonitorPanel` accepts an HTTP MJPEG URL).

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/Utils/FlashforgePrinterAgent.hpp src/slic3r/Utils/FlashforgePrinterAgent.cpp src/slic3r/Utils/FlashforgeApi.cpp tests/slic3rutils src/slic3r
git commit -m "Flashforge: IPrinterAgent so the Device tab shows Creator 5 status and controls

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2.7b: Bug sweep of findings deferred during Stages 1–2

User rule (2026-09-08): every bug found is fixed, together with related occurrences of the same pattern. This task closes the items reviews parked earlier. Each fix: grep for the same pattern across `src/slic3r/GUI/OrcaMCP/` and `src/slic3r/Utils/Flashforge*.cpp`, fix all occurrences, add or extend a test where one exists, verify live where it is an MCP behaviour.

**Files:** as listed per item.

- [ ] **Item A — HTTP server answers before the GUI is initialised** (`src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` `handle_request`): a `tools/call` arriving before `wxGetApp().plater()` / `preset_bundle` exist threw `type_error` and once took the process down. Add a readiness check at the top of `handle_request` (plater and preset bundle non-null, and app initialisation finished — find the flag `GUI_App` sets after `post_init`, e.g. `m_post_initialized`/`is_editor()` + plater) returning JSON-RPC error `-32001 "OrcaMCP is starting up"`; and wrap each tool invocation in a `try { } catch (const std::exception&)` that returns `-32603` with the message so no exception escapes to the HTTP thread. Verify: relaunch and hammer `get_scene_info` immediately; first responses are the startup error, never a crash.
- [ ] **Item B — `get_print_estimate` reports `in_progress` after slicing finished** (`OrcaMCPServer.cpp`, tools `get_slicing_status` and `get_print_estimate`): compare the state check with upstream 2.5's `Plater::is_background_process_slicing()` / `priv::background_process.running()` / `is_preview_shown()` and the plate's `is_slice_result_valid()`. Use the plate-level result validity as the source of truth for "done"; report `state: idle|slicing|done`. Verify live: slice, poll status to done, `get_print_estimate` returns numbers.
- [ ] **Item C — `apply_config` last-write-wins on duplicate type+key in one batch** (`OrcaMCPPresetConfigUtils.cpp` / `OrcaMCPServer.cpp`): detect duplicates and report them in a `duplicate_keys` array (last value applied, documented). Same for `set_object_config` batches if the same shape exists.
- [ ] **Item D — `list_gcode_files` and `fetch_material_slots` can throw on a non-object array element** (`src/slic3r/Utils/Flashforge.cpp`): guard `f.is_object()` / `is_string()` before `.value()`; add a Catch2 case with a malformed `gcodeList` to `test_flashforge_api.cpp` (move the parsing into `FlashforgeApi::parse_gcode_list` if not already there so it is testable). Related occurrence: the `slotInfos` loop.
- [ ] **Item E — credential-guard block repeated across `Flashforge` methods**: extract `bool require_local_api_credentials(wxString& msg) const` and use it in every method (behaviour identical).
- [ ] **Item F — `try_parse_json_int` copy in FlashforgeApi lacks `boost::trim`**: either add the trim or, better, make `Flashforge.cpp` call the FlashforgeApi version so only one copy exists.
- [ ] **Item G — `set_object_filament` treats any negative `volume_id` as "whole object"**: reject values other than -1/absent with an error. Related occurrence: the same pattern in `set_object_printable`/`set_object_config` if present.
- [ ] **Item H — `get_presets` returns ~1.9 MB**: add optional `type` (`printer|filament|print`) and `vendor` filters plus a `summary` boolean (names only) so MCP clients can page; default behaviour unchanged. Document in reference.md.
- [ ] **Item I — hex colour inputs accept trailing garbage** (`suggest_color_mix.target_color`): validate `^#[0-9A-Fa-f]{6}$` in the tool before calling the upstream parser; related occurrence: `filament_colour` writes via `apply_config` project type are validated by upstream already (confirm, no change if so).
- [ ] **Item J — Linux AppImage packaging expects `orca-slicer`** (CI run 34151045449: `mv: cannot stat 'orca-slicer'`, `Error: entrypoint does not exist: ./build/package/bin/orca-slicer`, and `cp: cannot stat .../scripts/flatpak/com.orcaslicer.OrcaMCP.metainfo.xml`): the fork renames the binary to `orca-mcp` (`src/CMakeLists.txt` `OUTPUT_NAME`) but upstream's `src/dev-utils/platform/unix/build_appimage.sh.in` / `build_linux_image.sh.in` and `build_linux.sh` hard-code `orca-slicer`, and the metainfo filename is derived from `SLIC3R_APP_KEY`. Fix: thread `@SLIC3R_APP_CMD@` through both templates (entrypoint, `mv`, `AppRun`, desktop `Exec=`), set `SLIC3R_APP_CMD` consistently in the top-level `CMakeLists.txt` (currently still `orca-slicer` there), and add `scripts/flatpak/com.orcaslicer.OrcaMCP.metainfo.xml` (copy of the OrcaSlicer one with the id/name adjusted). Related occurrence: any other `orca-slicer` literal in `scripts/` and `.github/workflows/` (grep).
- [ ] **Item K — `tests/slic3rutils/test_plugin_audit.cpp:74-75` fails on the fork** (`is_denied_filename("orcaslicer.conf")`): the audit derives the denied app-config filename from `SLIC3R_APP_KEY`, so on OrcaMCP it denies `orcamcp.conf`. Fix: make the audit deny BOTH the current app key's config name and the upstream `orcaslicer.conf` (users migrate configs from OrcaSlicer), and change the test to assert both names using `SLIC3R_APP_KEY` lowercased. Related occurrence: any other test hard-coding `orcaslicer` (grep `tests/` for `orcaslicer\.` and `OrcaSlicer\.conf`).

- [ ] **Item L — `load_project` can hang the app** (observed 2026-09-08 during Task 2.5 verification: `load_project` of a just-exported 3MF hung at 0% CPU until force-quit): reproduce with `export_3mf` → `new_project` → `load_project` of that file; check for a modal not covered by `McpDialogSuppressionGuard` (e.g. the "project modified / save changes" prompt or the 3MF version/`load geometry only` dialog) and for `load_project` being called while the background process is running. Fix the suppression gap and/or wait for the background process; add the regression sequence to the Stage 2 wrap-up checks.

- [ ] **Item M — raw `set_mcp_dialog_suppression(true/false)` pairs leak the flag on exceptions** (~a dozen handlers in OrcaMCPServer.cpp and the tool files): replace every manual pair with `McpDialogSuppressionGuard` (Task 1.2) so an exception between the calls cannot leave suppression enabled globally. Related occurrence: any new handler added in Stage 2. Verify with `grep -n "set_mcp_dialog_suppression(" src/slic3r/GUI/OrcaMCP/` → only the guard's own two calls remain.
- [ ] **Item N — no MCP way to name an unsaved project**: after Item L, `save_project` without a path returns `cancelled` instead of opening a dialog. Add a required-or-derived `output_path` behaviour: if the project has no path and none is given, return an error telling the caller to pass `output_path`; document in reference.md.

- [ ] **Item O — `export_3mf` / `load_project` now name the project silently** (Item L follow-up): `export_3mf` calls `set_project_filename`, which retitles the window and adds the path to Recent Projects; a later Cmd-S in the GUI overwrites that file. Surface it: add `"project_renamed_to": <path>` and an `info_messages` line to the `export_3mf` and `load_project` responses, and document the semantics ("export_3mf is this API's Save") in reference.md and CLAUDE.md's dialog section.

- [ ] **Item P — auto material mapping ignores colour**: `auto_material_mappings` takes the first free slot of the matching material family (same as the GUI dialog), so with four PLA slots loaded, tool 1 (blue) mapped to slot 1 (magenta). Prefer, within the same material family, the loaded slot whose `materialColor` is closest to the project filament colour (CIE76 via `color_decompose_delta_e`), falling back to first-free when no colours are known; report `color_delta_e` per mapping in the response so an agent can warn on poor matches. Applies to both `print_printer_file` and `send_to_printer` (shared helper).

- [ ] **Step: build, run `[flashforge]` tests, regen schema, pytest, live checks per item, commit per item** with messages `fix: <item>`, each ending with the Co-Authored-By trailer.

---

### Task 2.8: Stage 2 wrap-up

- [ ] **Step 1: Regenerate the schema, run all tests**

```bash
python3 scripts/regen_tools_schema.py
python3 -m pytest scripts/tests -q
./build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests "[flashforge]"
```

  - Dialog-hang regression (Task 2.7b Item L): `load_model` + `apply_config layer_height` (dirty project *and* presets) → `new_project`, then the same again → `export_3mf` (explicit path) → `load_model` → `load_project` of that file; both must return in seconds with `status: success` and `info_messages` naming the suppressed unsaved-changes dialogs (never a hang), and `save_project` / `export_3mf` / `export_gcode` with no path must return an error instead of opening a file dialog.

- [ ] **Step 2: Full natural-language run** through Claude Code: "Load benchy, split it into four color regions by part, assign one tool each, add a 60/40 red-blue mix for the hull, slice, send to the Creator 5 Pro and start it, then report progress every minute until it's done." Every step must go through MCP tools with no dialogs.

- [ ] **Step 3: Push**

```bash
git push origin mcp
```

---

# Stage 3: Docs, packaging, CI, release

### Task 3.1: Re-apply the fork's CI rebranding onto upstream's workflows

Upstream's `build_orca.yml` is now multi-arch (Windows `_x64`/`_arm64`, Linux `_aarch64`), names everything `OrcaSlicer_*`, dropped the `-1` flag for `-j 3`, builds Linux with `./build_linux.sh -istrlL`, and runs the orca-test-repo regression test unconditionally on x86_64 Linux.

**Files:**
- Modify: `.github/workflows/build_orca.yml`, `build_all.yml`, `build_deps.yml`, `release.yml`
- Delete: `.github/workflows/winget_updater.yml`

- [ ] **Step 1: `build_orca.yml`** — apply these edits on upstream's file (use `git diff main mcp@{1} -- .github/workflows/build_orca.yml` before the merge as the reference for the fork's intent):
  - Every `OrcaSlicer_Mac_universal_`, `OrcaSlicer_Windows_`, `OrcaSlicer_Linux_` artifact/file name → `OrcaMCP_...`, keeping upstream's `${{ env.ARCH_SUFFIX }}` / `${{ env.arch_suffix }}` tokens.
  - macOS DMG step: copy the app as `OrcaMCP.app`, `hdiutil create -volname "OrcaMCP"`, output `OrcaMCP_Mac_universal_${{ env.ver }}.dmg`; keep upstream's `retry` wrapper.
  - Notarization gates: replace `github.repository == 'OrcaSlicer/OrcaSlicer' && (...)` with `github.repository == 'okets/OrcaMCP'` (and the negated form on the unsigned path).
  - Keep upstream's `-j 3` and `-istrlL` + `check_appimage_libs.sh`. Keep the regression test enabled (the build now produces `build/package/bin/orca-slicer`); if it fails on the fork for a reason unrelated to our changes, gate it with `&& github.repository == 'OrcaSlicer/OrcaSlicer'` as commit `4697da3132` did.
  - Windows portable/installer glob: `build/OrcaMCP*.exe`; keep the fork's "Copy MCP scripts to portable package" step but change its condition to `runner.os == 'Windows'`.
  - Delete the `WebFreak001/deploy-nightly` steps (they target upstream's release id) and the MSIX step (no Store listing for OrcaMCP).
  - Keep the fork's ordering fix: "Delete intermediate per-arch artifacts" must run **after** the universal DMG upload.

- [ ] **Step 2: `build_all.yml`** — add `mcp` back to `push`/`pull_request` branches, set the flatpak job to `if: false` with the fork's comment, and either take upstream's `unit_tests.yml` fan-out together with its "Pack unit tests" steps for every leg, or restrict the matrix to `linux x86_64` only. Choose the second unless the pack steps come for free from Step 1.

- [ ] **Step 3: `build_deps.yml`** — confirm no `-1` remains (`grep -n -- "-1" .github/workflows/build_deps.yml`).

- [ ] **Step 4: `release.yml`** — artifact names and two bug fixes:

```yaml
      - name: Download macOS artifact
        uses: actions/download-artifact@v4
        with:
          name: OrcaMCP_Mac_universal_V${{ steps.version.outputs.version }}
          path: artifacts/macos
      - name: Download Linux artifact
        uses: actions/download-artifact@v4
        with:
          name: OrcaMCP_Linux_ubuntu_2404_V${{ steps.version.outputs.version }}
          path: artifacts/linux
      - name: Download Windows artifact
        uses: actions/download-artifact@v4
        with:
          name: OrcaMCP_Windows_V${{ steps.version.outputs.version }}_x64
          path: artifacts/windows
```

Change `draft: ${{ github.event_name == 'workflow_dispatch' && inputs.draft || true }}` to `draft: ${{ github.event_name == 'workflow_dispatch' && inputs.draft == 'true' }}` (tag pushes publish immediately; manual runs honour the input), and fix the compare URL to `https://github.com/okets/OrcaMCP/compare/v2.4.0.1-dev...${{ steps.version.outputs.tag }}` (or drop it and rely on `generate_release_notes`). Restrict the tag trigger so upstream's tags do not fire it: `tags: ['v*.*.*.*-dev', 'v*.*.*.*']` (four-component versions are the fork's namespace).

- [ ] **Step 5: Delete `winget_updater.yml`**, then validate syntax and run a manual build

```bash
git rm .github/workflows/winget_updater.yml
python3 - <<'EOF'
import yaml,glob
for f in glob.glob('.github/workflows/*.yml'): yaml.safe_load(open(f)); print('ok', f)
EOF
git add .github && git commit -m "ci: rebrand upstream multi-arch workflows for OrcaMCP; fix release download names and draft flag

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
git push origin mcp
gh run list -R okets/OrcaMCP --workflow build_all.yml --limit 1
```

Wait for the run (`gh run watch <id> -R okets/OrcaMCP`). Expected: macOS universal, Linux x86_64 and Windows x64 legs green, artifacts named `OrcaMCP_Mac_universal_V2.5.0.1-dev`, `OrcaMCP_Linux_ubuntu_2404_V2.5.0.1-dev`, `OrcaMCP_Windows_V2.5.0.1-dev_x64`. Iterate until green.

---

### Task 3.2: Packaging checks

- [ ] **Step 1: Verify the bridge ships in every artifact**

Download the three artifacts (`gh run download <id> -R okets/OrcaMCP`) and check:
- DMG: `OrcaMCP.app/Contents/Resources/scripts/orcamcp-bridge.py` and `tools_schema.py` exist and match `scripts/`.
- AppImage: `--appimage-extract` then `usr/share/OrcaMCP/scripts/orcamcp-bridge.py` (or wherever `install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/"...)` lands it) exists.
- Windows installer: run in a VM or inspect with `7z l`, expect `resources/scripts/orcamcp-bridge.py` and `scripts/orcamcp-bridge.py`.

- [ ] **Step 2: Verify first launch writes `~/.orcamcp/`** on a clean macOS user (or after `rm -rf ~/.orcamcp`): launch the DMG app, confirm both scripts appear in `~/.orcamcp/`, and that Preferences → MCP Clients → Claude Code "Connect" writes the absolute bridge path.

- [ ] **Step 3: Fix anything found, commit**

---

### Task 3.3: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`, `docs/README.md`, `docs/tools/reference.md`, `docs/setup/configuration.md`, `PROJECT_ROADMAP.md`

- [ ] **Step 1: Compute the real tool count**

```bash
grep -c 'register_tool({' src/slic3r/GUI/OrcaMCP/*.cpp | awk -F: '{s+=$2} END {print s}'
python3 -c "import sys; sys.path.insert(0,'scripts'); import tools_schema; print(len(tools_schema.FULL_TOOLS_LIST))"
```

The user-visible count is the schema count + 1 (`start_orca`, bridge-only). Use that number everywhere below.

- [ ] **Step 2: `docs/tools/reference.md`** — add entries in the existing format (`### name` → description → parameters table → example → returns) for: `set_object_printable`, `set_gcode_view_type` (previously undocumented), `get_filaments`, `set_mixed_filament`, `delete_mixed_filament`, `set_object_filament`, `get_flush_volumes`, `set_flush_volumes`, `auto_calc_flush_volumes`, `get_toolchanger_config`, `discover_printers`, `add_physical_printer`, `get_printer_status`, `printer_control`, `list_printer_files`, `print_printer_file`; update `select_printer`, `send_to_printer`, `apply_config`, `get_valid_config_keys`. Fix the count on line 3 and add the new tools to the quick-reference table under new categories **Multi-Material** and **Printer (Flashforge)**.

- [ ] **Step 3: `README.md`** — rewrite these sections:
  - Header: one paragraph stating it tracks upstream OrcaSlicer 2.5.0-dev and adds MCP control, color mixing tools, and open Flashforge Creator 5 / 5 Pro integration (no cloud, no closed network plugin).
  - **Downloads**: link `https://github.com/okets/OrcaMCP/releases/latest` with the three asset names, plus the macOS Gatekeeper note (`xattr -cr /Applications/OrcaMCP.app` if unsigned) and Windows SmartScreen note.
  - **Quick Start → Configure Claude Code**: the installed bridge path is `~/.orcamcp/orcamcp-bridge.py` (written by the app on first launch), and the in-app Preferences → MCP Clients "Connect" button configures Claude Code, Claude Desktop, Cursor, Windsurf, Cline, Codex and Copilot. Keep the repo `.mcp.json` example for source checkouts.
  - **Flashforge Creator 5 / 5 Pro**: one line only, linking to `docs/printers/flashforge-creator-5.md`. The README stays generic (user ruling 2026-09-07: it is an MCP-oriented slicer; printer-specific material lives in its own document).
  - **Available Tools (N)**: full category table including Multi-Material and Printer categories.
  - **Building from Source**: macOS `./build_release_macos.sh -x -j 8` (deps + slicer) or `-s -x -j 8`; Linux `./build_linux.sh -u` then `./build_linux.sh -dsi`; Windows `build_release_vs2022.bat`. Remove the non-existent `./build_release.sh`.
  - **Syncing with upstream**: one paragraph pointing at the CLAUDE.md procedure.

- [ ] **Step 3b: `docs/printers/flashforge-creator-5.md`** (new) — everything Creator 5 specific, in this order: what works (direct send with material-station mapping, status/control tools, Device tab); printer prerequisites (static IP or DHCP reservation, LAN mode ON on the touchscreen, where the serial and check code are shown); setup through Claude (`discover_printers` → `add_physical_printer` with the check code) and the manual path via the physical-printer dialog (host type Flashforge, serial field, "API Key / Password" = check code, Test button); the natural-language example from Task 2.8; the material-station mapping rules; the note that Reforge/Klipper-modded printers use the Moonraker host type; a short API reference table (endpoints, control commands, `-200` = no change) for contributors. Link it from README's Documentation table and from docs/README.md.

- [ ] **Step 4: `CLAUDE.md`** — replace the stale "Adding New Tools" snippet with the real 4-field `register_tool({...})` aggregate and the `run_on_main_thread` lambda idiom (copy the `get_object_config` example from `OrcaMCPServer.cpp`), add the checklist (new file → `src/slic3r/CMakeLists.txt`; path params naming; `python3 scripts/regen_tools_schema.py`; `pytest scripts/tests`; docs), fix the three count claims, update the Key Files table (`OrcaMCPCommon`, `OrcaMCPFilamentTools`, `OrcaMCPPrinterTools`, `FlashforgeApi`, `FlashforgePrinterAgent`), add the Flashforge API summary table from Task 2.2, and record the version/tag rule (`2.5.0.1-dev` / `v2.5.0.1-dev`, four-component tags only).

- [ ] **Step 5: `docs/README.md`, `docs/setup/configuration.md`, `PROJECT_ROADMAP.md`** — counts, bridge path, and a roadmap entry for "Moonraker status/control parity with the Flashforge tools".

- [ ] **Step 6: Commit**

```bash
git add README.md CLAUDE.md docs PROJECT_ROADMAP.md
git commit -m "docs: Creator 5 integration, multi-material tools, downloads, corrected tool counts and build commands

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3.4: Release `v2.5.0.1-dev`

- [ ] **Step 1: Confirm the version contract**

```bash
grep SoftFever_VERSION version.inc          # set(SoftFever_VERSION "2.5.0.1-dev")
git status --porcelain                      # empty
git log origin/mcp..mcp --oneline           # empty after push
```

- [ ] **Step 2: Tag and push**

```bash
git tag v2.5.0.1-dev
git push origin v2.5.0.1-dev
gh run list -R okets/OrcaMCP --workflow release.yml --limit 1
gh run watch <run-id> -R okets/OrcaMCP
```

Expected: ~1 h; the `create_release` job downloads all three artifacts and publishes a non-draft release `OrcaMCP v2.5.0.1-dev` with `OrcaMCP-v2.5.0.1-dev-macos-universal.dmg`, `OrcaMCP-v2.5.0.1-dev-linux-x64.AppImage`, `OrcaMCP-v2.5.0.1-dev-windows-x64-installer.exe`.

- [ ] **Step 3: Install the DMG on the Mac and run the acceptance check**

Fresh install to `/Applications`, launch, connect Claude Code from Preferences, then from Claude Code: `get_server_info` shows `2.5.0.1-dev`; `discover_printers` finds the Creator 5 Pro; `get_printer_status` returns `ready`; the Task 2.8 natural-language run succeeds.

- [ ] **Step 4: Edit the release notes**

```bash
gh release edit v2.5.0.1-dev -R okets/OrcaMCP --notes-file - <<'EOF'
## OrcaMCP v2.5.0.1-dev

Based on upstream OrcaSlicer 2.5.0-dev (color mixing, Flashforge Creator 5 / 5 Pro profiles).

### New
- Multi-material MCP tools: get_filaments, set_mixed_filament, delete_mixed_filament, set_object_filament, get_flush_volumes, set_flush_volumes, auto_calc_flush_volumes, get_toolchanger_config; apply_config now supports project settings and reports invalid keys.
- Flashforge Creator 5 / 5 Pro: direct send with material-station mapping, get_printer_status, printer_control (pause/resume/cancel/light/temperatures), list_printer_files, print_printer_file, discover_printers, add_physical_printer, and Device-tab monitoring. Uses the printer's open local API on port 8898; no Flashforge cloud, account, or closed network plugin.

### Install
macOS: open the DMG, drag to Applications. If Gatekeeper blocks it: `xattr -cr /Applications/OrcaMCP.app`.
Windows: run the installer (SmartScreen: More info → Run anyway).
Linux: `chmod +x` the AppImage and run it.
Then Preferences → MCP Clients → Connect for your AI client.
EOF
```

- [ ] **Step 5: Merge-back hygiene**

```bash
git checkout main && git merge upstream/main && git push origin main
git checkout mcp
```

---

## Self-review notes

- **Coverage of the three requested stages:** Stage 1 = Tasks 1.1–1.8 (sync, tools updated, multi-material tools, toolchanger settings reachable via `get_toolchanger_config` + `apply_config` + `get_valid_config_keys category=toolchanger`). Stage 2 = Tasks 2.1–2.8 (profile fix, API helpers with tests, host methods, printer tools, direct send, Device tab agent). Stage 3 = Tasks 3.1–3.4 (CI, packaging, docs, release).
- **Known verification gaps to close during execution, not assumptions:** exact public/private status of `Sidebar::update_mixed_filament_list`, `update_dynamic_filament_list`, `delete_mixed_filament_at`; the name of `PhysicalPrinterCollection::get_selected_printer_config`; the exact `IPrinterAgent` virtual list; the variable upstream uses for the Windows arch suffix in the CPack block; the on-disk test binary path. Each task names where to look.
- **Type consistency:** `MixedFilamentResult` (upstream struct) is the only mixed-filament input type across Tasks 1.5/1.6; `FlashforgeApi::PrinterStatus` is the only status type across Tasks 2.2/2.3/2.5/2.7; `resolve_print_host_config` + `make_print_host` are the only host-resolution entry points across 2.4/2.5/2.6.
