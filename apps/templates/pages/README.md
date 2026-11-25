# template-pages

The internal-page templates `bdk init` writes into `Pages/` (task T-CLI-02, sdk/cli.md section 4.3). The CLI
embeds every file here except this README and `project.json`; the generator is
`packages/cli/src/core/init/generate.ts`.

| Path | Used for |
|---|---|
| `starters/<page>.js` | One loader per starter page (`new-tab`, `settings`, `history`, `bookmarks`, `downloads`): a `title` and `load()`, which returns `{ label, detail, href? }` items from `@bdk/internal/*`. Valid JavaScript and TypeScript, so it is copied as `.js` or `.ts`. The grants each needs are in `STARTERS` (`packages/cli/src/core/init/choices.ts`) and written to `bdk.toml`. |
| `plain/` | `--pages plain`: `index.html`, `main.js` and `style.css` per page, with the loader beside them as `starter.js`, its imports rewritten to `/@bdk/internal/<module>.js`, where the runtime serves the SDK. No build step. |
| `vite/index.html` | `--pages vite`: each page's `index.html`, with `{{title}}` and `{{entry}}` |
| `vite/frameworks/<framework>/<language>/` | `shared/` (copied to `Pages/shared/`: the component or renderer every page uses) and `page/` (each page's entry, with `{{starter}}`) |
| `vite/styling/<styling>/style.css` | `Pages/shared/style.css`: plain CSS, or Tailwind |
| `versions.json` | The exact versions each combination pins in `Pages/package.json` |

`Pages/vite.config.*` and `Pages/tsconfig.json` are written by the generator. Each page is its own origin and its
own Vite build; pages share code only through `Pages/shared/` at build time.

## Frameworks

`vanilla`, `react` and `preact`. They build the DOM with `createElement` and text, never by assigning HTML strings,
which the internal-page policy forbids: `require-trusted-types-for 'script'` with `trusted-types` allowing no policy
(`chromium/bdk/engine/pages/bundled_page_source.cc`). Vue, Svelte, Solid and Lit create templates through
`innerHTML` and are not offered until BDK allows a named policy (sdk/README.md section 7, "Trusted Types").

## Checks

`packages/cli/src/core/init/init.test.ts` generates every combination and checks each page's HTML against the
policy, the pinned versions and the generated `bdk.toml`. On 2026-11-25 three combinations were installed from the
registry and built with the packaged `bdk build pages` by hand (React with TypeScript and Tailwind, Preact with
TypeScript, vanilla JavaScript), and `tsc --noEmit` passed for the TypeScript ones. That a framework's output runs
under Trusted Types in Chromium has not been checked: it needs a Chromium runtime.

`pnpm exec nx run template-pages:lint` runs Biome over the templates.
