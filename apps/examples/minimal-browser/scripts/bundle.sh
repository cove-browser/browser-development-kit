#!/usr/bin/env bash
# Builds the example and assembles .build/MinimalBrowser.app from the build product.
#
#   scripts/bundle.sh [debug|release]
#
# 1. bdk.toml is resolved to bdk.resolved.json by the bdk CLI: the packaged executable when
#    packages/cli/dist/bdk exists, otherwise the TypeScript sources through tsx.
# 2. swift build builds the executable.
# 3. The bundle gets the executable, Info.plist (NSPrincipalClass = BDKApplication) and bdk.resolved.json in
#    Resources, and `bdk build pages` packages Pages/ (the Settings page) and the SDK into it. Built against the
#    real runtime (Package.swift: .bdk/runtime exists and BDK_RUNTIME is not "stub"), it also gets the runtime's
#    framework in Contents/Frameworks, and `bdk build sign` re-signs the framework and its helpers as this
#    application: ad hoc, or with BDK_SIGN_IDENTITY (and BDK_SIGN_TEAM) when set. Against the stub it is signed
#    ad hoc as is.
set -euo pipefail

configuration="${1:-debug}"
project="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$project/../../.." && pwd)"
app="$project/.build/MinimalBrowser.app"

bdk() {
  if [[ -x "$root/packages/cli/dist/bdk" ]]; then
    "$root/packages/cli/dist/bdk" "$@"
  else
    (cd "$root" && pnpm exec tsx packages/cli/src/main.ts "$@")
  fi
}

bdk build config --project "$project"
framework="$root/.bdk/runtime/Chromium.app/Contents/Frameworks/Chromium Framework.framework"

cd "$project"
swift build --configuration "$configuration"
product="$(swift build --configuration "$configuration" --show-bin-path)/MinimalBrowser"

if [[ -d "$app" ]]; then chmod -R u+w "$app"; fi
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$product" "$app/Contents/MacOS/MinimalBrowser"
# swift build stamps the deployment target (14.0) as the SDK the binary was linked against, and macOS picks the
# window design from that: an old SDK gets the old traffic lights and chrome. Record the SDK it was really built
# with, as Xcode does. The deployment target is unchanged.
sdk="$(xcrun --sdk macosx --show-sdk-version)"
minos="$(vtool -show-build "$app/Contents/MacOS/MinimalBrowser" | awk '/minos/ {print $2; exit}')"
vtool -set-build-version macos "$minos" "$sdk" -replace -output "$app/Contents/MacOS/MinimalBrowser" \
  "$app/Contents/MacOS/MinimalBrowser"
cp "$project/Info.plist" "$app/Contents/Info.plist"
cp "$project/bdk.resolved.json" "$app/Contents/Resources/bdk.resolved.json"
# The bundled pages and the SDK they import; before signing, which covers Resources.
bdk build pages --bundle "$app" --project "$project"
# The executable links the framework only when it was built against the real runtime.
if otool -L "$product" | grep -q "Chromium Framework.framework"; then
  mkdir -p "$app/Contents/Frameworks"
  # A copy-on-write clone on APFS; the runtime store is read-only, the bundle must not be.
  cp -cR "$framework" "$app/Contents/Frameworks/"
  chmod -R u+w "$app/Contents/Frameworks"
  # Ad hoc by default. Each ad hoc build is a new code identity, so the Keychain asks again for the engine's
  # storage key after every rebuild; a real identity is one the Keychain recognises across rebuilds.
  sign=(--identity "${BDK_SIGN_IDENTITY:--}")
  if [[ -n "${BDK_SIGN_TEAM:-}" ]]; then sign+=(--team "$BDK_SIGN_TEAM"); fi
  bdk build sign --bundle "$app" "${sign[@]}"
else
  codesign --force --sign - "$app"
fi

echo "$app"
