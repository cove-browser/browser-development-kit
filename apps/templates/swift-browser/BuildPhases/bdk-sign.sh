#!/usr/bin/env bash
# Last run-script phase of the application target (sdk/cli.md section 8): `bdk build sign`. It reads the
# signing identity, team and entitlements from Xcode's build settings. BDK_SKIP_SIGNING=1 skips it.
set -euo pipefail
# bdk-steps.sh is next to this script; shellcheck runs from elsewhere and cannot follow it.
# shellcheck source=/dev/null
source "$(dirname "$0")/bdk-steps.sh"
bdk_run_steps sign
