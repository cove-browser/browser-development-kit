# BDK C ABI

Status: proposal, 2026-11-15. The rules are from [../architecture.md](../architecture.md) section 5. This document turns them into a header layout, concrete types and function families. Every identifier is a placeholder and no header has been compiled. Status labels are in [README.md](README.md) section 2. Tasks: T-ABI-01 to T-ABI-05, T-XPL-01.

Application developers do not call the ABI. It is the one boundary that the Swift SDK, and later the .NET and Linux bindings, depend on. It is also what the stub runtime implements, which is what lets hosted CI test everything above it without Chromium.

## 1. Headers

All in `packages/core/include/bdk/`. They compile as C11 and as C++, with warnings as errors, on macOS and Linux in every pull request.

| Header | Contents |
|---|---|
| `bdk.h` | Includes the rest |
| `bdk_base.h` | Export macro, handles, strings, buffers, errors, values, callbacks, the version and hash |
| `bdk_runtime.h` | Start-up configuration, start, shutdown, runtime events, thread posting |
| `bdk_profile.h` | Profile lifecycle |
| `bdk_contents.h` | WebContents lifecycle, navigation, state, session state |
| `bdk_view.h` | Native view handles, focus, keyboard |
| `bdk_window.h` | Window registration, anchors, window requests |
| `bdk_ui.h` | UI request delivery and completion |
| `bdk_broker.h` | Command invocation, events, application command registration |
| `bdk_provider.h` | Provider registration and the callbacks a provider implements |
| `bdk_automation.h` | Sessions, tasks, the external endpoint |
| `bdk_diagnostics.h` | Logging, crash reports, network log |
| `bdk_ids.h` | Generated: command, event, capability, surface and error identifiers |

Nothing in these headers includes a platform header or names an AppKit, Win32 or GTK type.

## 2. Base types

```c
#define BDK_EXPORT __attribute__((visibility("default")))   /* __declspec(dllexport) on Windows */

/* A handle is an identifier, not a pointer: type tag, generation, index. Zero is never valid. */
typedef uint64_t bdk_handle_t;
typedef bdk_handle_t bdk_runtime_t, bdk_profile_t, bdk_contents_t, bdk_window_t, bdk_view_t,
                     bdk_request_t, bdk_call_t, bdk_subscription_t, bdk_session_t, bdk_task_t,
                     bdk_error_t, bdk_buffer_t, bdk_context_t;

/* UTF-8, explicit length, not necessarily terminated. Borrowed for the duration of the call or callback. */
typedef struct bdk_string { const char* data; size_t length; } bdk_string_t;

/* Bytes. Borrowed for the duration of the call or callback. */
typedef struct bdk_bytes { const uint8_t* data; size_t length; } bdk_bytes_t;

typedef enum bdk_status {
  BDK_OK = 0,
  BDK_ERR_INVALID_ARGUMENT = 1,
  BDK_ERR_STALE_HANDLE = 2,
  BDK_ERR_WRONG_THREAD = 3,
  BDK_ERR_NOT_STARTED = 4,
  BDK_ERR_FAILED = 5          /* details are in the error out-parameter */
} bdk_status_t;
```

| Rule | Detail |
|---|---|
| Handles | Generation-checked. A call with a stale or mistyped handle returns `BDK_ERR_STALE_HANDLE` and touches nothing. |
| Structs | Every struct passed by pointer begins with `size_t struct_size`. The callee reads only the bytes the caller declared, so a struct grows by appending fields. Small value structs passed by value, such as `bdk_string_t`, are frozen. |
| Strings in | Borrowed. The callee copies what it keeps. |
| Strings out | Returned as a `bdk_buffer_t` the caller releases, or delivered to a callback as a borrowed `bdk_string_t` valid until the callback returns. |
| Ownership | Every function that returns a handle documents the one function that releases it. `bdk_release(bdk_handle_t)` releases any owned handle; typed aliases exist for readability. |
| Booleans | `uint8_t`, 0 or 1 |
| Enums | Fixed-width integers. Unknown values received by a newer host from an older runtime, or the reverse, must be tolerated: every `switch` has a default. |
| Threads | Every function is called on the UI thread, and every callback arrives on it, unless the header marks it `BDK_ANY_THREAD`. A call from another thread returns `BDK_ERR_WRONG_THREAD`. |
| Exceptions | None cross the boundary in either direction |
| Reentrancy | A callback may call back into the ABI. BDK never calls a callback synchronously from inside the function that registered it. |

