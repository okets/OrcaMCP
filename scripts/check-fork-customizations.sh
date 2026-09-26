#!/usr/bin/env bash
#
# Asserts the fork customisations that an upstream merge is most likely to silently revert.
#
# This exists because of a real incident. The September 2026 sync resolved
# .github/workflows/build_orca.yml with `--theirs` and a note in the sync guide saying the fork
# rebranding would be "re-applied in a later task -- release CI is knowingly broken until then".
# The re-apply never happened. v2.5.0.1-dev shipped a DMG containing OrcaSlicer.app, which
# overwrote users' real Orca Slicer, and it was ad-hoc signed because the signing step was gated on
# upstream's repository name, so Gatekeeper refused to open it.
#
# A note in a document did not prevent that. A failing build will.
#
# Each check names what it protects and why, so a future merge that trips one knows what to restore
# rather than being tempted to delete the check.
#
# Usage: scripts/check-fork-customizations.sh
# Exit:  0 all invariants hold, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.." || { echo "cannot reach the repository root" >&2; exit 1; }

FAILURES=0

# check <description> <file> <extended-regex> <why it matters>
check() {
    local desc="$1" file="$2" pattern="$3" why="$4"
    if [[ ! -f "$file" ]]; then
        printf '  MISSING FILE  %s\n                %s\n' "$desc" "$file"
        FAILURES=$((FAILURES + 1))
        return
    fi
    if grep -Eq -- "$pattern" "$file"; then
        printf '  ok            %s\n' "$desc"
    else
        printf '  LOST          %s\n                in %s\n                %s\n' "$desc" "$file" "$why"
        FAILURES=$((FAILURES + 1))
    fi
}

# check_absent is for things upstream reintroduces that must NOT be here.
check_absent() {
    local desc="$1" file="$2" pattern="$3" why="$4"
    if [[ -f "$file" ]] && grep -Eq -- "$pattern" "$file"; then
        printf '  REVERTED      %s\n                in %s\n                %s\n' "$desc" "$file" "$why"
        FAILURES=$((FAILURES + 1))
    else
        printf '  ok            %s\n' "$desc"
    fi
}

# check_step_condition <description> <file> <step name> <extended-regex> <why>
# Reads the `if:` belonging to a named workflow step, so a check cannot be fooled by an unrelated
# step elsewhere in the same file.
step_condition() {
    awk -v step="$2" '
        index($0, "- name: " step) { found = 1; next }
        found && /^[[:space:]]*if:/  { sub(/^[[:space:]]*if:[[:space:]]*/, ""); print; exit }
        found && /- name:/          { exit }
    ' "$1"
}

check_condition() {
    local desc="$1" file="$2" step="$3" pattern="$4" why="$5" want_absent="${6:-}"
    local cond; cond="$(step_condition "$file" "$step")"
    if [[ -z "$cond" ]]; then
        printf '  NO SUCH STEP  %s\n                step "%s" not found in %s\n' "$desc" "$step" "$file"
        FAILURES=$((FAILURES + 1))
        return
    fi
    local hit=1
    grep -Eq -- "$pattern" <<<"$cond" || hit=0
    if [[ -n "$want_absent" ]]; then
        (( hit == 0 )) && { printf '  ok            %s\n' "$desc"; return; }
    else
        (( hit == 1 )) && { printf '  ok            %s\n' "$desc"; return; }
    fi
    printf '  LOST          %s\n                step "%s" condition is: %s\n                %s\n' "$desc" "$step" "$cond" "$why"
    FAILURES=$((FAILURES + 1))
}

# check_count <description> <file> <extended-regex> <expected count> <why>
# For invariants that must appear in several places -- the signed and unsigned DMG paths both
# rename the bundle, and a merge can easily revert only one of them.
check_count() {
    local desc="$1" file="$2" pattern="$3" want="$4" why="$5"
    local got; got=$(grep -Ec -- "$pattern" "$file" 2>/dev/null || echo 0)
    if [[ "$got" == "$want" ]]; then
        printf '  ok            %s\n' "$desc"
    else
        printf '  LOST          %s\n                in %s: found %s, expected %s\n                %s\n' \
            "$desc" "$file" "$got" "$want" "$why"
        FAILURES=$((FAILURES + 1))
    fi
}

echo "Fork customisations that upstream merges tend to revert:"
echo

# --- Identity -------------------------------------------------------------------------------
check "app name is OrcaMCP" \
    version.inc \
    'set\(SLIC3R_APP_NAME "OrcaMCP"\)' \
    "Drives the bundle name, installer name and window title."

check "app key is OrcaMCP" \
    version.inc \
    'set\(SLIC3R_APP_KEY "OrcaMCP"\)' \
    "Drives CFBundleName and the Linux desktop entry id."

check "unix binary is orca-mcp" \
    version.inc \
    'set\(SLIC3R_APP_CMD "orca-mcp"\)' \
    "The AppImage and desktop entry both look for this exact name."

# --- macOS packaging ------------------------------------------------------------------------
check "macOS bundle identifier is the fork's own" \
    src/CMakeLists.txt \
    'MACOSX_BUNDLE_GUI_IDENTIFIER "com\.orcamcp\.OrcaMCP"' \
    "Sharing com.orcaslicer.OrcaSlicer makes macOS treat this and the real Orca Slicer as one app."

