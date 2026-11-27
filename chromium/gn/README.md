# chromium/gn

`release.gn` and `dev.gn` are GN args presets. `chromium:build --config dev|release` (default
`release`) copies the preset to `src/out/<config>/args.gn` (only if it differs) and runs `gn gen`.
Each configuration has its own output directory.

Both: `target_os = "mac"`, `target_cpu = "arm64"`, `is_component_build = false`,
`chrome_pgo_phase = 0`, `use_remoteexec = false`,
`cc_wrapper = "env CCACHE_SLOPPINESS=time_macros ccache"`.
`release`: `is_debug = false`, `symbol_level = 0`, `blink_symbol_level = 0`.
`dev`: `is_debug = false`, `dcheck_always_on = true`, `symbol_level = 1`, `blink_symbol_level = 0`.

Verified at b8c2be851f62: every name is declared in the sources (`target_os`/`target_cpu` in
`build/config/BUILDCONFIG.gn`, `is_debug`, `is_component_build` there, `symbol_level` in
`build/config/compiler/compiler.gni`, `blink_symbol_level` in `third_party/blink/renderer/config.gni`,
`chrome_pgo_phase` in `build/config/compiler/pgo/pgo.gni`, `dcheck_always_on` in
`build/config/dcheck_always_on.gni`, `use_remoteexec` in `build/toolchain/rbe.gni`, `cc_wrapper` in
`build/toolchain/cc_wrapper.gni`), and `gn gen out/dev` and `out/release` succeed in the real checkout.

Both also set `support_external_google_api_key = true` (declared in `google_apis/BUILD.gn`), added in M6
(T-PRV-09) so that one runtime serves applications with and without their own Google key. Only
`google_apis/buildflags.h` depends on it, read by the `google_apis` target and `fuchsia_web` (not built
here). Built on Chromium 158 (the release build's `buildflags.h` has it on), and the engine scenario
`engine_launch_chromium_starts_with_the_application_s_google_key` starts with a key (2026-11-27).

Both also set `generate_about_credits = true` (declared in `components/resources/BUILD.gn`), added in M9
(T-REL-02): the build then runs `tools/licenses/licenses.py credits` to write
`gen/components/resources/about_credits.html`, which is `chrome://credits` and, through
`chromium/runtime-files.json`'s `notices`, the runtime's `THIRD_PARTY_NOTICES.html`. Its default
(`is_official_build`) copies a placeholder, which packaging refuses. Only that action, its buildflag header
(read by `about_ui_credits_unittests.cc`, not built here) and `components_resources.pak` depend on it, so the
rebuild is small. Built on Chromium 158: the release build writes the full credits page (2026-11-25).