### 2.1 Errors

```c
BDK_EXPORT bdk_string_t bdk_error_domain(bdk_error_t);    /* "capability" */
BDK_EXPORT bdk_string_t bdk_error_code(bdk_error_t);      /* "denied" */
BDK_EXPORT bdk_string_t bdk_error_message(bdk_error_t);
BDK_EXPORT bdk_string_t bdk_error_details_json(bdk_error_t);
BDK_EXPORT void         bdk_error_release(bdk_error_t);
```

A function that can fail returns `bdk_status_t` and takes a trailing `bdk_error_t* out_error`. On `BDK_ERR_FAILED` the caller owns the error. Asynchronous failures deliver the error to the callback, which borrows it.

Domains and codes are the ones in [swift-api.md](swift-api.md) section 16. They are strings so that a binding built against an older header can still report a newer error.

### 2.2 Asynchronous calls

```c
typedef void (*bdk_result_cb)(void* context, bdk_error_t error /* 0 on success */, bdk_string_t result_json,
                              bdk_buffer_t binary /* 0 if none; borrowed */);
typedef void (*bdk_progress_cb)(void* context, double fraction, bdk_string_t message);
typedef void (*bdk_release_cb)(void* context);   /* called exactly once, when BDK will not use context again */

BDK_EXPORT bdk_status_t bdk_call_cancel(bdk_call_t);
```

Every asynchronous function takes a callback, a context pointer and a release callback, and returns a `bdk_call_t`. The result callback is called exactly once, including after a cancel, in which case the error is `broker.cancelled`. The release callback follows it. This maps onto Swift `async` with task cancellation and onto a C# `Task` with a `CancellationToken`.

### 2.3 Values

Command arguments, results and event payloads cross as UTF-8 JSON, with an optional binary buffer beside the JSON for results that are bytes. This is the recommendation in [README.md](README.md) section 7.

| For | Against |
|---|---|
| One encoding for Swift, C#, the Mojo page bridge and the automation channel | A parse on each side of every call |
| The stub runtime and the conformance suite are trivial to write | Not suited to high-frequency data |
| An older binding can pass through fields it does not know | |

High-frequency paths do not use it: page state changes and navigation events are typed structs (section 5.3), and input never crosses the ABI at all.

## 3. Version and hash

```c
#define BDK_ABI_VERSION 1
BDK_EXPORT uint32_t     bdk_abi_version(void);     /* BDK_ANY_THREAD; callable before start */
BDK_EXPORT bdk_string_t bdk_abi_hash(void);        /* BDK_ANY_THREAD; hex SHA-256 */
BDK_EXPORT bdk_string_t bdk_runtime_manifest_json(void);   /* BDK version, Chromium version, digest */
```

| Value | Computed from | Checked |
|---|---|---|
| Version | An integer raised on any incompatible change | By the binding at start. A mismatch fails before Chromium initialises. |
| Hash | The normalised text of every public header plus every interface definition | For an exact match at start. In CI, the headers' hash is compared with the hash recorded for the published runtime (tooling document 10.1). |

A source-built and a prebuilt runtime for the same inputs must report the same hash.

## 4. Runtime

