#!/usr/bin/env bash
# Runs the conformance suite and the engine scenarios against the real runtime: the packaged runtime resolved at
# .bdk/runtime (pnpm chromium:runtime), or the framework directory in BDK_ENGINE_FRAMEWORK. Needs a logged-in Mac
# (the suite runs as an application with windows). Exits 0 with a message when there is no runtime.
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
frameworks="${BDK_ENGINE_FRAMEWORK:-$root/.bdk/runtime/Chromium.app/Contents/Frameworks}"
if [[ ! -d "$frameworks/Chromium Framework.framework" ]]; then
  echo "core:test-engine: no runtime at $frameworks; run pnpm chromium:runtime (or set BDK_ENGINE_FRAMEWORK). Skipped."
  exit 0
fi
build="$root/packages/core/build-engine"
cmake -S "$root/packages/core" -B "$build" -G Ninja -DBDK_ENGINE_FRAMEWORK="$frameworks" >/dev/null
cmake --build "$build" --target bdk_engine_conformance
app="$build/BDKEngineConformance.app/Contents/MacOS/BDKEngineConformance"
if [[ $# -gt 0 ]]; then
  "$app" "$@" 2>"$build/engine.log"
  exit $?
fi
# One Chromium process cannot run the whole suite: its descriptors and ports grow with every runtime it
# starts, and past about two hundred of them a child process fails to launch. The browser-owned surface
# scenarios (tests_engine_ui.c, and the autofill prompts in tests_engine_prompts.c) therefore run in a
# second process.
status=0
"$app" --run '!engine_ui_*' 2>"$build/engine.log" || status=$?
"$app" --run 'engine_ui_*' 2>"$build/engine-ui.log" || status=$?
exit $status
