# @bdk/protocol

The types that commands, events and transports share, for `@bdk/internal` (internal pages) and, in M7,
`@bdk/automation` (the automation host and its clients). Design:
[docs/planning/sdk/javascript-api.md](../../../docs/planning/sdk/javascript-api.md) sections 1, 3.1 and 5.5. Task:
T-JS-01. Private: nothing is published to npm.

It is small and generic on purpose. It has no command names and no generated code; those belong to the packages that
use it. Add to it, do not change what is here.

## Exports

| Export | Is |
|---|---|
| `BdkError`, `BdkErrorInfo`, `isBdkError` | The error of every call: `domain`, `code`, `message`, `details`. `isBdkError` checks the shape, because `@bdk/internal` keeps its own copy of the class |
| `SdkErrors` | Errors the SDK raises itself: `broker.cancelled`, `broker.transportUnavailable`, `broker.invalidResponse`, plus the broker's `capability.denied`, `broker.unknownCommand`, `broker.notImplemented` and `broker.invalidArguments` as the SDK raises them locally |
| `CallOptions`, `Progress`, `Subscription`, `EventFilter` | What every call takes and every subscription returns |
| `Paginated<T>`, `ItemOf<P>` | The result shape of a paginated command, and its item type |
| `Transport` | `invoke(command, args, options)` and `subscribe(event, filter, listener, onError)`; a page, a session and a test each have one |
| `CallerInfo`, `CallerKind`, `Page` | The answer of `runtime.caller`, with `internalScheme` for a page |

## Tests

`pnpm exec nx run js-protocol:test`. Runtime tests cover the error class; `expectTypeOf` tests pin the shapes.
`pnpm exec nx run js-protocol:lint` type-checks both.
