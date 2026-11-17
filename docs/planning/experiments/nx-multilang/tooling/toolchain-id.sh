#!/bin/sh
# Prints the identity of the native toolchain. Used as an Nx `runtime` input so
# that a compiler/SDK change invalidates native caches even when no tracked file changed.
set -eu
printf 'clang: %s\n' "$(clang++ --version | head -1)"
printf 'swift: %s\n' "$(swift --version 2>&1 | head -1)"
printf 'sdk: %s\n' "$(xcrun --show-sdk-version 2>/dev/null || echo none)"
printf 'arch: %s-%s\n' "$(uname -s)" "$(uname -m)"
printf 'override: %s\n' "${BDK_TOOLCHAIN_ID_OVERRIDE:-}"
