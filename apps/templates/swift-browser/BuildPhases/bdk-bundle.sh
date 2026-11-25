#!/usr/bin/env bash
# Run-script phase of the application target, after "Copy Bundle Resources" (sdk/cli.md section 8):
# `bdk build pages automation embed`. `embed` is not built yet, so today it runs `pages automation`.
# Both write into CODESIGNING_FOLDER_PATH, which Xcode sets. Then the resolved configuration, which
# `bdk build automation` completes with the scripts' digests, is copied into the bundle's Resources, where
# BDKConfiguration.load() reads it.
set -euo pipefail
# bdk-steps.sh is next to this script; shellcheck runs from elsewhere and cannot follow it.
# shellcheck source=/dev/null
source "$(dirname "$0")/bdk-steps.sh"
bdk_run_steps pages automation embed
cp "${SRCROOT}/bdk.resolved.json" "${CODESIGNING_FOLDER_PATH}/Contents/Resources/bdk.resolved.json"