```c
typedef struct bdk_start_config {
  size_t        struct_size;
  bdk_string_t  resolved_config_json;     /* the contents of bdk.resolved.json */
  bdk_string_t  bundle_path;              /* the application bundle; pages and scripts are read from here */
  bdk_string_t  framework_path;           /* where the runtime was loaded from */
  bdk_string_t  user_data_path_override;  /* tests only */
  bdk_string_t  google_api_key;           /* empty unless a service uses provider = "google" */
  uint32_t      host_abi_version;
  bdk_string_t  host_abi_hash;
  bdk_native_handle_t native_app;         /* tagged platform handle (section 5.4): the NSApplication */
} bdk_start_config_t;

BDK_EXPORT bdk_status_t bdk_runtime_start(const bdk_start_config_t*, bdk_runtime_t* out, bdk_error_t* out_error);
BDK_EXPORT bdk_call_t   bdk_runtime_shutdown(bdk_runtime_t, bdk_result_cb, void* ctx, bdk_release_cb);
BDK_EXPORT bdk_status_t bdk_runtime_freeze_registrations(bdk_runtime_t, bdk_error_t* out_error);
BDK_EXPORT void         bdk_post_to_ui_thread(void (*fn)(void*), void* ctx);   /* BDK_ANY_THREAD */
```

Start is two steps underneath one Swift call:

1. `bdk_runtime_start` checks the ABI, validates the resolved configuration, registers the scheme and initialises Chromium far enough to accept registrations.
2. The binding registers page handlers, application commands, providers and UI handlers.
3. `bdk_runtime_freeze_registrations` freezes the route and capability tables and lets the first renderer start.

A registration call after the freeze returns `config.registrationAfterStart`.

## 5. Host functions

The functions behind the Swift objects in [swift-api.md](swift-api.md) sections 5 to 9. One family per object; the table gives the families, not every function.

### 5.1 Profiles

| Function family | Purpose |
|---|---|
| `bdk_profiles_list`, `bdk_profile_create`, `bdk_profile_open`, `bdk_profile_open_private`, `bdk_profile_close`, `bdk_profile_rename`, `bdk_profile_delete` | Lifecycle. Create, open and delete are asynchronous. |
| `bdk_profile_info` | Identifier, name, kind, parent |

`bdk_profile_create` takes a display name and returns the new profile's identifier; it is the host function behind the `profiles.create` command. `bdk_profile_open` takes an identifier and never creates. Swift's `open(_ name:, createIfMissing:)` is a convenience built from list, create and open in the binding.

### 5.2 WebContents

| Function family | Purpose |
|---|---|
| `bdk_contents_create`, `bdk_contents_close`, `bdk_contents_destroy` | Lifecycle. Create takes a size-prefixed options struct. |
| `bdk_contents_load`, `_reload`, `_stop`, `_go_to_offset`, `_go_to_index`, `_entries`, `_prune` | Navigation |
| `bdk_contents_state` | Fills a size-prefixed struct with URL, title, loading, progress, security, audio, capture, visibility, focus, zoom |
| `bdk_contents_session_state`, `bdk_contents_restore` | Serialised navigation state, as a buffer |
| `bdk_contents_set_muted`, `_set_zoom`, `_set_user_agent`, `_perform_command`, `_can_perform_command` | Page controls |
| `bdk_contents_discard`, `_set_auto_discardable` | Added (S15). Discard a hidden page's renderer; exempt a page from Chromium's automatic discarding. The `bdk_contents_t` handle stays valid across a discard, including when Chromium replaces the underlying WebContents. |
| `bdk_contents_find_start`, `_find_next`, `_find_previous`, `_find_stop`, `_find_present`, `bdk_contents_print` | Find in page and printing, started by the application (T-SWF-07). `find_start` takes the text and an optional size-prefixed options struct (`match_case`); `find_stop` takes whether to clear the selection. Results arrive as `BDK_CONTENTS_EVENT_FIND_RESULT` events with `{"matchCount","activeIndex","rect","final"}`. `find_present` and `print` raise a `find` and a `printing` UI request. |
| `bdk_contents_set_host_context` | Conditional on decision D1 |

Scripting, capture, translation and DevTools go through broker commands, not host functions, because automation uses them too.

### 5.3 Events

