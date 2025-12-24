# OrcaMCP Release & Update Process

---

## Versioning Scheme

```
X.Y.Z.N
│ │ │ └─ OrcaMCP release number (0 = sync only, 1+ = has OrcaMCP changes)
└─┴─┴─── OrcaSlicer version (always taken from upstream)
```

| Version | Meaning |
|---------|---------|
| `2.3.2.0` | Synced to OrcaSlicer 2.3.2, no OrcaMCP changes |
| `2.3.2.1` | OrcaSlicer 2.3.2 + OrcaMCP changes |
| `2.4.0.0` | Synced to OrcaSlicer 2.4.0 (4th number resets) |

### Version Update Rules

| Situation | Version Change |
|-----------|----------------|
| Sync to new upstream X.Y.Z | → `X.Y.Z.0` (reset 4th to 0) |
| OrcaMCP changes, same upstream | → increment 4th number |
| Sync + OrcaMCP changes together | → `X.Y.Z.1` |

---

## Upstream Sync Workflow

```mermaid
flowchart TD
    A[Check upstream version] --> B{New OrcaSlicer<br/>version?}
    B -->|Yes| C[git fetch upstream]
    B -->|No| D[No sync needed]
    C --> E[git checkout main<br/>git merge upstream/main]
    E --> F[git checkout mcp<br/>git merge main]
    F --> G{Conflict in<br/>version.inc?}
    G -->|Yes| H[Set version to X.Y.Z.0<br/>using upstream X.Y.Z]
    G -->|No| I[Continue]
    H --> J[git add version.inc<br/>git commit]
    I --> J
    J --> K[Build & Test]
    K --> L{Adding OrcaMCP<br/>changes?}
    L -->|Yes| M[Set version to X.Y.Z.1]
    L -->|No| N[Keep X.Y.Z.0]
    M --> O[Tag & Release]
    N --> O
```

### Sync Commands

```bash
# 1. Check versions
grep "SoftFever_VERSION" version.inc                    # Current
gh release view --repo SoftFever/OrcaSlicer             # Upstream

# 2. Fetch and merge
git fetch upstream
git checkout main && git merge upstream/main
git checkout mcp && git merge main

# 3. Resolve version.inc → set to X.Y.Z.0 (upstream version + .0)

# 4. Push
git push origin main mcp
```

---

## Release Workflow

```mermaid
flowchart TD
    A[Changes ready] --> B{Type of release?}

    B -->|Sync only| C[Set version X.Y.Z.0]
    B -->|OrcaMCP changes| D[Increment 4th number]
    B -->|Sync + changes| E[Set version X.Y.Z.1]

    C --> F[Update version.inc]
    D --> F
    E --> F

    F --> G[git commit -m 'Release X.Y.Z.N']
    G --> H[git tag -a vX.Y.Z.N]
    H --> I[git push origin mcp vX.Y.Z.N]
    I --> J[GitHub Actions builds]
    J --> K[Draft release created]
    K --> L[Review & Publish release]
    L --> M[Users see update in app]
```

### Release Commands

```bash
# 1. Set version in version.inc
# Edit: set(SoftFever_VERSION "X.Y.Z.N")

# 2. Commit and tag
git add -A
git commit -m "Release X.Y.Z.N"
git tag -a vX.Y.Z.N -m "Release X.Y.Z.N: description"
git push origin mcp vX.Y.Z.N

# 3. Wait for GitHub Actions (~15 min)
# 4. Publish draft release at github.com/okets/OrcaMCP/releases
```

---

## User Update Flow

```mermaid
sequenceDiagram
    participant User as OrcaMCP App
    participant GH as GitHub API

    User->>GH: GET /repos/okets/OrcaMCP/releases
    GH-->>User: [{tag_name: "v2.3.2.1", ...}]

    User->>User: Compare 2.3.2.0 vs 2.3.2.1

    alt New version available
        User->>User: Show update dialog
        User->>User: Open browser to release page
    else Up to date
        User->>User: Show "No updates"
    end
```

**API Endpoint:** `https://api.github.com/repos/okets/OrcaMCP/releases`

---

## GitHub Actions

The workflow (`.github/workflows/release.yml`) triggers on tag push:

```yaml
on:
  push:
    tags:
      - 'v*'
```

| Step | First Build | Subsequent Builds |
|------|-------------|-------------------|
| Tag pushed | instant | instant |
| Build Deps (cached) | 30-60 min each | **0s** (cached) |
| Build Linux | ~50 min | ~50 min |
| Build Windows | ~1 hour | ~1 hour |
| Build macOS (universal) | ~2 hours | ~2 hours |
| Create release | ~1 min | ~1 min |

**Notes:**
- All platforms build **in parallel**, so total wall time ≈ macOS time (~2 hours)
- macOS is slowest because it builds a **universal binary** (ARM + Intel)
- Dependencies are cached based on `hashFiles('deps/**')` - only rebuilt if deps change
- Large C++ codebase (~500K lines) means long compile times are unavoidable

---

## Quick Reference

### Check Versions
```bash
# Current OrcaMCP
grep "SoftFever_VERSION" version.inc

# Latest upstream
gh release view --repo SoftFever/OrcaSlicer --json tagName -q .tagName

# Compare
git fetch upstream && git show upstream/main:version.inc | grep SoftFever_VERSION
```

### Create Release
```bash
git tag -a v2.3.2.1 -m "Description" && git push origin v2.3.2.1
```

### Delete Tag (if mistake)
```bash
git tag -d v2.3.2.1
git push origin :refs/tags/v2.3.2.1
```

---

## Version History Example

```
v2.3.2.0  ← Synced to OrcaSlicer 2.3.2
v2.3.2.1  ← Added MCP tool
v2.3.2.2  ← Bug fixes
v2.4.0.0  ← Synced to OrcaSlicer 2.4.0 (reset)
v2.4.0.1  ← OrcaMCP improvements
v2.4.1.0  ← Synced to OrcaSlicer 2.4.1 (reset)
```
