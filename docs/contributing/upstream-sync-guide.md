# Upstream Sync Guide

This document tracks the process of syncing OrcaMCP with upstream OrcaSlicer and serves as a guide for future syncs.

## Current Sync: March 2026

### Overview

| Metric | Value |
|--------|-------|
| Sync Date | 2026-03-06 |
| Upstream Commits Behind | 253 (main already synced) |
| MCP-Specific Commits | 74 |
| Upstream Version | v2.3.2-rc |
| Current OrcaMCP Version | 2.3.2.10 |

### Why This Sync

- GitHub Actions workers expire after 60 days of inactivity
- Upstream has significant new features (libvgcode G-code viewer)
- Bug fixes and new printer profiles

---

## Sync Strategy

### Branch Model

```
upstream/main (OrcaSlicer)
      |
      v
    main  --------> keeps in sync with OrcaSlicer
      |
      v
    mcp   --------> MCP work + upstream updates (default branch)
```

### Merge vs Rebase Decision

**Chosen: Merge** (not rebase)

Rationale:
- 390 upstream commits with merge commits - rebase would be messy
- 74 MCP commits would need to be replayed - high risk of errors
- Merge preserves both histories cleanly
- Force-push to default branch (mcp) would break collaborator workflows

---

## Execution Plan

### Phase 1: Preparation

- [x] Fetch latest upstream
- [x] Analyze commit count and potential conflicts
- [x] Create this sync plan document

### Phase 2: Update Main Branch

- [x] `git checkout main` - **Already done!**
- [x] `git merge upstream/main` - **Already synced**
- [x] Resolve any conflicts - **None**
- [x] `git push origin main` - **Already pushed**

> **Note:** Main was already synced with upstream. Proceeding directly to Phase 3.

### Phase 3: Merge into MCP Branch

- [x] `git checkout mcp`
- [x] `git merge main`
- [x] Resolve conflicts (see Expected Conflicts below)
- [ ] Test build compiles (CI running)
- [x] `git push origin mcp`

### Phase 4: Verification

- [ ] Build OrcaSlicer locally
- [ ] Verify MCP server starts (port 13618)
- [ ] Test basic MCP tools (get_scene_info, load_model)
- [ ] Verify libvgcode integration didn't break preview tools

### Phase 5: Release

- [ ] Bump version in `version.inc`
- [ ] Create and push tag
- [ ] Verify release workflow triggers
- [ ] Monitor build completion

---

## Expected Conflicts

Based on MCP changes, conflicts likely in:

| File | Reason | Resolution Strategy |
|------|--------|---------------------|
| `src/slic3r/GUI/GUI_App.cpp` | MCP initialization code | Keep MCP additions, accept upstream changes |
| `src/slic3r/GUI/HttpServer.cpp` | MCP route registration | Keep MCP routes, merge any HTTP server changes |
| `src/slic3r/GUI/HttpServer.hpp` | ResponseJson additions | Keep MCP additions |
| `src/libslic3r/PrintConfig.cpp` | 3MF loading fix | Keep our null-check fix |
| `CMakeLists.txt` | OrcaMCP source files | Add our files to updated list |
| `src/slic3r/GUI/GCodeViewer.*` | libvgcode rewrite | Accept upstream, verify preview tools work |

---

## New Upstream Features

### libvgcode (Major)

Port of PrusaSlicer 2.8.0's improved G-code viewer:
- New view types: ActualSpeed, ActualVolumetricFlowRate, LayerTime, FanSpeed, Temperature
- Potential new MCP tool: `set_gcode_view_type`
- May require updates to `render_plate_view` and `get_preview_base64`

### Other Changes

- Network plugin versioning system
- 20+ new printer profiles
- Bug fixes (UTF-8, adaptive infill, support settings)
- Gizmo improvements

---

## Progress Log

### 2026-03-06

**Status: Merge Complete - Awaiting Build Verification**

