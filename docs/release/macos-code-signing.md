# macOS Code Signing Setup for OrcaMCP

This guide covers setting up Apple code signing and notarization for OrcaMCP releases.

---

## Prerequisites

- Apple Developer Program membership ($99/year) - **APPROVED**
- Access to GitHub repository secrets
- macOS with Keychain Access

---

## Step 1: Create Developer ID Certificate

1. Go to https://developer.apple.com/account/resources/certificates/list
2. Click **+** to create a new certificate
3. Select **Developer ID Application** (for distributing outside App Store)
4. You'll need to create a Certificate Signing Request (CSR):
   - Open **Keychain Access** on your Mac
   - Menu: **Keychain Access → Certificate Assistant → Request a Certificate from a Certificate Authority**
   - Enter your email, select "Saved to disk", click Continue
   - Save the `.certSigningRequest` file
5. Upload the CSR to Apple Developer portal
6. Download the certificate (`.cer` file)
7. Double-click to install in Keychain

---

## Step 2: Export Certificate as .p12

1. Open **Keychain Access**
2. In the left sidebar, select **login** keychain, then **My Certificates** category
3. Find your certificate: `Developer ID Application: Your Name (XXXXXXXXXX)`
4. Right-click → **Export "Developer ID Application: ..."**
5. Choose format: **Personal Information Exchange (.p12)**
6. Save the file (e.g., `developer_id.p12`)
7. Set a strong password — save this for `P12_PASSWORD`

---

## Step 3: Get Your Team ID

1. Go to https://developer.apple.com/account
2. Click **Membership details** (or look in the top right)
3. Find **Team ID** — a 10-character alphanumeric code (e.g., `ABC123XYZ9`)

---

## Step 4: Create App-Specific Password

Apple requires an app-specific password for notarization (2FA requirement).

1. Go to https://appleid.apple.com/account/manage
2. Sign in with your Apple ID
3. Under **Sign-In and Security**, click **App-Specific Passwords**
4. Click **+** to generate a new password
5. Name it: `OrcaMCP Notarization`
6. Copy the generated password (format: `xxxx-xxxx-xxxx-xxxx`)
7. Save this for `APP_PWD`

---

## Step 5: Base64 Encode the Certificate

```bash
# Encode the .p12 certificate
base64 -i ~/path/to/developer_id.p12 | pbcopy

# The encoded string is now in your clipboard
# It will be a long string of characters
```

---

## Step 6: Add GitHub Secrets

Go to: https://github.com/okets/OrcaMCP/settings/secrets/actions

Click **New repository secret** for each:

| Secret Name | Description | Example Value |
|-------------|-------------|---------------|
| `BUILD_CERTIFICATE_BASE64` | Base64-encoded .p12 certificate | (long base64 string) |
| `P12_PASSWORD` | Password set when exporting .p12 | `your-p12-password` |
| `KEYCHAIN_PASSWORD` | Temporary keychain password (can be anything) | `temp-keychain-pwd` |
| `MACOS_CERTIFICATE_ID` | Full certificate name from Keychain | `Developer ID Application: Your Name (ABC123XYZ9)` |
| `TEAM_ID` | 10-character Apple Team ID | `ABC123XYZ9` |
| `APPLE_DEV_ACCOUNT` | Your Apple ID email | `you@example.com` |
| `APP_PWD` | App-specific password from Step 4 | `xxxx-xxxx-xxxx-xxxx` |

---

## Step 7: Update GitHub Workflow

The workflow currently only signs for `OrcaSlicer/OrcaSlicer`. Update it to sign for `okets/OrcaMCP`:

In `.github/workflows/build_orca.yml`, change line 111 from:
```yaml
if: github.repository == 'OrcaSlicer/OrcaSlicer' && (github.ref == 'refs/heads/main' || startsWith(github.ref, 'refs/heads/release/')) && inputs.os == 'macos-14'
```

To:
```yaml
if: github.repository == 'okets/OrcaMCP' && inputs.os == 'macos-14'
```

This enables signing for all OrcaMCP builds (not just main branch).

---

## Step 8: Test the Signing

1. Bump version in `version.inc`
2. Commit and push
3. Create a new tag: `git tag -a v2.3.2.4 -m "Test code signing"`
4. Push the tag: `git push origin v2.3.2.4`
5. Wait for build (~3 hours)
6. Download the DMG and verify:
   ```bash
   # Check if signed
   codesign -dv --verbose=4 /Applications/OrcaMCP.app

   # Check notarization
   spctl -a -v /Applications/OrcaMCP.app
   ```

Expected output for signed & notarized app:
```
/Applications/OrcaMCP.app: accepted
source=Notarized Developer ID
```

---

## Troubleshooting

### "Certificate not found" error in GitHub Actions
- Verify `MACOS_CERTIFICATE_ID` matches exactly what's in Keychain
- Check that the certificate hasn't expired

### "Unable to notarize" error
- Verify `APP_PWD` is correct (app-specific password, not Apple ID password)
- Check `APPLE_DEV_ACCOUNT` is the correct Apple ID
- Ensure `TEAM_ID` is correct

### "Code signature invalid" on user's Mac
- The app may have been modified after signing
- Re-download from GitHub releases

---

## Certificate Renewal

Developer ID certificates are valid for 5 years. Set a reminder to renew before expiration.

To check expiration:
```bash
security find-certificate -c "Developer ID Application" -p | openssl x509 -noout -enddate
```

---

## Summary Checklist

- [x] Apple Developer Program approved
- [x] Developer ID Application certificate created
- [x] Certificate exported as .p12
- [x] Team ID noted (9PCJMHHHK6)
- [x] App-specific password created
- [x] Certificate base64 encoded
- [x] All 7 GitHub secrets added
- [x] Workflow updated for okets/OrcaMCP
- [x] Test build successful (v2.3.2.8)
- [x] Signed app verified with `spctl`

---

## Local Signing (Optional)

For faster iteration (~5 minutes vs 3 hours on GitHub), use the local signing script.

### One-time setup

Store your notarization credentials in the macOS Keychain:

```bash
xcrun notarytool store-credentials "local-notary" \
    --apple-id "your@email.com" \
    --team-id "9PCJMHHHK6"
# Enter app-specific password when prompted
```

### Usage

```bash
# Sign an existing app bundle
./scripts/sign-and-notarize.sh /Applications/OrcaMCP.app

# Sign from build directory
./scripts/sign-and-notarize.sh build/arm64/src/Release/OrcaSlicer.app
```

The script will:
1. Sign the app bundle with your Developer ID certificate
2. Create a DMG with Applications symlink
3. Sign the DMG
4. Submit to Apple for notarization (2-10 min wait)
5. Staple the notarization ticket
6. Verify everything passes Gatekeeper

Output: `OrcaMCP.dmg` in current directory, fully signed and notarized.
