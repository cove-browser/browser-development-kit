#!/usr/bin/env bash
# Builds the example and assembles .build/MinimalBrowser.app from the build product.
#
#   scripts/bundle.sh [debug|release]
#
# 1. bdk.toml is resolved to bdk.resolved.json by the bdk CLI: the packaged executable when
#    packages/cli/dist/bdk exists, otherwise the TypeScript sources through tsx.
# 2. swift build builds the executable.
# 3. The bundle gets the executable, Info.plist (NSPrincipalClass = BDKApplication) and bdk.resolved.json in
#    Resources, and is signed ad hoc. There is no runtime inside it: the example runs against the stub
#    runtime (Sources/MinimalBrowser/StubRuntime.swift), so `bdk build sign` has nothing to re-sign yet.
set -euo pipefail

configuration="${1:-debug}"
project="$(cd "$(dirname "$0")/.." && pwd)"
root="$(cd "$project/../../.." && pwd)"
app="$project/.build/MinimalBrowser.app"

if [[ -x "$root/packages/cli/dist/bdk" ]]; then
  "$root/packages/cli/dist/bdk" build config --project "$project"
else
  (cd "$root" && pnpm exec tsx packages/cli/src/main.ts build config --project "$project")
fi

cd "$project"
swift build --configuration "$configuration"
product="$(swift build --configuration "$configuration" --show-bin-path)/MinimalBrowser"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$product" "$app/Contents/MacOS/MinimalBrowser"
cp "$project/Info.plist" "$app/Contents/Info.plist"
cp "$project/bdk.resolved.json" "$app/Contents/Resources/bdk.resolved.json"
codesign --force --sign - "$app"

echo "$app"
