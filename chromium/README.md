# chromium

Chromium pin (`pin.json`), the `//bdk` overlay, the patch series, GN presets and the .gclient template.

The pin is kept current by the roll bot (`.github/workflows/chromium-roll.yml`, `pnpm exec nx run chromium:roll`).
It opens or updates one `chromium-roll/<version>` pull request and checks the patch series without a build; a
person still builds, publishes the runtime and pushes the updated `pin.json` to the roll branch. See
`tooling/README.md`.
