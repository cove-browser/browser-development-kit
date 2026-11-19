# AGENTS.md

Guidance for coding agents working in BDK, the Browser Development Kit. This file sets standards and says
where to look. It is not documentation: when a fact lives in a doc, follow the link instead of copying it here,
and if the two disagree, the doc wins and this file should be fixed.

## Read before you change anything

| You are touching | Read first |
|---|---|
| Anything, for the first time | [docs/planning/README.md](docs/planning/README.md), then [amendments.md](docs/planning/amendments.md) (it overrides the rest of `docs/planning`) |
| Design intent, components, security | [docs/planning/architecture.md](docs/planning/architecture.md) (sections 14 and 16 especially) |
| Repo layout, Nx graph, caching, CI, hooks | [docs/planning/repository-and-tooling.md](docs/planning/repository-and-tooling.md) |
| What to build next, and its acceptance criteria | [roadmap.md](docs/planning/roadmap.md), [tasks.md](docs/planning/tasks.md), [risks.md](docs/planning/risks.md) |
| Public SDK surface (Swift, C ABI, JS, CLI, `bdk.toml`) | [docs/planning/sdk/](docs/planning/sdk/README.md) |
| `packages/core` (C ABI, stub, conformance suite) | [packages/core/docs/conventions.md](packages/core/docs/conventions.md) |
| `tooling/` and the `chromium:*` commands | [tooling/README.md](tooling/README.md) |
| `chromium/` (pin, overlay, patches, GN) | [chromium/README.md](chromium/README.md) and the README in each subdirectory |
| The original product spec | [docs/BDK-Design-Spec-v0.7.pdf](docs/BDK-Design-Spec-v0.7.pdf) |

Every package and directory has a README. Read the nearest one before editing in it.

## Standards

### Follow the existing code, including what is not written down

Most conventions here are not documented. Before writing something new, find the closest existing example and
match it: file layout, naming, error handling, test style, how commands parse flags and take locks, how docs and
commit messages are phrased. Consistency with the surrounding code beats your preference. If you think a
convention is wrong, raise it rather than diverging silently. If you establish a new one, record it in the
relevant doc, not here.

### Reusability and code quality

- Reuse before adding. Look for a shared module (for example under `tooling/src/`) before writing a helper, and
  extract one when a second caller appears.
- Keep units small and single-purpose, with clear inputs and outputs, so they can be tested without Chromium.
- Every change ships with tests at the level the existing code uses. Do not weaken or skip tests or lint rules
  to get green; fix the cause. If a rule is truly wrong, change it in its config with a stated reason.
- Leave no dead code, commented-out code, or TODOs without a task reference.

### Minimise Chromium builds

A full Chromium build costs hours; an incremental one can still cost over an hour.

- Prefer work that does not touch Chromium: the C ABI stub, tooling, Swift and JS packages. Prove behaviour
  against the stub and the conformance suite first.
- Put engine code in the `//bdk` overlay (`chromium/bdk`). Add a patch to Chromium only when no overlay hook
  can work, keep it as small as possible, and give it a commit body explaining why (see
  [chromium/patches/README.md](chromium/patches/README.md)).
- Avoid edits to widely included Chromium headers and anything that changes GN args; both rebuild large parts
  of the tree.
- Use `chromium:patch-check` (no build) to validate patches, and an experiment checkout for anything risky.
- Do not start a Chromium build, sync or roll unless asked. Hooks and CI never build Chromium; keep it that way.
- Tests use fixtures and fakes, never a real checkout.

### Room for other platforms

macOS is the first target, not the only one.

- Keep platform code behind the boundaries described in
  [architecture.md section 14](docs/planning/architecture.md) and the cross-platform checklist in
  [packages/core/docs/conventions.md](packages/core/docs/conventions.md). No AppKit, Win32 or toolkit type in
  shared code or public headers.
- Isolate platform-specific tooling (for example `lockf` versus `flock`) behind one function, not scattered
  conditionals.
- Do not hard-code `mac`/`arm64`, paths or tools outside the files that already own those choices.

### Security and maintainability are not negotiable

- Do not trade away a security invariant ([architecture.md section 16](docs/planning/architecture.md)) for speed,
  convenience or a passing test.
- Never commit secrets, keys or credentials; the `gitleaks` hook is a backstop, not a licence.
- Do not bypass hooks (`--no-verify`) or checks. CI repeats them anyway.
- Prefer the simple, readable solution over the clever one. Pin dependency versions as the repo already does.

## Working rules

- Use pnpm and Nx targets (`pnpm exec nx run <project>:<target>`). Root scripts are in `package.json`.
- Keep Chromium storage under `BDK_HOME`, as resolved by `tooling/src/paths.ts`.
- Changing a public header in `packages/core` means regenerating and committing the ABI hash (see its
  conventions doc).
- Commits use Conventional Commits with a scope (`feat(core): ...`, `fix(chromium): ...`), enforced by
  commitlint. Match the tone of recent `git log`.
- When you change behaviour, update the doc that describes it in the same change. When a decision departs from
  `docs/planning`, record it there.
