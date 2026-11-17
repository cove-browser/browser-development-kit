# chromium/patches

`series` lists patch files, one per line, in application order (`#` comments allowed). Patches are
`git format-patch` output. Every patch needs a commit message body saying why the hook is needed and
which BDK component uses it; an empty body is refused by every command.

- `pnpm chromium:patch-apply [--experiment <id>] [--force]`: under the lock, resets `src` to
  `pin.commit` and runs `git am --3way` per patch. Does nothing if `src` is already pin plus exactly
  the series (so mtimes and incremental builds are kept). Refuses to reset when `src` has uncommitted
  changes or commits not in the series, unless `--force`. On a conflict it aborts `git am`, exits
  non-zero, and names the patch and every conflicting file.
- `pnpm chromium:patch-export [--experiment <id>]`: regenerates the patch files and `series` from the
  commits between `pin.commit` and HEAD in `src`. Deterministic (`--zero-commit --no-signature
  --full-index`, no user git config, file names from the subject with no numbers): exporting twice
  changes nothing, and stale `.patch` files are removed.
- `pnpm chromium:patch-check [--remote <url>] [--tag <tag>] [--commit <sha>] [--work-dir <dir>] [--json]`:
  no build. Blobless, depth-1, sparse fetch of the tag (default `chromium/pin.json`, from
  `https://github.com/chromium/chromium.git`, or `BDK_PATCH_CHECK_REMOTE`) limited to the files the
  series touches, then `git am --3way` per patch, reporting each patch. Every patch is tried even
  after a failure. Exit 0 on an empty series (no network). Library: `checkPatchSeries` in
  `tooling/src/chromium/patch-check.ts`, for the roll bot (T-CHR-11).

Workflow: edit in `src`, commit with a proper message, run `chromium:patch-export`, commit the patches.
