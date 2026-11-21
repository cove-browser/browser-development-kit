import CBDK
import Foundation

/// How a logical route is served (swift-api.md section 4.2, architecture.md section 8.2).
public enum PageMode: Sendable {
    /// Serve Chromium's built-in page. The route needs one (`config.noBuiltin` otherwise).
    case chromium
    /// Serve the bundled page that is already in the resolved configuration for this route.
    case bundled
    /// Load no page: the handler decides what the user sees. It runs on the main actor, and the
    /// navigation is cancelled in the page that asked.
    case native(@MainActor (NativePageRequest) -> NativePageResult)
    /// Show the standard error page.
    case disabled
}

/// A navigation to a route in `native` mode.
public struct NativePageRequest {
    /// The logical route, as in the resolved configuration: "downloads".
    public let route: String
    /// The URL that was navigated to: `cove://downloads/recent?x=1`.
    public let url: URL
    /// "/recent".
    public let path: String
    /// The query of `url`, percent-decoded.
    public let query: [URLQueryItem]
    /// The page whose navigation asked for the route. Nil when the application does not hold it:
    /// a page it did not create, or one that is already closed.
    public let source: BDKWebContents?
    public let profile: BDKProfile
}

public enum NativePageResult: Sendable {
    /// The handler took over; nothing is loaded.
    case handled
    /// Show the standard error page in the page that asked.
    case blocked
}

/// The routes the application serves in a way `bdk.toml` could not express: `config.pages`. A
/// registration outranks the TOML override and an auto-discovered directory. Registering a route
/// again replaces the earlier registration, so the last one wins and one registration per route
/// reaches the runtime.
public struct PageRegistrations: Sendable {
    struct Entry: Sendable {
        let route: String
        let mode: PageMode
    }

    private(set) var entries: [Entry] = []

    public init() {}

    /// Sets how `route` is served. The runtime checks it at start: a start error is
    /// `config.noBuiltin` (`.chromium` without a built-in page), `config.routeUnknown` (`.bundled`
    /// without a bundled page), `config.routeMode` (`.disabled` on a route that cannot be),
    /// `config.routeInvalid` (an alias, a reserved name or no route name at all) or
    /// `config.handlerMissing` (a native route in the resolved file with no `.native` registration).
    public mutating func register(_ route: String, _ mode: PageMode) {
        entries.removeAll { $0.route == route }
        entries.append(Entry(route: route, mode: mode))
    }

    /// Hands every registration to the runtime. A registration, so before the freeze.
    @MainActor
    func adopt(into runtime: BDKRuntime) throws {
        let handle = try runtime.liveHandle()
        for entry in entries {
            try register(entry, runtime: runtime, handle: handle)
        }
    }

    @MainActor
    private func register(_ entry: Entry, runtime: BDKRuntime, handle: bdk_runtime_t) throws {
        let mode: UInt32
        var box: Unmanaged<NativePageBox>?
        switch entry.mode {
        case .chromium: mode = UInt32(BDK_PAGE_MODE_CHROMIUM)
        case .bundled: mode = UInt32(BDK_PAGE_MODE_BUNDLED)
        case .disabled: mode = UInt32(BDK_PAGE_MODE_DISABLED)
        case .native(let handler):
            mode = UInt32(BDK_PAGE_MODE_NATIVE)
            box = Unmanaged.passRetained(NativePageBox(handler: handler, runtime: runtime))
        }
        do {
            try withBDKString(entry.route) { route in
                try abiCall(.runtime) {
                    bdk_page_handler_register(
                        handle, route, mode, box == nil ? nil : nativePageCallback,
                        box?.toOpaque(), box == nil ? nil : nativePageReleaseCallback, $0)
                }
            }
        } catch {
            // A refused registration leaves the context with the caller.
            box?.release()
            throw error
        }
    }
}

// MARK: - Callbacks

@MainActor
private final class NativePageBox {
    let handler: @MainActor (NativePageRequest) -> NativePageResult
    weak var runtime: BDKRuntime?

    init(
        handler: @escaping @MainActor (NativePageRequest) -> NativePageResult,
        runtime: BDKRuntime
    ) {
        self.handler = handler
        self.runtime = runtime
    }

    /// Runs the handler and answers the navigation. A request that cannot be shown to the
    /// handler, because its JSON is malformed or the application does not hold the profile,
    /// is answered `blocked`: the page was not loaded either way.
    func deliver(call: bdk_context_t, requestJSON: String) {
        let result: NativePageResult
        if let request = makeRequest(call: call, requestJSON: requestJSON) {
            result = handler(request)
        } else {
            result = .blocked
        }
        bdk_native_page_complete(
            call,
            result == .handled ? UInt32(BDK_NATIVE_PAGE_HANDLED) : UInt32(BDK_NATIVE_PAGE_BLOCKED))
        EventLoop.kick()
    }

    private func makeRequest(call: bdk_context_t, requestJSON: String) -> NativePageRequest? {
        guard let runtime,
            case .object(let fields) = BDKJSValue(jsonString: requestJSON),
            let route = fields["route"]?.stringValue,
            let urlText = fields["url"]?.stringValue, let url = URL(string: urlText),
            let profile = runtime.profiles.loaded(handle: bdk_app_command_profile(call))
        else { return nil }
        var components = URLComponents()
        components.percentEncodedQuery = fields["query"]?.stringValue
        return NativePageRequest(
            route: route, url: url, path: fields["path"]?.stringValue ?? "",
            query: components.queryItems ?? [],
            source: runtime.webContents(handle: bdk_native_page_source(call)), profile: profile)
    }
}

private let nativePageCallback: bdk_native_page_cb = { context, call, request in
    guard let context else { return }
    let box = Unmanaged<NativePageBox>.fromOpaque(context).takeUnretainedValue()
    let requestJSON = swiftString(request)
    MainActor.assumeIsolated { box.deliver(call: call, requestJSON: requestJSON) }
}

private let nativePageReleaseCallback: bdk_release_cb = { context in
    guard let context else { return }
    Unmanaged<NativePageBox>.fromOpaque(context).release()
}
