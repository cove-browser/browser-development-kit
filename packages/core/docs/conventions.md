# BDK C ABI conventions

Specification: `docs/planning/sdk/c-abi.md`. This file records the rules the headers follow, the decisions taken where
c-abi.md was silent, and how the ABI hash works. Tasks T-ABI-01 (headers, version, hash) and T-ABI-02 (stub, suite).

## Layout

| Path | Contents |
|---|---|
| `include/bdk/*.h` | Public headers: plain C, `extern "C"` guards, `BDK_EXPORT`, no platform header, no platform type |
| `interface/` | Interface definitions (JSON, T-ABI-03): one `<namespace>.json` per namespace, `policy.json`, and the identifier registry `ids.json`. Every `*.json` here goes into the hash |
| `runtime/` | The runtime core, C++23, no exceptions, no platform code: handles, errors, calls, subscriptions, broker, UI requests and every ABI function's checks. What a page does is behind the `Backend` interface in `runtime/core_internal.h` |
| `stub/` | The stub's in-memory backend and its task queue. Test hooks in `stub/bdk_stub.h` are not ABI |
| `tests/` | Conformance suite in C11, written against `bdk/bdk.h` and `tests/harness.h` only |
| `cmake/abi-hash.cmake`, `generated/` | Hash generator and checker; the recorded hash |

## Rules (c-abi.md section 2, as implemented)

* Handles are 64-bit identifiers. The stub packs type tag, generation and index. Zero is never valid; a stale or
  mistyped handle returns `BDK_ERR_STALE_HANDLE` and touches nothing. Do not rely on the packing: only the runtime core
  does it.
* Every struct passed by pointer starts with `size_t struct_size`. The callee reads or writes only the bytes
  declared and treats missing fields as zero. A `struct_size` smaller than `sizeof(size_t)` is
  `BDK_ERR_INVALID_ARGUMENT`.
* Strings are UTF-8 with explicit length, borrowed. Strings out come in a `bdk_buffer_t` (release with
  `bdk_release`) or are borrowed until a callback returns.
* Everything is on the UI thread unless marked `BDK_ANY_THREAD`. Other threads get `BDK_ERR_WRONG_THREAD`
  (functions returning a handle return 0 and call nothing back). The thread that calls `bdk_runtime_start` is the UI
  thread until shutdown completes.
* No callback runs synchronously from the call that causes it (results, events, release callbacks, `resolve`).
* Asynchronous calls: the result callback runs exactly once, then the release callback. A cancelled call delivers
  `broker.cancelled`. A call that returns 0 was rejected synchronously and calls nothing back. A `bdk_call_t` goes
  stale when its result callback starts (cancel then returns `BDK_ERR_STALE_HANDLE`).
* Registration functions (`bdk_app_*`, `bdk_page_handler_register`, `bdk_provider_register`, `bdk_ui_set_handler`)
  work only between start and `bdk_runtime_freeze_registrations`; later they fail with
  `config.registrationAfterStart`. On success BDK owns the context and calls its release callback once, at shutdown
  (or on replacement). On failure the caller keeps the context and no release callback runs.
* Contents cannot be created before the freeze (`BDK_ERR_NOT_STARTED`).
* Errors: `bdk_status_t` plus a trailing `bdk_error_t* out_error`, set to 0 on entry and filled only with
  `BDK_ERR_FAILED`. Domains and codes are strings (`bdk_ids.h` has the ones used so far).
* Coordinates are window coordinates in device-independent pixels, origin top-left. Paths are UTF-8 strings.

## Decisions where c-abi.md was silent or sketched (provisional)

1. Signatures. Section 5 gives families, not signatures. The declared signatures follow the section 2 conventions and
   are marked PROVISIONAL in the headers. They are what the stub implements and the suite tests.
2. `bdk_release` returns `bdk_status_t` (c-abi.md leaves it unspecified) so double release is observable. Runtime,
   call and call-context handles are not released through it (`BDK_ERR_INVALID_ARGUMENT`).
3. `bdk_buffer_bytes(bdk_buffer_t)` was added: a buffer needs an accessor.
4. `bdk_profile_open` returns the profile handle through `bdk_profile_t* out_profile` (written synchronously, valid
   after the result callback succeeds) because the result callback carries only JSON. `bdk_profile_close` releases
   it. One open handle per profile (`profile.inUse`).
