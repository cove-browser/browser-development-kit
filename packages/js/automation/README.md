# @bdk/automation

The JavaScript API of BDK automation (task T-JS-04, milestone M7): what an automation script calls inside
the V8 automation host, and a client for external tools. The API is specified in
[javascript-api.md](../../../docs/planning/sdk/javascript-api.md) section 5; the wire protocol it speaks is
section 2 of the M7 design contract (frames: `hello`, `call`, `cancel`, `subscribe`, `result`, `error`,
`progress`, `event`, `start`, `abort`, `closed`, ...). The package is private and unpublished.

## Entry points

| Import | Contents |
|---|---|
| `@bdk/automation` | `session`, `task`, `contents` (returns `Page`), `scripts`, and the service namespaces `profiles`, `history`, `bookmarks`, `downloads`, `settings`, `permissions`, `cookies`, `privacy`, `extensions`, `search`, `passwords`, `autofill`. All bound to the host. |
| `@bdk/automation/commands` | `commands.execute(name, args, options)`. Scripts import it as `@bdk/commands`. |
| `@bdk/automation/events` | `events.on(name, listener, { contents, profile })` returning `{ unsubscribe() }`. Scripts import it as `@bdk/events`. |
| `@bdk/automation/client` | `connect({ socketPath, token })` for Node: speaks the same frames over the external Unix socket (4-byte big-endian length, then JSON) and returns the same API plus `session` and `close()`. |

Source layout: `wire.ts` (frame types, framing), `transport.ts` and `connection.ts` (calls, cancellation,
subscriptions), `host.ts` (the in-host transport over `globalThis.__bdkHost`, `session`, `task`),
`page.ts`, `scripts.ts`, `namespaces.ts`, `commands.ts`, `events.ts`, `api.ts` (assembles them over one
connection), `client.ts` (Node socket), and the generated `services.ts` and `types.ts` (see Types).

Outside the host, importing `@bdk/automation` does nothing and throws no error; using a host-bound API then
throws "this API needs the BDK automation host".

## Types

The service namespaces, `commands.execute` and `events.on` are typed from the interface definitions (task
T-JS-06). `pnpm exec nx run js-codegen:codegen` writes `src/types.ts` (every named type) and `src/services.ts`
(`CommandMap` and `EventMap` of everything a session may call or subscribe to, an interface per service
namespace, and `Services`) from `packages/core/interface`. Both are git-ignored, and the `build`, `test` and
`lint` targets depend on the generator, as `js-internal`'s do. They hold types only: the runtime is the one
proxy in `namespaces.ts`, so a call is `<namespace>.<method>` or, for a group, `autofill.addresses.list`.
`@bdk/automation/types` exports the named types.

* A service call takes its command's arguments, so `history.search({ query: "x" })` does not compile; it
  resolves with the result as BDK sends it (a paginated command returns one page, `{ items, nextCursor }`).
* `commands.execute(name, args)` takes any command a session may call (`CommandMap`) or an `app.` command,
  and `events.on(name, listener)` any event (`EventMap`) or `app.` event, with the payload typed by name.
* The application's `app.` commands and events come from its `bdk.commands.json`: `bdk build codegen` writes
  `@bdk/automation/app.d.ts`, which merges them into `AppCommands` and `AppEvents`. A script's `tsconfig.json`
  includes it. While nothing is declared, any `app.` name is accepted and its result and payload are `unknown`.
* Settings keys are plain strings here; `@bdk/internal` narrows them to the declared keys.

`src/typing.test.ts` holds the browser-side cases (`@ts-expect-error` lines that `nx run js-automation:lint`
compiles) and `src/application-typing.test.ts` compiles a script against the `app.d.ts` that
`generateApplication` writes for a `bdk.commands.json` fixture, with `tsc`.

## How scripts are bundled

The host provides nothing but `__bdkHost` (`send(frame)` and `onmessage`): no module loader, no Node. A script
is therefore bundled by Rolldown into ONE ES module with no `import` statements, with `@bdk/automation`,
`@bdk/commands` and `@bdk/events` aliased to this package's sources (`index`, `commands`, `events`) and inlined.
The host evaluates the module, delivers the `start` frame, calls the default export and sends `done` or
`failed` itself; this library never sends those. The library attaches to `__bdkHost` when it is imported, so
the `start` frame is not missed.

## Testing

```sh
pnpm exec nx run js-automation:test    # vitest
pnpm exec nx run js-automation:lint    # biome and tsc
pnpm exec nx run js-automation:build   # tsc to dist/
```

Tests use fakes only: a mock transport, a fake socket server on a temporary Unix socket, and a simulated host
(`src/bundle.test.ts` bundles `test/fixtures/summarise.ts` with the Rolldown API and runs it against a fake
`__bdkHost`). No Chromium is involved.
