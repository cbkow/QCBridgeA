#!/bin/zsh
# build-pkg.sh — the signed macOS installer for QCBridgeA.
#
#   dist/QCBridgeA-<version>-<arch>.pkg
#     └─ QCBridgeA-app.pkg (component, non-relocatable, pre/postinstall)
#          └─ /Applications/QCBridgeA.app
#
# Input: build/QCBridgeA.app (cmake --build build). The app is one static
# binary (the AJA SDK linked in; no Qt, no frameworks), so there is nothing
# to deploy or prune: the bundle is copied, smoke-tested, signed with the
# Developer ID Application identity (hardened runtime, timestamp) and the
# pkg with the Developer ID Installer identity. Same shape as QCBridgeN's.
# Unsigned when no identity is in the keychain (local testing only).
#
#   packaging/macos/build-pkg.sh [--notarize]
#
# --notarize submits the signed app to Apple first and staples it (the copy
# that lands in /Applications must carry its own ticket), then builds the
# pkg from the stapled app and notarizes and staples the pkg too. Two
# submissions, 2–15 minutes each. Needs the "QCView" notarytool keychain
# profile (QCBA_NOTARY_PROFILE overrides).
set -euo pipefail
NOTARIZE=0
[ "${1:-}" = "--notarize" ] && NOTARIZE=1
PROFILE="${QCBA_NOTARY_PROFILE:-QCView}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/packaging/macos"
BUILT_APP="${QCBA_APP:-$REPO/build/QCBridgeA.app}"
PKG_ID="com.qcview-app.QCBridgeA"
[ -d "$BUILT_APP" ] || { echo "ERROR: missing $BUILT_APP — cmake --build build first" >&2; exit 1; }
VERSION="$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$BUILT_APP/Contents/Info.plist")"
ARCH="$(uname -m)"
mkdir -p "$REPO/dist"
PKG="$REPO/dist/QCBridgeA-$VERSION-$ARCH.pkg"

STAGE="$(mktemp -d -t qcba-pkg)"
trap 'rm -rf "$STAGE"' EXIT
ROOT="$STAGE/root/Applications"
APP="$ROOT/QCBridgeA.app"
mkdir -p "$ROOT" "$STAGE/pkgs" "$STAGE/scripts" "$STAGE/resources"
echo "[pkg] staging payload (v$VERSION, $ARCH)"
ditto "$BUILT_APP" "$APP"
find "$APP" -name .DS_Store -delete
xattr -cr "$APP"
# Nothing but system libraries may be linked: a Homebrew or build-tree
# dylib would not be in the bundle.
if otool -L "$APP/Contents/MacOS/QCBridgeA" | tail -n +2 | grep -v -E '^\s+(/usr/lib/|/System/)' ; then
    echo "ERROR: the binary links a library outside /usr/lib and /System (see above)" >&2; exit 1
fi
# Smoke test the staged bundle before signing: it must start on its own.
# With no QCView running it waits for the rings and quits after a second.
"$APP/Contents/MacOS/QCBridgeA" --seconds 1 > "$STAGE/smoke.log" 2>&1 \
    || { echo "ERROR: the staged app does not run — $STAGE/smoke.log" >&2; cat "$STAGE/smoke.log" >&2; exit 1; }
grep -q "waiting for QCView" "$STAGE/smoke.log" || { echo "ERROR: the staged app did not reach the bus" >&2; cat "$STAGE/smoke.log" >&2; exit 1; }
echo "[pkg] staged app runs: $(head -1 "$STAGE/smoke.log")"

