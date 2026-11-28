# docs-site

The BDK documentation site: Fumadocs on Next.js, MDX content, static export (task T-REP-06, amendment 2.4).

| Path | Holds |
|---|---|
| `content/docs/*.mdx`, `meta.json` | The pages and their order: getting started, `bdk.toml`, CLI, Swift API, JavaScript APIs, versioning, security patches, compatibility gaps, experimental APIs. Each summarises its source in `docs/planning/sdk` (or `repository-and-tooling.md`, `risks.md`) and links to it; when the two disagree, the plan is right. Link between pages with relative `.mdx` paths. |
| `app/` | The Next.js app router: the home page, `docs/[[...slug]]`, and `api/search`, the search index written at build time |
| `lib/`, `components/` | The Fumadocs source (`fumadocs-mdx/macro`), the layout options, the MDX components and the static search dialog |
| `scripts/check-links.ts` | Checks every internal link and fragment of the export |

| Command | What it does |
|---|---|
| `pnpm docs:build` (`nx run docs-site:build`) | `next build` into `out/`, then the link check. `BDK_DOCS_BASE_PATH=/<path>` builds for a host that serves the site under a path, such as GitHub Pages. No network is used: no web fonts, no remote images. |
| `pnpm exec nx run docs-site:dev` | `next dev` |
| `pnpm exec nx run docs-site:lint` | Biome, and `tsc` for the scripts; `next build` type-checks the app |
| `pnpm exec nx run docs-site:test` | The link checker's tests |

Dependencies are pinned exactly in `package.json`, in the workspace (`pnpm-workspace.yaml`). External links are not
fetched by the check. Publishing (`docs.yml` in the tooling document) is not set up.