```c
typedef struct bdk_contents_event {
  size_t           struct_size;
  uint32_t         kind;            /* BDK_CONTENTS_EVENT_* from bdk_ids.h */
  bdk_contents_t   contents;
  bdk_string_t     url;
  bdk_string_t     detail_json;     /* fields specific to the kind */
  int32_t          net_error;
  uint32_t         flags;           /* main frame, same document, user initiated, error page */
} bdk_contents_event_t;

typedef void (*bdk_contents_event_cb)(void* context, const bdk_contents_event_t*);

BDK_EXPORT bdk_subscription_t bdk_contents_subscribe(bdk_contents_t, bdk_contents_event_cb, void* ctx, bdk_release_cb);
BDK_EXPORT bdk_subscription_t bdk_runtime_subscribe(bdk_runtime_t, bdk_runtime_event_cb, void* ctx, bdk_release_cb);
BDK_EXPORT void               bdk_unsubscribe(bdk_subscription_t);
```

Events are delivered in order. The struct and every string in it are valid until the callback returns.

### 5.4 Views and keyboard

```c
typedef enum bdk_native_kind {
  BDK_NATIVE_NSVIEW = 1, BDK_NATIVE_NSWINDOW = 2, BDK_NATIVE_NSAPPLICATION = 3, BDK_NATIVE_NSEVENT = 4,
  BDK_NATIVE_HWND = 16,
  BDK_NATIVE_X11_WINDOW = 32, BDK_NATIVE_WAYLAND_SURFACE = 33
} bdk_native_kind_t;

typedef struct bdk_native_handle { uint32_t kind; void* pointer; uint64_t id; } bdk_native_handle_t;

BDK_EXPORT bdk_status_t bdk_contents_native_view(bdk_contents_t, bdk_native_handle_t* out, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_contents_focus(bdk_contents_t);
BDK_EXPORT void         bdk_contents_set_keyboard_handler(bdk_contents_t, bdk_key_reserve_cb, bdk_key_unhandled_cb,
                                                          void* ctx, bdk_release_cb);
```

This is the one place platform objects cross, and only as an opaque tagged value. The binding casts it; the shared core never looks inside.

Attachment has no function. The engine observes the view entering and leaving a native window and moves the tab between host contexts itself.

### 5.5 Windows

| Function family | Purpose |
|---|---|
| `bdk_window_register`, `bdk_window_unregister` | Optional registration of a native window, with its kind |
| `bdk_window_set_anchor_provider` | A callback that returns a rectangle for a role |
| `bdk_window_set_request_handler` | Bounds, activation and close requests from extensions and pages |
| `bdk_window_for_contents`, `bdk_window_contents`, `bdk_window_active_contents` | Queries |

## 6. Broker

```c
typedef struct bdk_invoke_options {
  size_t         struct_size;
  bdk_profile_t  profile;        /* 0 for application-scoped commands */
  bdk_contents_t contents;       /* 0 unless the command targets a page */
} bdk_invoke_options_t;

BDK_EXPORT bdk_call_t bdk_invoke(bdk_runtime_t, uint32_t command_id, bdk_string_t arguments_json,
                                 const bdk_invoke_options_t*, bdk_result_cb, bdk_progress_cb /* may be null */,
                                 void* ctx, bdk_release_cb);
BDK_EXPORT bdk_call_t bdk_invoke_named(bdk_runtime_t, bdk_string_t command_name, bdk_string_t arguments_json,
                                       const bdk_invoke_options_t*, bdk_result_cb, bdk_progress_cb,
                                       void* ctx, bdk_release_cb);

BDK_EXPORT bdk_subscription_t bdk_subscribe(bdk_runtime_t, uint32_t event_id, bdk_string_t filter_json,
                                            const bdk_invoke_options_t*, bdk_event_cb, void* ctx, bdk_release_cb);
```

`bdk_invoke` is the single entry point for every command in [services.md](services.md). Calls made through the ABI have full authority; capability checks apply to the page and automation paths inside the engine, not here.

Application commands and events:

