# Cross-platform review of the ABI and service contracts

Task T-XPL-01 (M9). The C ABI (`packages/core/include/bdk`, `bdk_mac`), the runtime core (`packages/core/runtime`)
and the service contracts (`packages/core/interface`, [sdk/services.md](../planning/sdk/services.md)) reviewed
against what a Windows host, a Linux host and a future .NET binding need. Reviewed on 2026-11-25 at ABI version
1; the hash is whatever `packages/core/generated/abi-hash.txt` held then (this review changed no public header).
Checklist: [architecture.md](../planning/architecture.md) section 14, roadmap section 4,
[packages/core/docs/conventions.md](../../packages/core/docs/conventions.md) "Cross-platform checklist".

Each finding is **resolved** (fixed in this change, or already right and now written down) or **recorded**
(left for the task named, with what it needs). Nothing recorded here blocks macOS v1.

## What already holds

- No platform type in `include/bdk`: native objects cross as `bdk_native_handle_t {kind, pointer, id}`, with
  kinds for `HWND`, X11 windows and Wayland surfaces next to the AppKit ones. The one Objective-C type is in
  `include/bdk_mac`, outside the hash's `include/bdk` and outside the C ABI.
- Every struct passed by pointer starts with `size_t struct_size`; booleans are `uint8_t`, flags and kinds are
  `uint32_t`, never `bool` or an enum-typed field. Layout is natural alignment only, identical under the SysV
  AMD64, AAPCS64 and Microsoft x64 ABIs, and maps one to one onto `[StructLayout(LayoutKind.Sequential)]`.
- Handles are `uint64_t`, not pointers; zero is invalid; a stale handle is an error, not a crash. A .NET
  binding can wrap them in `SafeHandle`-like types with no pinning.
- Strings are UTF-8 with an explicit length (`bdk_string_t`), never NUL-terminated by contract, never
  `wchar_t`. Payloads are JSON. A .NET binding marshals with `Encoding.UTF8` and `System.Text.Json`.
- Callbacks are plain C function pointers with a `void* context` and a release callback that runs exactly
  once. That is what .NET's `[UnmanagedCallersOnly]` function pointers and a `GCHandle` passed as the context
  need: the release callback is where the binding frees the handle.
- No callback runs synchronously from the call that causes it, so a managed caller never re-enters BDK from
  inside a P/Invoke.
- The ABI hash normalises CRLF to LF, so a Windows checkout with `core.autocrlf` hashes the same.
- The core is C++23 with no platform header except `local_socket_posix.cc`, behind `local_socket.h`; the stub
  builds on macOS and Linux; the headers compile as C11 and C++23.

## Findings

