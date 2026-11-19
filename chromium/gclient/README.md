# chromium/gclient

`gclient.template` is rendered by `chromium:sync` into `<checkout>/.gclient` when that file is absent
(`{{chromium_url}}` is the only placeholder; override the URL with `BDK_CHROMIUM_URL`). One `src`
solution, `managed: False`, `checkout_pgo_profiles: False`, `target_os = ["mac"]`.

`pnpm chromium:sync [--experiment <id>] [--force] [--no-patch] [--no-hooks]` runs, under the checkout
lock (`$BDK_HOME/chromium/locks/managed.lock`, or `exp-<id>.lock`):

1. clone depot_tools into `$BDK_HOME/depot_tools` if missing, and put it first on PATH;
2. write `.gclient` if absent;
3. `gclient sync --revision src@refs/tags/<pin.tag> --no-history --shallow -D --force` (hooks run);
4. fail unless `src` HEAD equals `pin.commit`;
5. apply the patch series (`chromium/patches`), so a sync followed by a build is consistent.

Refuses (unless `--force`) when `src` has uncommitted changes or commits that are not the series.
Deviation: `git-cache` is not used, because `--no-history` checkouts do not use it.