```c
typedef void (*bdk_app_command_cb)(void* context, bdk_context_t call_context, bdk_string_t input_json);
/* The handler answers later with one of: */
BDK_EXPORT void bdk_app_command_progress(bdk_context_t, double fraction, bdk_string_t message);
BDK_EXPORT void bdk_app_command_resolve(bdk_context_t, bdk_string_t output_json);
BDK_EXPORT void bdk_app_command_reject(bdk_context_t, bdk_string_t domain, bdk_string_t code, bdk_string_t message);
/* And can ask who called and whether the call was cancelled: */
BDK_EXPORT bdk_string_t bdk_app_command_caller_json(bdk_context_t);
BDK_EXPORT uint8_t      bdk_app_command_is_cancelled(bdk_context_t);

BDK_EXPORT bdk_status_t bdk_app_command_register(bdk_runtime_t, bdk_string_t name, bdk_string_t declaration_json,
                                                 bdk_app_command_cb, void* ctx, bdk_release_cb, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_app_event_declare(bdk_runtime_t, bdk_string_t name, bdk_string_t declaration_json, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_app_event_emit(bdk_runtime_t, bdk_string_t name, bdk_string_t payload_json,
                                           bdk_string_t target_json, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_app_setting_register(bdk_runtime_t, bdk_string_t declaration_json, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_page_handler_register(bdk_runtime_t, bdk_string_t route, uint32_t mode,
                                                  bdk_native_page_cb, void* ctx, bdk_release_cb, bdk_error_t*);
```

All registration functions must be called between `bdk_runtime_start` and `bdk_runtime_freeze_registrations`.

## 7. Interface definition format

Plan, M2, task T-ABI-03. One JSON file per namespace in `packages/core/interface/`. It extends the shape used by the feasibility fixture in `experiments/nx-multilang`.

```json
{
  "namespace": "history",
  "since": 1,
  "types": {
    "HistoryEntry": { "url": "url", "title": "string", "lastVisit": "timestamp", "visitCount": "int", "typedCount": "int" },
    "TimeRange": { "start": "timestamp?", "end": "timestamp?" }
  },
  "commands": [
    {
      "name": "history.search",
      "capability": "history.read",
      "callers": ["swift", "page", "session"],
      "profile": "required",
      "args": { "text": "string", "range": "TimeRange?", "limit": "int?", "cursor": "string?" },
      "result": { "items": "HistoryEntry[]", "nextCursor": "string?" },
      "paginated": true
    },
    {
      "name": "history.deleteAll",
      "capability": "history.write",
      "callers": ["swift", "page", "session"],
      "profile": "required",
      "requires": ["gesture"],
      "args": {},
      "result": "void"
    }
  ],
  "events": [
    { "name": "history.visited", "capability": "history.read", "payload": { "entry": "HistoryEntry", "source": "ChangeSource" } }
  ]
}
```

| Field | Meaning |
|---|---|
| `callers` | Which of Swift, page and session may call. A command without `page` can never be granted to a page. |
| `capability` | One capability name. The capability table is the set of these. |
| `profile` | `required`, `optional` or `none` |
| `requires` | `gesture`, `authentication`, or both |
| `raises` | A UI surface the command can raise |
| `paginated`, `progress`, `stream` | Call behaviour |
| `binary` | The result includes a binary buffer |
| `since` | The ABI version that introduced it |
| `types` | Structs, enums and aliases. Primitive types: `string`, `int`, `double`, `bool`, `url`, `timestamp`, `bytes`, `json`; `?` for optional, `[]` for arrays. |

Validation, run in CI with no Chromium:

- Names match `namespace.verb`. Capabilities match `namespace.level`. Nothing starts with `app.`.
- Every referenced type exists.
- A command that lists `page` cannot use a capability on the "never for pages" list.
- Identifiers in `bdk_ids.h` are assigned once and never reused. Removing a command retires its identifier.
- The generated output is byte-stable for the same input.

Decisions taken in T-ABI-03 where this section was silent (2026-11-18). The implementation is `packages/js/codegen`; the rules for using it are in `packages/core/docs/conventions.md`.