identity="$(security find-identity -v -p codesigning 2>/dev/null | grep -o '"Developer ID Application: [^"]*"' | head -1 | tr -d '"')"
if [ -n "$identity" ]; then
    echo "[pkg] codesign with: $identity"
    codesign --force --options runtime --timestamp --sign "$identity" --entitlements "$SRC/entitlements.plist" "$APP"
    codesign --verify --deep --strict "$APP"
    if [ "$NOTARIZE" = 1 ]; then
        xcrun notarytool history --keychain-profile "$PROFILE" >/dev/null 2>&1 \
            || { echo "ERROR: notarytool cannot use profile \"$PROFILE\"" >&2; exit 1; }
        echo "[pkg] notarizing the app (profile $PROFILE)"
        ditto -c -k --keepParent "$APP" "$STAGE/app.zip"
        xcrun notarytool submit "$STAGE/app.zip" --keychain-profile "$PROFILE" --wait --output-format json > "$STAGE/app-notary.json"
        app_status="$(/usr/bin/python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("status",""))' "$STAGE/app-notary.json")"
        app_id="$(/usr/bin/python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("id",""))' "$STAGE/app-notary.json")"
        if [ "$app_status" != "Accepted" ]; then
            echo "ERROR: app notarization $app_status (id $app_id):" >&2
            xcrun notarytool log "$app_id" --keychain-profile "$PROFILE" >&2 || true
            exit 1
        fi
        xcrun stapler staple "$APP"
        xcrun stapler validate "$APP"
        echo "[pkg] app notarized and stapled (id $app_id)"
    fi
else
    [ "$NOTARIZE" = 1 ] && { echo "ERROR: --notarize needs a Developer ID Application identity" >&2; exit 1; }
    echo "WARN: no Developer ID Application identity — UNSIGNED app (local testing only)" >&2
fi

cp "$SRC/scripts/preinstall"  "$STAGE/scripts/preinstall"
cp "$SRC/scripts/postinstall" "$STAGE/scripts/postinstall"
chmod 755 "$STAGE/scripts/preinstall" "$STAGE/scripts/postinstall"
cp "$SRC/welcome.txt" "$STAGE/resources/welcome.txt"
cp "$REPO/LICENSE" "$STAGE/resources/LICENSE"
pkgbuild --analyze --root "$STAGE/root" "$STAGE/components.plist" >/dev/null
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$STAGE/components.plist" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$STAGE/components.plist" 2>/dev/null \
        || /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$STAGE/components.plist"
    i=$((i + 1))
done
pkgbuild --root "$STAGE/root" \
         --component-plist "$STAGE/components.plist" \
         --identifier "$PKG_ID" \
         --version "$VERSION" \
         --install-location / \
         --scripts "$STAGE/scripts" \
         "$STAGE/pkgs/QCBridgeA-app.pkg" >/dev/null
sed "s/@VERSION@/$VERSION/g" "$SRC/distribution.xml" > "$STAGE/distribution.xml"
rm -f "$PKG"
installer_id="$(security find-identity -v 2>/dev/null | grep -o '"Developer ID Installer: [^"]*"' | head -1 | tr -d '"')"
if [ -n "$installer_id" ]; then
    productbuild --distribution "$STAGE/distribution.xml" --package-path "$STAGE/pkgs" \
                 --resources "$STAGE/resources" --sign "$installer_id" --timestamp "$PKG" >/dev/null
    pkgutil --check-signature "$PKG" | sed 's/^/  /' | head -3
else
    echo "WARN: no Developer ID Installer identity — UNSIGNED pkg (local testing only)" >&2
    productbuild --distribution "$STAGE/distribution.xml" --package-path "$STAGE/pkgs" \
                 --resources "$STAGE/resources" "$PKG" >/dev/null
fi
if [ "$NOTARIZE" = 1 ]; then
    [ -n "$installer_id" ] || { echo "ERROR: --notarize needs a Developer ID Installer identity" >&2; exit 1; }
    echo "[pkg] notarizing the pkg"
    xcrun notarytool submit "$PKG" --keychain-profile "$PROFILE" --wait --output-format json > "$STAGE/pkg-notary.json"
    pkg_status="$(/usr/bin/python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("status",""))' "$STAGE/pkg-notary.json")"
    pkg_id="$(/usr/bin/python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("id",""))' "$STAGE/pkg-notary.json")"
    if [ "$pkg_status" != "Accepted" ]; then
        echo "ERROR: pkg notarization $pkg_status (id $pkg_id):" >&2
        xcrun notarytool log "$pkg_id" --keychain-profile "$PROFILE" >&2 || true
        exit 1
    fi
    xcrun stapler staple "$PKG"
    xcrun stapler validate "$PKG"
    echo "[pkg] pkg notarized and stapled (id $pkg_id)"
fi
echo "[pkg] done: $PKG ($(du -sh "$PKG" | cut -f1))"
shasum -a 256 "$PKG"
