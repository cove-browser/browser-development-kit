#!/bin/sh
set -eu
if [ -n "${BDK_TOOLCHAIN_ID_OVERRIDE:-}" ]; then echo "$BDK_TOOLCHAIN_ID_OVERRIDE"; exit 0; fi
printf 'xcode:%s clang:%s sdk:%s\n' "$(xcodebuild -version 2>/dev/null | tr '\n' ' ')" "$(clang++ --version | head -1)" "$(xcrun --show-sdk-version)"