- Fetched upstream (253 commits to merge - main was already synced)
- Created this sync plan document
- Merged main into mcp branch
- Resolved 9 files with conflicts:
  - `src/slic3r/GUI/GUI_App.cpp` - Added both MCP includes and new upstream includes
  - `src/slic3r/GUI/HttpServer.cpp` - Kept ResponseJson, added new ResponseHtml
  - `src/slic3r/GUI/HttpServer.hpp` - Kept both response classes, added set_port/get_port
  - `src/slic3r/GUI/Preferences.cpp` - Added both MCP and DRC format includes
  - `build_release_vs.bat` - Kept vswhere PATH fix, added Ninja Multi-Config option
  - `resources/web/data/text.js` - Kept MCP translation strings with upstream improvements
  - `.github/workflows/build_orca.yml` - Kept OrcaMCP naming, improved conditions, skipped upstream deploy steps
- Next: Build verification and release

---

## Current Sync: September 2026

### Overview

| Metric | Value |
|--------|-------|
| Sync Date | 2026-09-07 |
| Upstream Head | `37e1582c4c` ("redesign filament_id (#15513)") |
| Upstream Version | 2.5.0-dev |
| Upstream Commits Behind | 1501 |
| Pre-merge `mcp` HEAD | `07d05f590bd5c629f72d7472aa8e74a51886aa13` |
| Pre-merge `main` HEAD | `a3f229f4061718581c8faf8577e55683757d6581` |
| New OrcaMCP Version | 2.5.0.1-dev |

### Why This Sync

- Upstream added the **color mixing** feature and the **Flashforge Creator 5 / 5 Pro** printer
  profiles that later OrcaMCP work builds on.
- 1501 commits of upstream fixes, including the new Python plugin host, `wxInspector`,
  bundled FFMPEG, and wxWidgets 3.3.2.

### Procedure Used

```bash
git checkout main && git merge --ff-only upstream/main && git push origin main
git checkout -b sync-upstream-2.5 mcp
git merge --no-ff main          # 10 conflicts
```

Because `main` was fast-forwarded first, `git diff main mcp -- <file>` no longer shows the
fork's own hunks. Use the merge-base instead:

```bash
git diff a3f229f406 07d05f590b -- <file>   # merge-base -> pre-merge mcp
```

### Conflicts and Resolutions (10 files)

