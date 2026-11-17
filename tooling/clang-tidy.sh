#!/bin/sh
# Runs clang-tidy over a project's sources when a compilation database exists.
# Usage: clang-tidy.sh <build-dir> <source-dir>
set -eu
build=$1
src=$2
if [ ! -f "$build/compile_commands.json" ]; then
  echo "clang-tidy: no compile_commands.json in $build; skipping"
  exit 0
fi
tidy=$(command -v clang-tidy || true)
if [ -z "$tidy" ] && command -v brew >/dev/null 2>&1; then
  llvm=$(brew --prefix llvm 2>/dev/null || true)
  [ -x "$llvm/bin/clang-tidy" ] && tidy=$llvm/bin/clang-tidy
fi
if [ -z "$tidy" ]; then
  echo "clang-tidy: not installed; skipping"
  exit 0
fi
# shellcheck disable=SC2046
"$tidy" -p "$build" --quiet $(find "$src" -name '*.cc' -not -path "*/build/*")