5. Ephemeral profiles: `bdk_profile_open_private`, id `ephemeral:N`, never listed, gone after close.
6. `bdk_contents_state_t` strings are borrowed until the next `bdk_contents_state` call on that contents. Its
   `audio` and `capture` are flag words (`BDK_AUDIO_*`); no capture flags are defined.
7. Callback types c-abi.md names without a signature: `bdk_runtime_event_cb`, `bdk_event_cb`, `bdk_key_reserve_cb`,
   `bdk_key_unhandled_cb`, `bdk_native_page_cb`. Provisional shapes are in the headers.
8. `bdk_subscribe_named` was added (by-name pair of `bdk_subscribe`): application events have names and no
   generated identifier, so they could not be observed otherwise.
9. `bdk_provider_register` and `bdk_provider_set_key` take a `bdk_runtime_t` (c-abi.md omits it). The vtable has no
   callbacks yet because their signatures are not specified.
10. Application command, event and page-route names must start with `app.` (`config.nameNotApp`). Duplicates fail
    with `config.duplicateName` (not in the swift-api error table).
11. Which completions grant is the `grants` column of the surface table in `runtime/ui_router.cc`, from
    ui-requests.md section 5: `grant` (permissions, and on every surface), `keep` (downloads), `save` (passwords,
    autofill), `accept` (autofill suggestions), `proceed` (interstitials), `select` (device choosers), `share` (screen
    capture), `open` (external protocols). A granting completion needs a gesture event of kind `BDK_NATIVE_NSEVENT`
    (`ui.gestureRequired`). Completing twice fails with `ui.requestCompleted`. Other completion names are not
    validated per surface until the per-surface interface definitions exist; the router adds only the rules in
    "The UI request router" below.
12. `bdk_native_handle_t` and `bdk_native_kind_t` live in `bdk_base.h` (c-abi.md puts them in section 5.4) because the
    start configuration needs them.
13. `include/bdk/bdk_ids.h` is generated (T-ABI-03) from `interface/*.json` and the registry `interface/ids.json`; do
    not edit it. Error domains and codes, runtime and contents event kinds, event flags, audio, security and visibility
    values and surface numbers are `constants` in `interface/abi.json`, with the values they had when the file was
    written by hand. Surface numbers follow the order of the ui-requests.md table. See "Interface definitions and
    identifiers".

14. A surface's mode comes from `surfaces` in the resolved configuration. A surface that is not listed is `default`
    (the resolver lists them all). `[ui] default_browser_surfaces` is already folded into the listed modes by the
    resolver, so the core never reads it.

### The UI request router

`runtime/ui_router.cc` (T-UIR-01) is the one place that decides what happens to a request. The backend raises it with
`raise_ui_request(rt, request)` and learns the outcome from `request->on_finish`, called once, on a later task.