- Files in `interface/`: one `<namespace>.json` per namespace (the file name equals `namespace`), `policy.json` and `ids.json`. `policy.json` holds the "never for pages" list as data: `neverForPages` (`contents.*`, `automation.*`, `devtools.*`, `cookies.values`: the first three from configuration.md, the last from the services.md table, where Pages is "No") and `neverForSessions` (`passwords.reveal`). A command that lists `session` cannot use a `neverForSessions` capability. Entries are capability names or `namespace.*`.
- `types` entries are a struct (an object of field types), an enum (`{ "enum": ["a", "b"] }`) or an alias (a type expression). Type names are `PascalCase` and global across files. `void` is valid only as a result.
- A command or event name is `namespace.verb`, and the verb may have further dotted segments (`autofill.addresses.list`). A capability level may contain hyphens (`top-sites.read`). `capability` may be omitted on a command that needs none (`runtime.caller`) and is required on an event. `since` is required on the file and optional on a command or event. `profile`, `callers`, `args` and `result` are required on a command.
- A `paginated` command has `limit` and `cursor` in `args` and `items` and `nextCursor` in `result` (services.md section 2).
- `raises` names a surface in lower case, from the `surfaces` constants group.
- `constants` holds the identifier families of the ABI itself (error domains and codes, event kinds, flags, security and visibility values, surfaces) as groups with a macro prefix and explicit values. They generate `#define` lines in `bdk_ids.h` and are recorded in the registry like command identifiers.
- `ids.json` is the registry of every identifier ever assigned. Commands and events are numbered from 1 in the order they first appear (files in name order). A removed name is marked `retired` and its identifier is not reused. The generated macros are `BDK_COMMAND_<NAME>` and `BDK_EVENT_<NAME>`.
- The M3 namespaces are defined (T-SVC-01, 2026-11-19): runtime, profiles, settings, history, bookmarks, downloads, permissions, cookies, privacy, passwords, autofill, system, with the argument and result types services.md gives. The generator also writes `generated/bdk_command_table.inc`, the tables the broker reads (identifier, capability, callers, profile mode, marks, argument types, and the policy lists). A command key `authenticateWhen` names an argument that makes the call need authentication only when it is present (`passwords.update`). The checks the broker makes, the argument rules, the 4 MiB cap and the new error codes are in `packages/core/docs/conventions.md`, "The broker".
- `bdk_invoke` has full authority and an identifier or a name finds a built-in command as well as an `app.` command. A `void` result is the JSON value `null`.

## 8. UI requests

```c
typedef struct bdk_ui_request_info {
  size_t          struct_size;
  bdk_request_t   request;
  uint32_t        surface;          /* BDK_SURFACE_* */
  bdk_contents_t  contents;         /* 0 if none */
  bdk_window_t    window;           /* 0 if none */
  bdk_profile_t   profile;
  bdk_string_t    payload_json;     /* the fields in ui-requests.md for this surface */
  double          anchor[4];        /* x, y, width, height in window coordinates; all zero if none */
  bdk_native_handle_t secure_view;  /* for popups, side panels and hosted front ends */
} bdk_ui_request_info_t;

typedef enum bdk_ui_response { BDK_UI_HANDLED = 1, BDK_UI_USE_DEFAULT = 2, BDK_UI_REJECT = 3 } bdk_ui_response_t;
typedef bdk_ui_response_t (*bdk_ui_handler_cb)(void* context, const bdk_ui_request_info_t*);

BDK_EXPORT bdk_status_t bdk_ui_set_handler(bdk_runtime_t, uint32_t surface, bdk_ui_handler_cb, void* ctx,
                                           bdk_release_cb, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_ui_complete(bdk_request_t, bdk_string_t completion /* "grant", "deny", "accept", … */,
                                        bdk_string_t arguments_json, bdk_native_handle_t gesture_event,
                                        bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_ui_use_default(bdk_request_t, bdk_error_t*);
BDK_EXPORT void         bdk_ui_set_withdrawn_handler(bdk_request_t, void (*cb)(void*), void* ctx, bdk_release_cb);
BDK_EXPORT void         bdk_ui_request_release(bdk_request_t);    /* an uncompleted request is cancelled */
```

Completions are named in the interface definition for each surface, with the same validation as commands. A completion marked as granting requires `gesture_event`; the engine validates it and returns `ui.gestureRequired` otherwise.

## 9. Providers, automation, diagnostics

