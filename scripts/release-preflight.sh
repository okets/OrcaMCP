#!/usr/bin/env bash
#
# Release pre-flight. Run this BEFORE pushing a release tag.
#
# A release build takes about ninety minutes and most of what broke v2.5.0.1-dev -- five separate
# failed release runs on 2026-09-17 -- was detectable in seconds without building anything. This
# script runs every check that is, so the ninety minutes are only spent on things that genuinely
# need a build.
#
#   1. Fork customisations that upstream merges revert   (scripts/check-fork-customizations.sh)
#   2. Shellcheck, with CI's exact command and file set
#   3. Every workflow file parses as YAML
#   4. version.inc matches the tag you are about to push
#   5. The tag does not already exist locally or on the remote
#   6. Apple notarization credentials are usable        (optional; needs a local keychain profile)
#
# Usage: scripts/release-preflight.sh v2.5.0.2-dev
# Exit:  0 ready to tag, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.." || { echo "cannot reach the repository root" >&2; exit 1; }

TAG="${1:-}"
if [[ -z "$TAG" ]]; then
    echo "usage: $0 <tag>   e.g. $0 v2.5.0.2-dev" >&2
    exit 1
fi

FAIL=0
pass() { printf '  ok      %s\n' "$1"; }
fail() { printf '  FAIL    %s\n          %s\n' "$1" "$2"; FAIL=$((FAIL + 1)); }
skip() { printf '  skip    %s\n          %s\n' "$1" "$2"; }

echo "Release pre-flight for $TAG"
echo

# 1. Fork customisations ---------------------------------------------------------------------
if scripts/check-fork-customizations.sh >/tmp/preflight-fork.txt 2>&1; then
    pass "fork customisations intact"
else
    fail "fork customisations lost" "$(grep -E 'LOST|REVERTED|MISSING|NO SUCH' /tmp/preflight-fork.txt | head -5 | tr '\n' ';')"
fi

# 2. Shellcheck, exactly as CI runs it ---------------------------------------------------------
if command -v shellcheck >/dev/null; then
    if find . -not -name \*.md \( -path ./scripts/linux.d/\* -o -name \*.sh \) \
            -not -path "./build/*" -not -path "./deps/build/*" -print0 \
        | xargs -0 shellcheck >/tmp/preflight-sc.txt 2>&1; then
        pass "shellcheck clean (CI's own command)"
    else
        fail "shellcheck" "$(grep -E '^In ' /tmp/preflight-sc.txt | head -3 | tr '\n' ';')"
    fi
else
    skip "shellcheck" "not installed; CI will run it. brew install shellcheck"
fi

# 3. Workflow YAML parses ----------------------------------------------------------------------
yaml_ok=1
for f in .github/workflows/*.yml; do
    if command -v ruby >/dev/null; then
        ruby -ryaml -e "YAML.load_file('$f')" 2>/dev/null || { yaml_ok=0; fail "yaml" "$f does not parse"; }
    elif python3 -c "import yaml" 2>/dev/null; then
        python3 -c "import yaml; yaml.safe_load(open('$f'))" 2>/dev/null || { yaml_ok=0; fail "yaml" "$f does not parse"; }
    else
        skip "yaml" "neither ruby nor python yaml available"; yaml_ok=2; break
    fi
done
[[ $yaml_ok -eq 1 ]] && pass "all workflow files parse"

# 4. version.inc matches the tag ---------------------------------------------------------------
# release.yml downloads artifacts by exact name built from SoftFever_VERSION; a mismatch fails
# AFTER a full build. Two of the fork's earlier releases died exactly this way.
want="${TAG#v}"
have=$(sed -n 's/^set(SoftFever_VERSION "\(.*\)")$/\1/p' version.inc)
if [[ "$have" == "$want" ]]; then
    pass "version.inc is $have, matching $TAG"
else
    fail "version.inc says \"$have\" but the tag is $TAG" "release.yml names artifacts from version.inc and will not find them"
fi

# 5. Tag is free ------------------------------------------------------------------------------
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    fail "tag $TAG already exists locally" "delete it (git tag -d $TAG) or pick the next version"
elif git ls-remote --tags origin "refs/tags/$TAG" | grep -q .; then
    fail "tag $TAG already exists on origin" "a previous attempt left it; delete with: git push origin :refs/tags/$TAG"
else
    pass "tag $TAG is unused"
fi

# 6. Apple notarization credentials -----------------------------------------------------------
# The notary service refuses every submission with HTTP 403 until the Account Holder accepts a
# new Program License Agreement. That failure arrives at the END of a ninety-minute build. A
# history query hits the same check in a couple of seconds. Needs a stored profile:
#   xcrun notarytool store-credentials release-preflight --apple-id <id> --team-id <team>
if command -v xcrun >/dev/null && xcrun notarytool --help >/dev/null 2>&1; then
    if out=$(xcrun notarytool history --keychain-profile release-preflight 2>&1); then
        pass "Apple notary service accepts our credentials"
    elif grep -q "release-preflight" <<<"$out" && grep -qi "not found\|No keychain\|profile" <<<"$out"; then
        skip "notarization" "no 'release-preflight' keychain profile; see the comment above to store one"
    elif grep -q "403" <<<"$out"; then
        fail "notarization" "HTTP 403: an Apple agreement needs accepting at developer.apple.com before this will notarize"
    else
        fail "notarization" "$(head -2 <<<"$out" | tr '\n' ' ')"
    fi
else
    skip "notarization" "no notarytool here; only checkable on a Mac with Xcode"
fi

echo
if [[ $FAIL -eq 0 ]]; then
    cat <<EOF
Ready to tag. What this did NOT check, because it needs a build:
  - that the DMG actually contains ${TAG%%-*}'s OrcaMCP.app and is signed (verify on the artifact)
  - that the MSIX packaging step runs (build_all.yml skips it; only a release run exercises it)
EOF
    exit 0
fi
echo "$FAIL problem(s). Fix them before tagging -- each one otherwise costs a ninety-minute build."
exit 1
