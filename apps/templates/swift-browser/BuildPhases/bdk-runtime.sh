#!/usr/bin/env bash
# Run-script phase of the BDKRuntime target (sdk/cli.md section 8): `bdk build config runtime codegen`.
#
# A BDK release that publishes no runtime fails `bdk build runtime` with runtime.unpublished. The phase reports
# that as an Xcode warning and goes on: the application runs on the stub runtime, and the bundle phase leaves
# out `embed`. Any other failure of the step fails the build.
set -euo pipefail
# bdk-steps.sh is next to this script; shellcheck runs from elsewhere and cannot follow it.
# shellcheck source=/dev/null
source "$(dirname "$0")/bdk-steps.sh"
bdk_run_steps config
if ! output="$(bdk_run_steps runtime 2>&1)"; then
  if [[ "${output}" != *"[runtime.unpublished]"* ]]; then
    echo "${output}" >&2
    exit 1
  fi
  echo "warning: this BDK release publishes no runtime, so the application runs on the stub runtime." >&2
elif [[ -n "${output}" ]]; then
  echo "${output}"
fi
bdk_run_steps codegen
