#!/usr/bin/env bash
# Last run-script phase of the application target (sdk/cli.md section 8): `bdk build sign`. It reads the
# signing identity, team and entitlements from Xcode's build settings. BDK_SKIP_SIGNING=1 skips it.
#
# `bdk build sign` re-signs the runtime nested in Contents/Frameworks. Until `bdk build embed` puts one there,
# an application built from the template runs on the stub runtime, has nothing nested to re-sign, and keeps
# the signature Xcode gives it.
set -euo pipefail
# bdk-steps.sh is next to this script; shellcheck runs from elsewhere and cannot follow it.
# shellcheck source=/dev/null
source "$(dirname "$0")/bdk-steps.sh"
if ! compgen -G "${CODESIGNING_FOLDER_PATH}/Contents/Frameworks/*.framework" >/dev/null; then
  echo "warning: no runtime is embedded in the application yet, so \`bdk build sign\` has nothing to re-sign; Xcode signs the application." >&2
  exit 0
fi
bdk_run_steps sign
