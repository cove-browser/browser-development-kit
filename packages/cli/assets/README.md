# CLI assets

Files the `bdk` executable carries inside itself: JSON Schemas, project templates and similar read-only data. The
only reader is `src/assets.ts`.

- `cli:package` embeds every file in this directory, at any depth, as a named asset. The name is the path relative
  to this directory (for example `bdk.schema.json`). Adding a file needs no configuration change. This `README.md`
  is the one file that is not embedded.
- Unpackaged runs (development, tests) read the same files from here.
- Read assets as text through `Assets.readText(name)`. Do not import `node:sea` anywhere else.

Expected files: `bdk.schema.json`, the JSON Schema for `bdk.toml` (T-CFG-01).

The built `@bdk/internal` modules (`packages/js/internal/dist/*.js`) are also embedded, as `js/internal/<module>.js`, and
`Assets.list(prefix)` names them. They are not kept in this directory; `src/asset-sources.ts` says where they come from.

The built `@bdk/internal`, `@bdk/protocol` and `@bdk/vite` packages (`.js` and `.d.ts` files of
`packages/js/<package>/dist`) are embedded too, as `js/sdk/@bdk/<package>/<file>`, for `bdk build codegen`.
