# BDK implementation plan

Planning output for BDK, the Browser Development Kit, dated 2026-11-14. It is based on the BDK Design Specification v0.7 and the planning brief. No production code has been written.

| Document | Read it for |
|---|---|
| [amendments.md](amendments.md) | Everything that changed on 2026-11-15 after the product owner's review. Read this instead of rereading the rest. |
| [feasibility.md](feasibility.md) | What was inspected and run, with source references and a status for each of the ten investigations |
| [architecture.md](architecture.md) | Components, the Chromium adapters, the C ABI, internal pages, the broker, extensions, providers, security |
| [repository-and-tooling.md](repository-and-tooling.md) | Nx project graph, root scripts, caching, Chromium storage and worktrees, quality gates, CI |
| [roadmap.md](roadmap.md) | Milestones, dependencies, acceptance criteria and release gates |
| [tasks.md](tasks.md) | 108 agent-sized tasks with inputs, outputs, tests and ordering |
| [risks.md](risks.md) | Eight product-owner decisions and their status after the 2026-11-16 review, ranked risks and fallbacks |
| [sdk/](sdk/README.md) | The proposed SDK surface: `bdk.toml` reference, Swift API, UI requests, service commands, extensions, JavaScript APIs, C ABI, CLI, and a production-browser checklist. Added 2026-11-15. Sixteen additions (S1 to S16) and five scope statements in it still need confirmation. |
| [experiments/](experiments/) | The fixtures behind the executed and unexecuted tests |

Start with the summary table in `feasibility.md`, then section 1 of `architecture.md`, then section 1 of `risks.md`.

Chromium evidence is pinned to tag `158.0.8127.37`, commit `b8c2be851f62f6a394ec57955d7f31335d7cb6d4`.