| Question | Decision |
|---|---|
| Where are the modes validated? | `bdk_runtime_start` reads `surfaces` before the backend starts. A mode the surface does not allow is `ui.surfaceMode`, an unknown surface `ui.surfaceUnknown`, a value that is not a mode `config.resolvedInvalid`. packages/cli validates the same table from its schema first (`ui.surface_mode`); `units.test.ts` fails when the schema and the table in `ui_router.cc` disagree. The surface table (name, id, allowed modes, granting completions) is the core's source of truth for the runtime, the schema for the CLI. |
| `custom` and no handler | A `custom` surface with no handler fails `bdk_runtime_freeze_registrations` with `ui.handlerMissing` (naming every such surface); the runtime stays unfrozen so a handler can be added. configuration.md says "before start"; handlers are registered after `bdk_runtime_start` and before the freeze, so the freeze is where it can be checked. |
| `disabled` | The request is finished as `UiOutcome::kDisabled` (stub and harness: `BDK_STUB_UI_DISABLED`, `BDK_HARNESS_UI_DISABLED`, 6) on a later task, with no handler called and no queue slot taken. The backend denies or cancels and makes the web or extension API report unavailable; `ui_surface_available(rt, surface)` answers that question before a request is raised. A handler registered on a disabled surface is ignored and released at shutdown as usual. |
| `default`, no handler | `kDefaulted`: the backend shows its default presenter. A handler's `BDK_UI_USE_DEFAULT` and `bdk_ui_use_default` give the same outcome. |
| `reject` | `kRejected`: the backend denies or cancels. A handler answer that is not one of the three responses is also `kRejected`, the safe reading. |
| Interstitials (`certificate_errors`, `reputation_warnings`) | Only `go_back` and `proceed` complete them (`ui.completionNotAllowed`). `proceed` needs a gesture and a payload with `"isOverridable":true`; a request that does not say is not overridable (`ui.notOverridable`). `reject` and a dropped request finish as `kCompleted` with `go_back`, so the raiser never has to decide what an unsafe reject means. A withdrawal stays `kWithdrawn`. They cannot be disabled (`ui.surfaceMode`). |
| Subsets | A `grant` on `permissions` may name `kinds` in its arguments; each must be in the request's `kinds` array (`ui.widensRequest`). A request that names no kinds offers nothing to name. No `kinds` argument means everything asked. |
| Queueing | Requests of one surface for one page (`contents`, 0 is its own page) reach the handler one at a time; the next is delivered, on a later task, after the previous one finishes by any route (completed, default, reject, released, withdrawn). A request withdrawn while waiting is never delivered. The default presenter path finishes at once, so it never holds the queue; the engine queues its own presenters. |
| Notices | A request with no `on_finish` (a notice to a custom presenter, such as leaving fullscreen) is delivered at once and never queued. |
| Dropped | An uncompleted request released by the application finishes `kCancelled`, never granted. |
| Gesture kind | The core checks that the gesture is an `NSEVENT`. That it is recent and went to a window of this application is for the backend that has windows (Phase 2). |
| Extension surfaces | `[extensions.ui]` is not read by the core before M5. |

### Named in c-abi.md and not declared (nothing states their shape)

`bdk_contents_prune`, `_perform_command`, `_can_perform_command`, `_find_start/_next/_stop`, `_set_host_context`;
`bdk_window_set_anchor_provider`, `_set_request_handler`, `bdk_window_for_contents`, `_contents`, `_active_contents`;
`bdk_session_run`, `bdk_task_*`, `bdk_automation_endpoint_*`, `bdk_automation_subscribe_audit`;
`bdk_crash_reports_*`, `bdk_netlog_*`. Interface definition validation and generated identifiers are T-ABI-03.

## Interface definitions and identifiers

The format is c-abi.md section 7. The generator, validator and tests are in `packages/js/codegen`; nothing in this
chain needs Chromium.

| File in `interface/` | Role |
|---|---|
| `<namespace>.json` | One namespace: `types`, `commands`, `events` and, for the ABI's own identifier families, `constants`. The file name must equal `namespace` |
| `policy.json` | `neverForPages` and `neverForSessions`: capability names or `namespace.*` patterns. A command whose `callers` include `page` cannot use one on the first list; one that includes `session` cannot use one on the second |
| `ids.json` | The registry: every identifier ever assigned, retired ones included. Generated; keep it under version control |

`constants` is the section for identifiers that are not commands (c-abi.md is silent on it). Each entry is a group:
`{ "group", "kind": "string" | "enum" | "flags", "prefix": "BDK_X_", "comment", "values": { "NAME": value } }`, which
becomes `#define BDK_X_NAME <value>` (strings quoted, enums `Nu`, flags `0xNu` with a single bit each). Values are
stated in the definition. The group named `surfaces` is also the set of surface names a command's `raises` may use, in
lower case. Commands and events get `BDK_COMMAND_<NAME>` and `BDK_EVENT_<NAME>` (`history.deleteAll` becomes
`HISTORY_DELETE_ALL`) with an identifier the registry assigns: the next integer above every identifier ever assigned,
starting at 1. No command or event is defined yet: services.md is still a placeholder catalogue, and the broker is M3
(T-SVC-01).

Rules the registry enforces: a new name gets a new identifier; a removed name stays in `ids.json` marked `retired` and
its identifier is never given to another name; a retired name cannot be defined again; a constant whose value differs
from the registry is a renumbering and fails; so does a new constant that takes a value already in the registry.

