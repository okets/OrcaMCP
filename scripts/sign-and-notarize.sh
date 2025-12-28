#!/bin/bash
#
# Local macOS Code Signing and Notarization Script for OrcaMCP
#
# This script signs and notarizes an OrcaMCP.app bundle locally,
# allowing you to test the signing flow in ~5-15 minutes instead
# of waiting 3 hours for GitHub Actions.
#
# Prerequisites:
#   - Developer ID Application certificate in keychain
#   - Notarization credentials stored (run once):
#     xcrun notarytool store-credentials "local-notary" \
#         --apple-id "your@email.com" --team-id "9PCJMHHHK6"
#
# Usage:
#   ./scripts/sign-and-notarize.sh /path/to/OrcaMCP.app
#
set -e

# Configuration
CERTIFICATE_ID="Developer ID Application: Hanan Vaknin (9PCJMHHHK6)"
KEYCHAIN_PROFILE="local-notary"

# Entitlements file (same directory as this script)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENTITLEMENTS="$SCRIPT_DIR/disable_validation.entitlements"

if [ ! -f "$ENTITLEMENTS" ]; then
    echo "Error: Entitlements file not found: $ENTITLEMENTS"
    exit 1
fi

# Input: path to .app bundle
APP_PATH="$1"
if [ -z "$APP_PATH" ]; then
    echo "Usage: $0 /path/to/OrcaMCP.app"
    echo ""
    echo "Examples:"
    echo "  $0 /Applications/OrcaMCP.app"
    echo "  $0 build/arm64/src/Release/OrcaSlicer.app"
    exit 1
fi

if [ ! -d "$APP_PATH" ]; then
    echo "Error: App bundle not found: $APP_PATH"
    exit 1
fi

APP_NAME=$(basename "$APP_PATH" .app)
DMG_NAME="${APP_NAME}.dmg"
WORK_DIR="$(pwd)"

echo "========================================"
echo "OrcaMCP Code Signing & Notarization"
echo "========================================"
echo "App: $APP_PATH"
echo "Certificate: $CERTIFICATE_ID"
echo "Keychain Profile: $KEYCHAIN_PROFILE"
echo "Output: $WORK_DIR/$DMG_NAME"
echo "========================================"
echo ""

# Step 1: Sign the app
echo "[1/6] Signing app bundle..."
codesign --deep --force --verbose --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" \
    --sign "$CERTIFICATE_ID" \
    "$APP_PATH"
echo "    Done."
echo ""

# Step 2: Verify signature
echo "[2/6] Verifying signature..."
codesign --verify --deep --strict --verbose=2 "$APP_PATH"
echo "    Done."
echo ""

# Step 3: Create DMG
echo "[3/6] Creating DMG..."
rm -rf /tmp/dmg_staging
mkdir -p /tmp/dmg_staging
cp -R "$APP_PATH" /tmp/dmg_staging/
ln -sfn /Applications /tmp/dmg_staging/Applications

# Remove existing DMG if present
rm -f "$WORK_DIR/$DMG_NAME"

hdiutil create -volname "$APP_NAME" -srcfolder /tmp/dmg_staging \
    -ov -format UDZO "$WORK_DIR/$DMG_NAME"
rm -rf /tmp/dmg_staging
echo "    Done."
echo ""

# Step 4: Sign DMG
echo "[4/6] Signing DMG..."
codesign --force --verbose --options runtime --timestamp \
    --sign "$CERTIFICATE_ID" \
    "$WORK_DIR/$DMG_NAME"
echo "    Done."
echo ""

# Step 5: Notarize
echo "[5/6] Submitting for notarization (this may take 2-10 minutes)..."
xcrun notarytool submit "$WORK_DIR/$DMG_NAME" \
    --keychain-profile "$KEYCHAIN_PROFILE" \
    --wait
echo "    Done."
echo ""

# Step 6: Staple
echo "[6/6] Stapling notarization ticket..."
xcrun stapler staple "$WORK_DIR/$DMG_NAME"
echo "    Done."
echo ""

# Final verification
echo "========================================"
echo "Verification"
echo "========================================"
spctl -a -t open --context context:primary-signature -v "$WORK_DIR/$DMG_NAME"
echo ""
echo "Success! Output: $WORK_DIR/$DMG_NAME"
echo ""
echo "To test, drag the DMG to a different folder and open it."
echo "It should NOT show any Gatekeeper warnings."