check "bundle plist template takes the identifier from CMake" \
    cmake/modules/MacOSXBundleInfo.plist.in \
    '<string>[$]\{MACOSX_BUNDLE_GUI_IDENTIFIER\}</string>' \
    "Upstream hardcodes com.orcaslicer.OrcaSlicer here; a merge that restores it silently wins."

# Two code paths build this DMG -- signed and unsigned -- and both must rename.
check_count "DMG renames the bundle to the fork's name (both paths)" \
    .github/workflows/build_orca.yml \
    'OrcaSlicer\.app .*\$\{\{ env\.app_name \}\}_dmg/\$\{\{ env\.app_name \}\}\.app' \
    2 \
    "CMake always builds OrcaSlicer.app. Shipping that name installs over a user's real Orca Slicer."

check_absent "DMG does not ship the upstream bundle name" \
    .github/workflows/build_orca.yml \
    'srcfolder .*OrcaSlicer_dmg' \
    "Upstream's DMG folder means the rename above was lost."

check_condition "macOS signing runs in this repository" \
    .github/workflows/build_orca.yml "Sign app and notary" \
    "github\.repository == 'okets/OrcaMCP'" \
    "This fork has its own Apple credentials. Gated on upstream's repo, every build ships unsigned."

check_condition "signing is not gated on a branch list" \
    .github/workflows/build_orca.yml "Sign app and notary" \
    "refs/heads/" \
    "Releases here are tag-driven; refs/tags/* matches no branch, so signing would skip." \
    absent

check_condition "unsigned DMG fallback is the inverse of signing" \
    .github/workflows/build_orca.yml "Create DMG without notary" \
    "github\.repository != 'okets/OrcaMCP'" \
    "If both conditions can be true, the unsigned DMG overwrites the signed one."

check_count "signing quotes its password secrets" \
    .github/workflows/build_orca.yml \
    'security (create-keychain|unlock-keychain) -p "[$]KEYCHAIN_PASSWORD"' \
    2 \
    "Unquoted, a secret with a space word-splits and unlock-keychain dies with its usage message."

check_count "certificate import quotes its password" \
    .github/workflows/build_orca.yml \
    '(-P|-k) "[$]P12_PASSWORD"' \
    2 \
    "Same word-splitting failure, one step later, after the keychain is already unlocked."

# --- Release workflow -----------------------------------------------------------------------
check "release passes the Windows arch" \
    .github/workflows/release.yml \
    'arch: x64' \
    "Feeds the MSIX script, which declares ValidateSet(x64,arm64); empty fails the whole release."

check_absent "release does not upload assets through the gh-release action" \
    .github/workflows/release.yml \
    '^          files: \|' \
    "That upload failed on 6 of 7 runs on the 360 MB DMG; assets go up via gh release upload with retries."

check "release uploads assets with the gh CLI" \
    .github/workflows/release.yml \
    'gh release upload "[$]TAG"' \
    "Streams and retries; the only upload path that has moved the macOS image reliably."

check "release verifies every asset is attached" \
    .github/workflows/release.yml \
    'name: Verify release assets' \
    "Without it a release missing an installer still shows green."

check "release passes the Windows compiler" \
    .github/workflows/release.yml \
    'compiler: clang' \
    "Without it the released binary is built differently from the one CI tested."

# --- MCP surface ----------------------------------------------------------------------------
check "Connect AI strings are present" \
    resources/web/data/text.js \
    't12[78]' \
    "The home page's Connect AI / Setup MCP agents entry points."

check "the bridge script is packaged" \
    CMakeLists.txt \
    'orcamcp-bridge\.py' \
    "Without it an installed OrcaMCP cannot be driven by an agent at all."

check "the bridge's tool list is packaged" \
    CMakeLists.txt \
    'orcamcp_tools\.json' \
    "The bridge reads every tool's text from it, start_orca included; without it an agent cannot start the app."

# The guard can only catch a bad merge if CI still runs it, and the job that runs it lives in a
# file upstream also edits -- so the guard is exactly as revertible as everything it protects.
# It checks itself last, because a merge that drops this job makes every check above silent.
check "CI still runs this guard" \
    .github/workflows/build_all.yml \
    'check-fork-customizations\.sh' \
    "Without the job, an upstream merge can revert every invariant above and CI stays green."

check "the fork's branch still triggers CI" \
    .github/workflows/build_all.yml \
    '^[[:space:]]+- mcp$' \
    "Upstream's copy builds only its own branches, so a --theirs resolution stops building this fork."

check "WinGet publishing stays upstream's" \
    .github/workflows/winget_updater.yml \
    "github.repository == 'OrcaSlicer/OrcaSlicer'" \
    "Ungated, this fork publishes its installers into upstream's WinGet package, and without a token it just fails every release."

echo
if [[ $FAILURES -eq 0 ]]; then
    echo "All fork customisations intact."
    exit 0
fi

cat <<EOF

$FAILURES fork customisation(s) lost.

This almost always means an upstream merge resolved a file with --theirs. Restore the listed
items rather than relaxing the check: each one shipped a broken release at least once.
docs/contributing/upstream-sync-guide.md has the history.
EOF
exit 1
