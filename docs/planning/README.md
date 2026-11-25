# BDK implementation plan

Planning output for BDK, the Browser Development Kit, dated 2026-11-14. It is based on the BDK Design Specification v0.7 and the planning brief. It was written before any production code; progress since is in the status blocks of [roadmap.md](roadmap.md), and the decisions taken while building that depart from it are in [amendments.md](amendments.md).

| Document | Read it for |
|---|---|
| [amendments.md](amendments.md) | Everything that changed on 2026-11-15 after the product owner's review. Read this instead of rereading the rest. |
| [feasibility.md](feasibility.md) | What was inspected and run, with source references and a status for each of the ten investigations |
| [architecture.md](architecture.md) | Components, the Chromium adapters, the C ABI, internal pages, the broker, extensions, providers, security |
| [repository-and-tooling.md](repository-and-tooling.md) | Nx project graph, root scripts, caching, Chromium storage and worktrees, quality gates, CI |
| [roadmap.md](roadmap.md) | Milestones, dependencies, acceptance criteria and release gates |
| [tasks.md](tasks.md) | 148 agent-sized tasks with inputs, outputs, tests and ordering |
| [risks.md](risks.md) | Eight product-owner decisions and their status after the 2026-11-16 review, ranked risks and fallbacks |
| [sdk/](sdk/README.md) | The proposed SDK surface: `bdk.toml` reference, Swift API, UI requests, service commands, extensions, JavaScript APIs, C ABI, CLI, and a production-browser checklist. Added 2026-11-15. Sixteen additions (S1 to S16) and five scope statements in it still need confirmation. |
| [experiments/](experiments/) | The fixtures behind the executed and unexecuted tests |

## Recording gaps

A milestone is not marked done until everything it leaves undone is written down where the next phase will pick it up. That covers work in the milestone's tasks that was not built, API in [sdk/](sdk/README.md) whose milestone has passed, tests that could not be run, acceptance checked only by automation, and anything that is defined or generated but not implemented (a Swift wrapper the engine rejects, an event nothing emits).

For each gap, in the same change as the milestone's status block:

1. Add a task to [tasks.md](tasks.md), or widen an open one, with tests that would close it. A gap goes to the next milestone unless it depends on work scheduled later; then it goes to that milestone and the task says why.
2. List the task in that milestone's table in [roadmap.md](roadmap.md), and point the status block's "not done" at the task ID and milestone.
3. Mark the API in [sdk/](sdk/README.md) as not built, with the task ID, so the reference never reads as complete when it is not.

"Not done" with no task ID is not a record. Before closing a milestone, compare the interface definitions and the SDK documents with what the engine implements, not only with the task list.

Start with the summary table in `feasibility.md`, then section 1 of `architecture.md`, then section 1 of `risks.md`.

Chromium evidence is pinned to tag `158.0.8127.37`, commit `b8c2be851f62f6a394ec57955d7f31335d7cb6d4`.
