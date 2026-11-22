#!/usr/bin/env bash
# Run-script phase of the application target, after "Copy Bundle Resources" (sdk/cli.md section 8):
# `bdk build pages automation embed`. `automation` and `embed` are not built yet, so today it runs `pages`.
# `bdk build pages` writes into CODESIGNING_FOLDER_PATH, which Xcode sets.
set -euo pipefail
# bdk-steps.sh is next to this script; shellcheck runs from elsewhere and cannot follow it.
# shellcheck source=/dev/null
source "$(dirname "$0")/bdk-steps.sh"
bdk_run_steps pages automation embed
