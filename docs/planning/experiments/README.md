# Feasibility fixtures

Disposable fixtures behind [../feasibility.md](../feasibility.md). They are not part of BDK and are excluded from the root Nx workspace by `.nxignore`. Delete them once the corresponding milestone tasks replace them.

| Directory | Investigation | Executed on 2026-11-14 | Replaced by |
|---|---|---|---|
| `nx-multilang/` | 8: Nx with C++, Swift and TypeScript | Yes. All cases passed. | T-REP-01 |
| `worktree-cache/` | 9: shared runtime, isolation, locking, stale-cache detection | Yes. All cases passed, with dummy runtimes and a simulated build. | T-CHR-09 |
| `hybrid-broker-proxy/` | 4, 5 and part of 2: runtime scheme, capability broker, reparenting, on Electron as a stand-in | **No.** Written only. | T-WUI-01, T-AUT-02 |
| `mv3-probe/` | 3: Manifest V3 behaviour under a host | **No.** Written only; not loaded into any browser. | T-EXT-01, T-EXT-07 |

## Running the two executed fixtures

`nx-multilang`:

```sh
pnpm install
pnpm build && pnpm test
pnpm exec nx run-many -t build test --output-style=static   # run again to see cache hits
BDK_TOOLCHAIN_ID_OVERRIDE=other pnpm exec nx run-many -t build   # native targets miss, TypeScript hits
```

`worktree-cache` needs a Git repository and a store directory:

```sh
git init && git add -A && git commit -m fixture
pnpm install
git worktree add ../wt-a -b a && git worktree add ../wt-b -b b
export BDK_HOME="$PWD/../bdk-home"
mkdir -p "$BDK_HOME/chromium/checkouts/managed" "$BDK_HOME/runtimes"
pnpm exec nx run chromium:source-build        # publishes a dummy runtime under an exclusive lock
(cd ../wt-a && pnpm install && pnpm exec nx run app:build)
(cd ../wt-b && pnpm install && pnpm exec nx run app:build)
```

Both fixtures use macOS tools: `lockf`, APFS, Xcode's clang and Swift.

## The two fixtures that were not run

- `hybrid-broker-proxy` downloads Electron and starts it with hidden windows. `pnpm install`, then `node node_modules/electron/install.js`, then `./node_modules/.bin/electron .`. It writes `result-<scheme>.json`. Change `internal_scheme` in `bdk.config.json` and run again to check that the scheme is read at start-up. A pass shows content-layer behaviour only; Electron does not use `//chrome`, WebUI or Mojo.
- `mv3-probe` is loaded as an unpacked extension. Its service worker exposes `bdkProbe(name, args)` for a harness, and its popup, side panel and options page print what each believes the current tab and window are. Expect to fix small errors on first load.
