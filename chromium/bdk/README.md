# chromium/bdk

The `//bdk` overlay: BDK's engine code, built by GN inside the Chromium checkout. Chromium style
(`.clang-format`, `BasedOnStyle: Chromium`).

`chromium:build` copies this directory to `<checkout>/src/bdk` under the checkout lock, writing only
files whose content changed (so only they rebuild), deleting files removed here, and excluding `bdk/`
from `git status` in `src`. A copy rather than a symlink: the managed checkout is shared by every
worktree, and a link out of the source root would follow whichever worktree last changed it (and
dangle when that worktree is removed). The overlay therefore reflects the worktree that is building.

`bdk_overlay_marker()` is the trivial first target. The first patch in `chromium/patches` adds `//bdk`
to the `public_deps` of `//chrome:chrome_framework` and exports `_bdk_overlay_marker` in
`chrome/app/framework.exports` (the framework hides every unlisted symbol). `chromium:build` runs
`nm -gU` on the built framework and fails if the symbol is absent.
