# @bdk/internal

The SDK an internal page imports: `import { settings } from "@bdk/internal/settings"`. Design:
[docs/planning/sdk/javascript-api.md](../../../docs/planning/sdk/javascript-api.md) sections 1 to 4, 7 and 9. Task:
T-JS-03. Private: nothing is published to npm.

## Served unbundled

A bundled page loads these files as they are from `/@bdk/internal/<module>.js` (`bdk build pages` copies `dist/*.js` to
`Contents/Resources/BDK/js/internal/`). So the build is flat, one `.js` per module, and a module imports only its
siblings with a relative specifier (`./call.js`). Nothing bare at run time: `import type` from `@bdk/protocol` is
fine, because it is erased, and the few runtime pieces of that package (`BdkError`, `SdkErrors`) are repeated here, shape
for shape. `imports.test.ts` fails on a bare runtime import, in the sources and in `dist`; `protocol-parity.test.ts`
fails when the copies drift.

## Modules

| Module | Is | Written |
|---|---|---|
| `call` | The transport seam (`setTransport`, `getTransport`), `invoke`, `paginate`, `subscribe`; an aborted call rejects with `broker.cancelled` | by hand |
| `mojo-transport` | The default `Transport`, over the page broker's Mojo bindings (`/@bdk/page_broker.mojom-webui.js`, loaded on first use, injectable). Bigint call and subscription ids, `cancel` on abort, JSON in and out | by hand |
| `page` | `page`: `route`, `capabilities`, `can`, `url`, `info`, made from `runtime.caller` with top-level await. `create-page` has the testable `createPage` | by hand |
| `commands`, `events` | `commands.execute`, `events.on` for `app.` names, typed by `AppCommands` and `AppEvents`, which the application's declarations fill by declaration merging | by hand |
| `app-settings` | `AppSettings` (same mechanism) and `SettingKey` | by hand |
| `errors`, `capabilities` | `BdkError`; `holdsCapability`, the broker's grant rule | by hand |
| `testing` | A mock transport: scripted answers, recorded calls, grants checked with the broker's rule, `emit`, `grantsFromResolved` | by hand |
| `types`, `metadata` | Every named type of the interface definitions; the capability of each command and event | generated |
| `settings`, `profiles`, `history`, `bookmarks`, `downloads`, `permissions`, `privacy` (with `cookies`), `passwords`, `autofill`, `search`, `system` | One typed function per command a page may call, `on<Event>` per event | generated |

Generated files are written by `pnpm exec nx run js-codegen:codegen` from `packages/core/interface` and are not
committed (they are in the root `.gitignore`); every target of this package runs `codegen` first. Run it by hand
after a fresh clone for editor types. A new namespace's module goes in `.gitignore` and in the `codegen` target's
`outputs`; a `js-codegen` test checks both. Not yet generated, because the definitions do not exist: `extensions`,
`translation`, `favicons`, `top-sites`. A command whose `callers` exclude `page` has no function.

The application's own commands, events and settings come from `@bdk/internal/app.d.ts`, which `bdk build codegen`
writes with `generateApplication` of `@bdk/codegen`.

## Tests

`pnpm exec nx run js-internal:test`: the Mojo transport against a fake bindings object, the seam, pagination, the page,
the mock transport and its grants, a generated module per shape, and the no-bare-import and package `exports` checks.
`pnpm exec nx run js-internal:lint` type-checks them, with the declaration-merging tests.