| # | Area | Finding | Resolution |
|---|---|---|---|
| X1 | Gesture provenance | The core decided whether a grant was user-confirmed by comparing with `BDK_NATIVE_NSEVENT` in three places (`ui_router.cc`, twice in `core_windows.cc`): a macOS constant in shared code. | **Resolved**: one function, `is_gesture_event` (`ui_router.cc`, declared in `core_internal.h`); the other two call it. A Windows or Linux runtime changes that function only. |
| X2 | Input event kinds | `bdk_native_kind_t` has no kind for a Windows input message or a Linux (GDK or X11/Wayland) input event, so a key or gesture event cannot cross on those hosts (`bdk_key_reserve_cb`, `bdk_ui_complete`, `bdk_extension_*` with a gesture). | **Recorded** for T-XPL-02: add `BDK_NATIVE_WIN32_MSG` (pointer to `MSG`) and a Linux event kind when the first non-mac runtime exists. Additive enum values; a minor ABI change that regenerates the hash. Not added now, to keep the hash stable while M9 changes it elsewhere. |
| X3 | `BDK_EXPORT` on Windows | `bdk_base.h` defines `BDK_EXPORT` as `__declspec(dllexport)` for everyone. A C or C++ application that includes the headers on Windows would declare the functions dllexport instead of dllimport: it links, through the import library, but with the wrong declaration and an extra thunk per call. .NET is unaffected (it does not read the headers). | **Recorded** for T-XPL-02: `#if defined(BDK_BUILDING_RUNTIME) dllexport #else dllimport`, defined by the runtime's build only. Changes the hash, so it lands with the Windows build. |
| X4 | Calling convention and word size | No calling convention is named. On x64 and arm64 there is one per OS, so none is needed; on 32-bit Windows `cdecl` and `stdcall` differ. | **Resolved** by scope: BDK supports 64-bit hosts only (arm64 macOS now; x64 and arm64 Windows and Linux later). Written in conventions.md. A 32-bit host is not planned. |
| X5 | Structs returned by value | `bdk_error_domain` and similar return a 16-byte `bdk_string_t`; Microsoft x64 returns it through a hidden pointer, SysV in two registers. Compilers and .NET's marshaller handle both for blittable structs. | **Resolved**: noted for binding authors in conventions.md; no change. |
| X6 | Paths | The ABI says paths are UTF-8 (`user_data_path_override`, `bundle_path`, `framework_path`). Windows file APIs take UTF-16, and `std::filesystem::path(std::string)` reads the ANSI code page there. | **Resolved** for the code this review added (`profile_versions.cc` builds paths from `char8_t`, which is UTF-8 on every platform); **recorded** as a rule for the Windows backend: convert at the edge with `base::FilePath::FromUTF8Unsafe` or `char8_t`, never from `std::string`. Added to conventions.md. |
| X7 | Path field names | `bundle_path` and `framework_path` are macOS words. On Windows they would be the application directory and the directory holding the runtime DLL. | **Recorded**: the fields keep their names (renaming breaks the ABI); their meaning per platform goes into `bdk_runtime.h`'s comment with T-XPL-02. |
| X8 | Screen coordinates | Window coordinates are defined (DIP, origin top-left of the window); screen coordinates (`setFrame` in `bdk_window_request_cb`, the Swift `BDKWindow` frames) were not. The engine converts with `gfx::ScreenRectToNSRect`, so they are Chromium's: DIP, origin at the top-left of the primary display, y downwards. | **Resolved**: written in conventions.md. That is also what Windows and X11 use natively, so only macOS converts. |
| X9 | The application object | `bdk_mac/bdk_application.h` exists because Chromium needs the `NSApplication` subclass. Windows and Linux have no equivalent object; their event-loop integration (a message loop; GLib) is the runtime's own. | **Resolved**: no ABI counterpart is needed; `bdk_start_config_t.native_app` is optional and zero there. |
| X10 | Automation endpoint | The local endpoint is a Unix-domain socket in a directory only the user can read (`local_socket_posix.cc`). Windows has `AF_UNIX` since Windows 10 1803 but no file-mode permissions on it; a named pipe with a DACL for the current user is the equivalent. | **Recorded** for T-XPL-02: `local_socket_win.cc` behind `local_socket.h`, with the same "local, authenticated, off by default" invariant (architecture 16, invariant 7). The protocol above the socket is unchanged. |
| X11 | Threading | "The UI thread is the thread that called `bdk_runtime_start`." Chromium on Windows and Linux also runs its UI on the main thread's message loop, so the rule holds. A .NET application's UI thread (WPF, WinUI, Avalonia) must be that thread, and callbacks arrive on it, so a binding posts nothing. | **Resolved**: no change. A .NET binding must call `bdk_runtime_start` on its UI thread; noted in conventions.md. |
| X12 | Credential store key | `[storage] keychain_item` names a Keychain item. Windows (DPAPI) has no item name; Linux's Secret Service has an application attribute. | **Recorded**: the key stays as the macOS value; a platform-neutral `[storage] secret_name` or per-platform keys is decided with the CLI's Windows module (T-XPL-02). `bdk.toml` additions are minor. |
| X13 | Service contracts | Three commands describe macOS behaviour in their documentation: `downloads.reveal` ("Show in Finder"), `system.defaultBrowser.request` ("macOS shows its own confirmation") and `contents.requestAttention` (Dock bounce). Each has an equivalent: Explorer's select, Windows' Default Apps settings or `xdg-settings`, `FlashWindowEx` or an urgency hint. Their arguments and results carry nothing platform-specific. | **Resolved**: the contracts stay; on Windows `defaultBrowser.request` can only open the system's settings, which its result (`void`) already allows. services.md's wording is per platform when T-XPL-02 adds one. |
| X14 | Generated bindings | `bdk_ids.h`, the Swift and TypeScript declarations come from `interface/*.json` through `packages/js/codegen`. A C# binding would be one more generator over the same files: constants, command names, argument and result types. | **Resolved**: nothing in the interface definitions is Swift- or TypeScript-specific; a C# generator is future work with the .NET binding (not planned in v1). |
| X15 | Packaging and signing | Signing, notarization, helper bundles and SwiftPM are macOS-only, and are in the CLI's `xcode` module and `tooling/`. | **Resolved**: already behind the platform module (sdk/cli.md 2). |
| X16 | Tooling | `tooling/src/lock.ts` already has one function for `lockf` and `flock`; APFS clones in `chromium:experiment` refuse other filesystems; the runtime store's `hostTarget()` names `windows` and `linux`. The profile snapshot code uses `std::filesystem` only. | **Resolved**: no change. |

## What would close the recorded findings

T-XPL-02 (M10) compiles the platform-neutral engine code for Windows or Linux. With it: X2 and X3 (header
changes, one hash regeneration), X7's comment, X10's socket, and X12's configuration key. The conformance suite
then runs on that host against the stub, as it does on Linux today.
