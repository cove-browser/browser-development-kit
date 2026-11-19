#!/bin/sh
# Prints the identity of the native toolchain. Used as an Nx `runtime` input so
# that a compiler or SDK change invalidates native caches even when no tracked
# file changed. Must stay fast and deterministic: no timestamps, no paths.
set -eu

first_line() {
  "$@" 2>&1 | head -n 1 || true
}

printf 'clang: %s\n' "$(first_line clang++ --version)"
printf 'swift: %s\n' "$(swift --version 2>&1 | grep -m 1 -i 'swift version' || echo none)"
printf 'sdk: %s\n' "$(xcrun --show-sdk-version 2>/dev/null || echo none)"
printf 'arch: %s-%s\n' "$(uname -s)" "$(uname -m)"
printf 'override: %s\n' "${BDK_TOOLCHAIN_ID_OVERRIDE:-}"
