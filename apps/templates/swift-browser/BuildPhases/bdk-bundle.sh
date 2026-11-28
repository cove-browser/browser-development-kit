#!/usr/bin/env bash
# Run-script phase of the application target, after "Copy Bundle Resources" (sdk/cli.md section 8):
# `bdk build pages automation extensions embed`. They write into CODESIGNING_FOLDER_PATH, which Xcode sets.
# `extensions` copies the `[extensions.bundled]` directories into Contents/Resources, before signing; with none
# it does nothing. `embed` runs only when the runtime phase resolved a runtime (.bdk/runtime.json); with none the
# application runs on the stub runtime. Then the resolved configuration, which `bdk build automation` completes
# with the scripts' digests, is copied into the bundle's Resources, where BDKConfiguration.load() reads it.
set -euo pipefail
# bdk-steps.sh is next to this script; shellcheck runs from elsewhere and cannot follow it.
# shellcheck source=/dev/null
source "$(dirname "$0")/bdk-steps.sh"
steps=(pages automation extensions)
if [[ -f "${SRCROOT}/.bdk/runtime.json" ]]; then
  steps+=(embed)
else
  echo "warning: no runtime is resolved (.bdk/runtime.json), so \`bdk build embed\` is left out; the application runs on the stub runtime." >&2
fi
bdk_run_steps "${steps[@]}"
cp "${SRCROOT}/bdk.resolved.json" "${CODESIGNING_FOLDER_PATH}/Contents/Resources/bdk.resolved.json"
