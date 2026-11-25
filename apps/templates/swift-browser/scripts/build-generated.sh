#!/usr/bin/env bash
# Generates an application from this template with the packaged bdk, then builds it and runs its tests
# with xcodebuild, against the stub runtime and the BDK package of this repository:
#
#   apps/templates/swift-browser/scripts/build-generated.sh [<work directory>]
#
# The project has plain starter pages, so the bundle phase packages pages too. Run by
# template-swift-browser:build, which packages the CLI first. Nothing is written into the repository.
set -euo pipefail

root="$(cd "$(dirname "$0")/../../../.." && pwd)"
bdk="$root/packages/cli/dist/bdk"
work="${1:-$(mktemp -d "${TMPDIR:-/tmp}/bdk-template-XXXXXX")}"
if [[ ! -x "$bdk" ]]; then
  echo "error: $bdk does not exist; run pnpm cli:package first." >&2
  exit 1
fi

rm -rf "$work/Generated" "$work/DerivedData"
mkdir -p "$work"
(cd "$work" && "$bdk" init Generated --name "Generated Browser" --bundle-id dev.bdk.templates.generated \
  --pages plain --starter-pages new-tab,settings --automation --package-manager pnpm --no-install \
  --no-git --bdk-package "$root" --yes)
# The automation scripts are bundled with the project's Rolldown. Link the repository's copy, the version the
# generated package.json pins, instead of installing it, so the build needs no network.
mkdir -p "$work/Generated/node_modules"
ln -s "$(cd "$root/packages/cli/node_modules/rolldown" && pwd -P)" "$work/Generated/node_modules/rolldown"

xcode=(xcodebuild -project "$work/Generated/GeneratedBrowser.xcodeproj" -scheme GeneratedBrowser
  -derivedDataPath "$work/DerivedData" BDK_CLI="$bdk")
"${xcode[@]}" build -quiet
"${xcode[@]}" test -quiet

app="$work/DerivedData/Build/Products/Debug/GeneratedBrowser.app"
for file in Contents/Resources/bdk.resolved.json Contents/Resources/Pages/NewTab/index.html \
  Contents/Resources/BDK/Automation/hello.js; do
  if [[ ! -f "$app/$file" ]]; then
    echo "error: the built application has no $file" >&2
    exit 1
  fi
done
codesign --verify --deep --strict "$app"
echo "$app"
