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