| File | Resolution |
|------|------------|
| `.github/ISSUE_TEMPLATE/bug_report.yml` | `--ours` (kept the fork's issue template) |
| `README.md` | `--ours` (kept the OrcaMCP README) |
| `.github/workflows/build_orca.yml` | `--theirs` (upstream's multi-arch workflow; fork rebranding re-applied in a later task — release CI is knowingly broken until then) |
| `build_release_vs.bat` | `--theirs`, then re-applied the fork's `vswhere` PATH hunk after `set _START_TIME=%TIME%` |
| `resources/web/data/text.js` | `--theirs`, then re-inserted the fork's `t127`/`t128` ("Connect AI" / "Setup MCP agents") strings into all 15 language blocks |
| `version.inc` | Upstream's file with `SLIC3R_APP_NAME`/`SLIC3R_APP_KEY` = `OrcaMCP` (auto-merged) and `SoftFever_VERSION` set to `2.5.0.1-dev`; upstream's `SLIC3R_VERSION "02.08.01.55"` kept |
| `CMakeLists.txt` | Two CPack hunks. Kept upstream's new Windows arch-suffix block (`if (WIN32) ... string(APPEND CPACK_PACKAGE_FILE_NAME "_arm64"/"_x64")`) but with the OrcaMCP installer base name, summary and homepage URL; kept the fork's `CPACK_NSIS_INSTALLED_ICON_NAME` + `CPACK_NSIS_EXTRA_INSTALL_COMMANDS` desktop-shortcut block (upstream dropped the shortcut). The `file(COPY ... orcamcp-bridge.py ... tools_schema.py)` block auto-merged. |
| `src/CMakeLists.txt` | Two hunks. `OrcaSlicer_app_gui`: took upstream's new multi-property `set_target_properties` (adds `WIN32_EXECUTABLE`) with `OUTPUT_NAME "orca-mcp"`. Windows install: kept **both** upstream's `install(DIRECTORY "${CMAKE_PREFIX_PATH}/libpython/" ...)` and the fork's `install(DIRECTORY .../scripts/ ...)`. All other fork hunks (`orca-mcp` names, `ln -sf`, `MACOSX_BUNDLE_BUNDLE_NAME "OrcaMCP"`, the non-Windows scripts install) auto-merged. |
| `src/slic3r/GUI/GUI_App.cpp` | All four MCP hunks (OrcaMCP includes, `start_http_server()` + `ensure_bridge_script_copied()` in `post_init()`, `homepage_connectai` web command, `/mcp` routing in `start_http_server`) auto-merged. The single conflict was cosmetic: the splash text. Kept the fork's wording with upstream's new second argument — `scrn->SetText(_L("Loading configuration (this may take a couple of minutes)") + dots, 5);` |
| `src/slic3r/GUI/Preferences.hpp` | Kept the fork's `create_mcp_clients_page()` / `refresh_mcp_client_buttons()` declarations and dropped `create_shortcuts_page()`, which upstream removed (no definition remains anywhere in `src/`). All other fork members (`Widgets/Button.hpp`, the new constructor, `m_initial_tab`, `m_highlight_option`, `MCPClientUIElements`, `m_mcp_client_ui`) auto-merged. |

### Compile Fixes

**None were needed.** Every fork hunk in the auto-merged risk files
(`HttpServer.{hpp,cpp}`, `Plater.{hpp,cpp}`, `NotificationManager.{hpp,cpp}`,
`MsgDialog.cpp`, `GUI.{hpp,cpp}`, `Preferences.cpp`, `src/slic3r/CMakeLists.txt`) survived
the merge intact and compiled against upstream 2.5.0-dev without modification.
`src/slic3r/GUI/OrcaMCP/` was not touched.

### Build Environment Fixes (not source changes)

Two stale-state problems in the local `deps/` build tree, unrelated to the merge:

1. `deps/build/arm64/CMakeCache.txt` (and `build/arm64/CMakeCache.txt`) cached
   `GIT_EXECUTABLE=/opt/homebrew/bin/git`, which no longer exists, so `find_package(Git)`
   reported an empty version and `ExternalProject_Add` refused `--recursive`. Repointed the
   cache entries at `/usr/bin/git`.
2. Upstream added `deps/PNG/0002-clang19-macos.patch`, so the PNG patch step re-ran over an
   already-patched source tree and failed. Fixed by deleting
   `deps/build/arm64/dep_PNG-prefix/` to force a clean re-extract.

Watch for (2) on any dep whose patch set changes: `OCCT`, `OpenCV`, `OpenEXR`, `TBB` and
`PNG` all have `PATCH_COMMAND` steps.

### Build

Deps had to be rebuilt (`git diff --stat a3f229f406 upstream/main -- deps/` shows 27 files
changed, including new `python3`, `wxInspector`, `FFMPEG`, `Assimp` and `Eigen` projects).

```bash
./build_release_macos.sh -d -x -j 12   # deps
./build_release_macos.sh -s -x -j 12   # slicer: 767/767 targets, 0 failures, 8m31s
```

Result: `build/arm64/OrcaSlicer/OrcaSlicer.app`, `CFBundleName = OrcaMCP`,
`CFBundleShortVersionString = 2.5.0.1-dev`, with `Contents/Resources/scripts/`
containing `orcamcp-bridge.py` and `tools_schema.py`.

### Verification

- `get_server_info` — MCP server responds on `http://localhost:13618/mcp`.
- `get_scene_info` — returns the default plate, `active_warnings.count = 0`.
- Flashforge Creator 5 profiles are bundled: `Flashforge Creator 5 Pro 0.4 nozzle` (and the
  0.6/0.8 variants, plus the non-Pro models) are present in
  `Contents/Resources/profiles/Flashforge.json`. They do **not** appear in `get_presets`
  because that tool lists only presets for vendors installed in the user's configuration;
  Flashforge has to be added through the printer wizard first.

---

## Post-Sync Tasks

- [ ] Update CLAUDE.md if any MCP patterns changed
- [ ] Update docs/tools/reference.md if tools changed
- [ ] Consider adding `set_gcode_view_type` tool for libvgcode
- [ ] Update PROJECT_ROADMAP.md with sync completion

---

## Lessons Learned

### What Worked Well

- Merge strategy (vs rebase) preserved clean history
- Conflict resolution was straightforward - mostly "keep both" patterns
- Having a plan document helped track progress
- Main branch was already synced, reducing merge complexity

### What Could Be Improved

- **Workflow triggers**: `build_all.yml` didn't trigger on `mcp` branch pushes
  - Fixed by adding `mcp` to the branches list
- **Document expected conflicts earlier**: Could have prepared resolution strategies in advance
- **ASK before choosing verification strategy**: Don't assume "safe" approach is wanted
  - **Fast path**: Tag immediately after merge → release build either works or fails (~2h total)
  - **Safe path**: Run verification build first → then tag (~3.5h total)
  - Always ask which path the user prefers

### Recommendations for Future Syncs

1. **Sync frequency**: Do smaller, more frequent syncs (monthly) to reduce conflict volume
2. **Pre-sync checklist**:
   - Verify `build_all.yml` triggers on your branch
   - Record HEAD commits for rollback
   - Check upstream release notes for breaking changes
3. **Conflict patterns**: MCP files typically need "keep both" resolution:
   - Our MCP includes + their new includes
   - Our MCP classes + their new classes
4. **Test before release**: Always run full CI build before creating release tag

---

## Release Process

### Version Bump and Tag

```bash
# 1. Bump version in version.inc
# Current: set(SoftFever_VERSION "2.3.2.X")
# Change to next version

# 2. Commit the version bump
git add version.inc
git commit -m "Bump version to 2.3.2.Y"

# 3. Push commit
git push origin mcp

# 4. Create and push tag (triggers release.yml)
git tag v2.3.2.Y
git push origin v2.3.2.Y
```

### Release Workflow

The `release.yml` workflow:
1. Triggers on tags matching `v*`
2. Builds all platforms (Windows, macOS, Linux)
3. Creates GitHub Release with assets
4. Takes ~2-3 hours to complete

### Update Mechanism

OrcaMCP checks for updates from our GitHub releases:

```cpp
// src/libslic3r/AppConfig.cpp:42
static const std::string VERSION_CHECK_URL =
    "https://api.github.com/repos/okets/OrcaMCP/releases";
```

**Important**: Users won't see update prompts until the GitHub Release is **published** (not just tagged). The release workflow must complete and create the release with assets.

### Verifying Release Published

```bash
# Check if release exists
gh release view v2.3.2.Y -R okets/OrcaMCP

# List recent releases
gh release list -R okets/OrcaMCP --limit 5
```

---

## Rollback Plan

If the merge causes critical issues:

```bash
# Reset mcp to pre-merge state
git checkout mcp
git reset --hard HEAD~1  # or specific commit hash
git push origin mcp --force-with-lease

# If main was also affected
git checkout main
git reset --hard <pre-merge-commit>
git push origin main --force-with-lease
```

**Before merge, record:**
- Current mcp HEAD: `404f7bae304b2c60af1edba39a59f38bc116ca18`
- Current main HEAD: `d6761fedc67aa4ed70c3447638a29eb67f3b8121`
