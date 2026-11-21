#!/usr/bin/env bash
# Run-script phase of the BDKRuntime target (sdk/cli.md section 8): `bdk build config runtime codegen`.
# `runtime` is not built yet, so today it runs `config codegen`.
set -euo pipefail
# bdk-steps.sh is next to this script; shellcheck runs from elsewhere and cannot follow it.
# shellcheck source=/dev/null
source "$(dirname "$0")/bdk-steps.sh"
bdk_run_steps config runtime codegen