To add an identifier: edit the definition, run `pnpm exec nx run js-codegen:codegen` (rewrites `ids.json` and
`bdk_ids.h`), run `cmake -DMODE=generate -P packages/core/cmake/abi-hash.cmake` (the definitions are hashed), and
commit all of it. `pnpm exec nx run js-codegen:codegen-check` fails when the committed outputs are stale; `js-codegen:test`
and the `abi-linux` job run the same check. To remove one, delete it from the definition and regenerate. Do not edit
`ids.json` by hand except to resolve a merge: two branches that add identifiers conflict there, and the fix is to keep
both sides and re-run the generator, which fails if two names ended up with one identifier.

## The ABI hash

`bdk_abi_hash()` returns a lower-case hex SHA-256 of the public interface.

Input: every `include/bdk/*.h` and `interface/*.json`, in byte-wise path order. For each file the text is normalised:
CRLF and CR become LF; in headers `/* ... */` comments are removed; every whitespace run becomes one space; leading
and trailing whitespace is dropped. The digest covers `"<path relative to packages/core>\n<normalised text>\n"` for
each file in order. Reformatting or editing a comment leaves the hash alone; changing a declaration, macro, type,
header name or interface definition changes it. Do not use `//` comments in headers (they are not stripped).

Commands (CMake 3.20 or newer, run from anywhere):

```sh
cmake -P packages/core/cmake/abi-hash.cmake                   # print the hash to stdout
cmake -DMODE=generate -P packages/core/cmake/abi-hash.cmake   # rewrite generated/bdk_abi_hash.h and abi-hash.txt
cmake -DMODE=check    -P packages/core/cmake/abi-hash.cmake   # non-zero exit if the recorded hash is stale
```

`generated/bdk_abi_hash.h` (`#define BDK_ABI_HASH "..."`) is committed and is what `bdk_abi_hash()` returns in the
stub; a real runtime includes the same header. `generated/abi-hash.txt` holds the bare hash for tooling that does
not want to run CMake. The `check` mode runs in every build (target `bdk_abi_hash_check`) and as ctest
`abi_hash_is_current`. After any header change run `generate` and commit the result (`cmake --build <dir> --target
abi_hash_update` does the same).

## Running the suite against another runtime

Implement `tests/harness.h` for the runtime (pump the UI thread, run a function on another thread, script a
navigation, raise and withdraw a UI request, simulate a key), then configure with
`-DBDK_RUNTIME_LIBRARY=<library> -DBDK_HARNESS_SOURCES=<harness.cc>`. `bdk_conformance --list` and `--run <name>`
select tests.

## Cross-platform checklist (c-abi.md section 11) for this skeleton

No AppKit, Win32 or toolkit type appears; native objects cross only as `bdk_native_handle_t` (kinds exist for NSView,
NSWindow, NSApplication, NSEvent, HWND, X11 and Wayland). The stub uses only the C++23 standard library and
pthreads, and builds on macOS and Linux.

## Running the suite against the engine

`nx run core:test-engine` (`scripts/test-engine.sh`) builds `BDKEngineConformance.app` with CMake
(`-DBDK_ENGINE_FRAMEWORK=<directory with Chromium Framework.framework>`, default the runtime at
`.bdk/runtime`), embeds the framework and runs the suite plus `tests/tests_engine.c` in it. The harness
(`tests/harness_engine.mm`) is an application, because the framework needs `NSApplication` and an app bundle;
it drives the engine through `chromium/bdk/engine/bdk_testing.h`. Chromium starts once per process, so the
harness keeps it between runtimes (`BDK_ENGINE_KEEP_CHROMIUM`), forgets persistent profiles at each start (the
suite expects a new data directory) and serves every http(s) load from the engine's interceptor. It needs a
logged-in Mac. Scenarios that need a key window (an input method's composition) are skipped, with a message,
when macOS does not let the harness come to the front. `BDK_TEST_TRACE=1` prints every contents event.

A harness implements four hooks besides pumping and acting as Chromium: `bdk_harness_prepare_start_config`,
`bdk_harness_attach_view` (focus needs a window in a real runtime), `bdk_harness_session_format` (tests that
craft the stub's text format run only for it) and `bdk_harness_newer_session_state`.
