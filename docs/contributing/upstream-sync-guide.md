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
