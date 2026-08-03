#!/bin/sh
#
# Sign shinogi.app with a Developer ID Application certificate.
#
#   IDENTITY="Developer ID Application: ..." tools/sign-macos.sh <app>
#
# UNVERIFIED: written without access to a Mac.
#
# Order matters and is the usual source of "it signed fine and still
# will not launch": every nested binary must be signed before the bundle
# that contains it, because signing the outer bundle seals a hash of
# what is inside. Sign inside-out, and sign the bundle last.
#
# The hardened runtime is required for notarisation. QEMU generates code
# at runtime, so it needs the JIT entitlement or it will be killed on
# launch by the very hardening that let it be notarised.
#
set -eu

APP="${1:?usage: sign-macos.sh <app bundle>}"
IDENTITY="${IDENTITY:?IDENTITY not set}"
ENTITLEMENTS="$(dirname "$0")/macos-entitlements.plist"

[ -d "$APP" ] || { echo "no app bundle at $APP" >&2; exit 2; }
[ -f "$ENTITLEMENTS" ] || { echo "no entitlements at $ENTITLEMENTS" >&2; exit 2; }

echo "signing $APP as: $IDENTITY"

# Inside-out: dylibs and helper binaries first.
find "$APP/Contents/MacOS" -type f \( -name '*.dylib' -o -perm -111 \) \
     ! -name 'shinogi' -print0 |
while IFS= read -r -d '' f
do
    codesign --force --timestamp --options runtime \
             --entitlements "$ENTITLEMENTS" \
             --sign "$IDENTITY" "$f"
done

# Then the bundle itself.
codesign --force --timestamp --options runtime \
         --entitlements "$ENTITLEMENTS" \
         --sign "$IDENTITY" "$APP"

echo "=== verifying ==="
codesign --verify --deep --strict --verbose=2 "$APP"

# This is the check that predicts whether Gatekeeper will accept it.
# It reports "rejected" until the app has been notarised and stapled,
# which is expected at this stage rather than a failure.
spctl --assess --type execute --verbose=2 "$APP" || \
    echo "(spctl rejects an unnotarised build - expected before notarisation)"