| Header | Function families |
|---|---|
| `bdk_provider.h` | `bdk_provider_register(service, vtable, ctx)` where the vtable is a size-prefixed struct of callbacks: `supported_pairs`, `prepare`, `translate`, `detect` for translation; `check_url`, `check_download`, `update` for reputation; `start`, `stop` for geolocation; `list`, `get_secret`, `save`, `remove` for credentials (Added, S14); `check_credential` for leak check (Added, S16). Each callback receives a context it resolves or rejects later, as application commands do. `bdk_provider_set_key(service, key)`. |
| `bdk_automation.h` | `bdk_session_create(options_json)`, `bdk_session_revoke`, `bdk_session_grant_contents`, `bdk_session_run(script, arguments_json)` returning a task; `bdk_task_cancel`; `bdk_task_subscribe`; `bdk_automation_endpoint_open(authenticator_cb)`, `bdk_automation_endpoint_close`; `bdk_automation_subscribe_audit` |
| `bdk_diagnostics.h` | `bdk_log_set_handler`, `bdk_crash_reports_list`, `bdk_crash_reports_set_handler`, `bdk_netlog_start`, `bdk_netlog_stop` |

Provider callbacks may be invoked off the UI thread if the provider declares it; this is the only ABI surface where that is allowed, and the header marks each such callback.

## 10. The stub runtime and the conformance suite

Plan, M1, tasks T-ABI-02 and T-ABI-05.

| | Stub runtime | Real runtime |
|---|---|---|
| Location | `packages/core/src/stub` | `chromium/bdk` |
| Implements | Every exported function, in memory | The same, over Chromium |
| Pages | Navigation state machines with scripted outcomes. No rendering, no JavaScript. | Real |
| Services | In-memory stores for history, bookmarks, downloads, settings, permissions, passwords | Chromium's |
| UI requests | Raised by the test | Raised by Chromium |
| Capability checks | The same generated table, the same check order | The same |
| Runs in | Hosted CI on Linux and macOS | A developer's Mac; hosted macOS against the last published runtime |

The conformance suite is one set of tests written against the ABI. It runs against both. What it covers:

- Handle rules: stale, mistyped and released handles; double release.
- Struct growth: a caller that declares a smaller `struct_size` than the runtime knows, and a larger one.
- Callback rules: exactly-once results, release callbacks, no synchronous callbacks, cancellation.
- Thread rules.
- Every command in the interface definitions, for argument validation and result shape.
- The check order in [services.md](services.md) section 2, for a page caller and a session caller, using a test-only way to act as each.
- The UI-request rules in [ui-requests.md](ui-requests.md) section 3.
- Start-up: version mismatch, hash mismatch, invalid resolved configuration, registration after the freeze.

A behaviour that the stub and the real runtime could disagree on belongs in the suite.

## 11. Cross-platform checklist

Applied to every ABI change (roadmap section 4, guardrail 4).

| Question | Must be |
|---|---|
| Does a signature or struct name an AppKit, Win32 or toolkit type? | No. Use `bdk_native_handle_t`. |
| Does it assume a main-thread run loop the host owns? | Only through `bdk_post_to_ui_thread` |
| Is the ownership of every pointer and handle written down? | Yes |
| Is there a plausible Windows implementation, and a plausible Linux one? | Named in the review |
| Can C# call it without unsafe tricks: blittable structs, no callbacks that return structs by value, no variadics? | Yes |
| Are coordinates defined? | Window coordinates in device-independent pixels, origin top-left, on every platform. The macOS binding converts. |
| Is a path a UTF-8 string, not a file-system representation? | Yes |

## 12. Evolution

| Change | Allowed without raising the ABI version |
|---|---|
| Add a function | Yes. The hash changes, so SDK and runtime still move together. |
| Append a field to a size-prefixed struct | Yes |
| Add a command, an event, a capability, an enum value or an error code | Yes |
| Add a required argument to a command | No |
| Change the meaning or type of a field, argument or result | No |
| Remove or rename anything | No |
| Reuse a retired identifier | Never |

Before 1.0 the version may be raised in any `0.y` release. After 1.0 a raise is a major version of BDK (tooling document 8.1). The hash is checked for an exact match regardless, because the Swift package and the runtime ship as one version; the version number exists so that the failure message can say which side is older.
