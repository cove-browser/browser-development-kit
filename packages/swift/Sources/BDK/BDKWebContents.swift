import AppKit
import CBDK
import Foundation
import os

/// The serialised navigation state of a page, as the engine wrote it. Opaque and versioned: a state
/// from a newer engine is refused with `contents.stateTooNew`.
public struct BDKSessionState: Sendable, Equatable, Codable {
    public let data: Data

    public init(data: Data) {
        self.data = data
    }
}

/// One page, with its own navigation history. Create it with `profile.webContents.create()`.
@MainActor
public final class BDKWebContents: Identifiable {
    public struct ID: Hashable, Sendable {
        public let rawValue: UInt64
    }

    public struct Options: Sendable {
        public var initialURL: URL?
        public var restoring: BDKSessionState?

        public init(initialURL: URL? = nil, restoring: BDKSessionState? = nil) {
            self.initialURL = initialURL
            self.restoring = restoring
        }
    }

    public let id: ID
    public let profile: BDKProfile
    /// The application's side of keyboard handling (swift-api.md section 9).
    public weak var keyboardDelegate: (any BDKKeyboardDelegate)?
    /// The events of this page. Each iterator sees every event from when it was created.
    public var events: BDKEventSequence<Event> {
        BDKEventSequence(broadcaster)
    }

    /// True once the page was closed, by `close()` or by the page itself.
    public private(set) var isClosed = false
    /// Set only on a page launched as an installed web app: the app, and whether the page is in its
    /// scope. Navigating an ordinary page to an app's start URL does not set it (web-apps.md 4.2).
    public private(set) var webApp: BDKWebAppPageState?

    package private(set) var handle: bdk_contents_t
    let broadcaster = EventBroadcaster<Event>()
    private var hostView: BDKWebView?
    var findSession: FindSession?
    var devToolsSession: DevTools?
    private var lastState = State()

    /// False once the page was destroyed, or its profile or runtime went away.
    public var isAlive: Bool {
        handle != 0
    }

    private init(profile: BDKProfile, handle: bdk_contents_t) {
        self.profile = profile
        self.handle = handle
        self.id = ID(rawValue: handle)
    }

    isolated deinit {
        guard handle != 0 else { return }
        // A page that closed itself (a default-presented one the runtime held, say) needs no close.
        if !isClosed {
            Logger(subsystem: "BDK", category: "lifecycle")
                .warning("page \(self.id.rawValue) was released without close(); destroying it")
        }
        _ = bdk_contents_destroy(handle)
        EventLoop.kick()
    }

    // MARK: Creation and lifetime

    static func create(in profile: BDKProfile, options: Options) throws -> BDKWebContents {
        let runtime = try profile.runtime.liveHandle()
        let profileHandle = try profile.liveHandle()
        var handle: bdk_contents_t = 0
        try withBDKString(options.initialURL?.absoluteString ?? "") { url in
            try abiCall(.contents) { error in
                var abiOptions = bdk_contents_options_t()
                abiOptions.struct_size = MemoryLayout<bdk_contents_options_t>.size
                abiOptions.profile = profileHandle
                abiOptions.initial_url = url
                return bdk_contents_create(runtime, &abiOptions, &handle, error)
            }
        }
        let contents = BDKWebContents(profile: profile, handle: handle)
        contents.installBridges()
        profile.runtime.register(contents)
        if let state = options.restoring {
            do {
                try contents.restore(state)
            } catch {
                contents.destroy()
                throw error
            }
        }
        return contents
    }

    /// Wraps a page the engine created for a request (a new window, a print preview). The page is
    /// the application's from here on, as one made with `create`, unless the request goes to the
    /// default presenter: then the runtime holds it until it closes (`BDKUIRequest.adoptPage`).
    static func adopt(handle: bdk_contents_t, in profile: BDKProfile) -> BDKWebContents {
        if let existing = profile.runtime.webContents(handle: handle) { return existing }
        let contents = BDKWebContents(profile: profile, handle: handle)
        contents.installBridges()
        profile.runtime.register(contents)
        return contents
    }

    /// Asks the page to close, waits for it to report `closed`, then destroys it. A page that
    /// already closed itself is destroyed at once.
    public func close() async throws {
        var iterator = events.makeAsyncIterator()
        if !isClosed {
            let handle = try liveHandle()
            try abiCall(.contents) { bdk_contents_close(handle, $0) }
            while !isClosed, await iterator.next() != nil {}
        }
        destroy()
    }

    /// Destroys the page now, without unload handlers. Later calls throw `contents.stale`.
    public func destroy() {
        guard handle != 0 else { return }
        _ = bdk_contents_destroy(handle)
        EventLoop.kick()
        invalidate()
    }

    /// Forgets the native handle after the engine released it, and ends the event sequences.
    func invalidate() {
        guard handle != 0 else { return }
        handle = 0
        profile.runtime.unregister(self)
        broadcaster.finish()
        hostView?.contentsWasInvalidated()
    }

    func liveHandle() throws -> bdk_contents_t {
        guard handle != 0, profile.runtime.state == .running else {
            throw BDKError.stale(.contents)
        }
        return handle
    }

    func noteEvent(_ event: Event) {
        if case .closed = event {
            isClosed = true
            profile.runtime.pageClosed(self)
        }
        if case .webAppScope(let appID, let isInScope) = event, let state = webApp,
            state.app.id == appID
        {
            webApp = BDKWebAppPageState(app: state.app, isInScope: isInScope)
        }
    }

    /// The page was launched as this installed app (a `.webApp` new-window request, or
    /// `profile.webApps.launch`). It starts in the app's scope.
    func noteWebApp(_ app: BDKWebAppInfo) {
        if webApp?.app.id != app.id { webApp = BDKWebAppPageState(app: app, isInScope: true) }
    }

    // MARK: State

    public enum Security: Sendable, Equatable {
        case unknown
        case insecure
        case secure
        case broken
    }

    public enum Visibility: Sendable, Equatable {
        case hidden
        case visible
    }

    public struct State: Sendable, Equatable {
        public var url: URL?
        public var title = ""
        public var isLoading = false
        /// 0.0 to 1.0.
        public var estimatedProgress = 0.0
        public var security = Security.unknown
        public var isAudible = false
        public var isMuted = false
        public var visibility = Visibility.hidden
        public var isFocused = false
        /// 1.0 is 100 percent.
        public var zoom = 1.0
    }

    /// Reads the page's state from the engine.
    public func state() throws -> State {
        let handle = try liveHandle()
        var abi = bdk_contents_state_t()
        abi.struct_size = MemoryLayout<bdk_contents_state_t>.size
        try abiCall(.contents) { bdk_contents_state(handle, &abi, $0) }
        var state = State()
        state.url = URL(string: swiftString(abi.url))
        state.title = swiftString(abi.title)
        state.isLoading = abi.loading != 0
        state.estimatedProgress = abi.progress
        state.security = Self.security(abi.security)
        state.isAudible = abi.audio & BDK_AUDIO_AUDIBLE != 0
        state.isMuted = abi.audio & BDK_AUDIO_MUTED != 0
        state.visibility = abi.visibility == BDK_VISIBILITY_VISIBLE ? .visible : .hidden
        state.isFocused = abi.focused != 0
        state.zoom = abi.zoom
        lastState = state
        return state
    }

    private static func security(_ value: UInt32) -> Security {
        switch value {
        case BDK_SECURITY_NONE: .insecure
        case BDK_SECURITY_SECURE: .secure
        case BDK_SECURITY_BROKEN: .broken
        default: .unknown
        }
    }

    // The properties below read the engine and, once the page is gone, return the last values read.
    public var url: URL? { current.url }
    public var title: String { current.title }
    public var isLoading: Bool { current.isLoading }
    public var estimatedProgress: Double { current.estimatedProgress }
    public var security: Security { current.security }
    public var isAudible: Bool { current.isAudible }
    public var isMuted: Bool { current.isMuted }
    public var visibility: Visibility { current.visibility }
    public var isFocused: Bool { current.isFocused }
    /// The current state, or the last one read when the page is gone.
    var current: State {
        (try? state()) ?? lastState
    }

    // MARK: Settings

    public func setMuted(_ muted: Bool) throws {
        let handle = try liveHandle()
        try abiCall(.contents) { bdk_contents_set_muted(handle, muted ? 1 : 0, $0) }
    }

    /// 1.0 is 100 percent. A value that is not positive throws `broker.invalidArguments`.
    public func setZoom(_ zoom: Double) throws {
        let handle = try liveHandle()
        try abiCall(.contents) { bdk_contents_set_zoom(handle, zoom, $0) }
    }

    public func setUserAgent(_ userAgent: String) throws {
        let handle = try liveHandle()
        try abiCall(.contents) { error in
            withBDKString(userAgent) { bdk_contents_set_user_agent(handle, $0, error) }
        }
    }

    /// Releases the page's renderer to save memory. The object stays valid.
    public func discard() throws {
        let handle = try liveHandle()
        try abiCall(.contents) { bdk_contents_discard(handle, $0) }
    }

    /// False exempts the page from automatic discarding, for example a pinned page.
    public func setAutoDiscardable(_ discardable: Bool) throws {
        let handle = try liveHandle()
        try abiCall(.contents) {
            bdk_contents_set_auto_discardable(handle, discardable ? 1 : 0, $0)
        }
    }

    // MARK: Session state

    public func sessionState() throws -> BDKSessionState {
        let handle = try liveHandle()
        var buffer: bdk_buffer_t = 0
        try abiCall(.contents) { bdk_contents_session_state(handle, &buffer, $0) }
        return BDKSessionState(data: takeBuffer(buffer))
    }

    /// Replaces the navigation entries. A state from a newer engine throws `contents.stateTooNew`.
    public func restore(_ state: BDKSessionState) throws {
        let handle = try liveHandle()
        try state.data.withUnsafeBytes { raw in
            let bytes = bdk_bytes_t(
                data: raw.bindMemory(to: UInt8.self).baseAddress, length: raw.count)
            try abiCall(.contents) { bdk_contents_restore(handle, bytes, $0) }
        }
    }

    // MARK: Focus

    /// Makes this page the focused one.
    public func focus() throws {
        let status = bdk_contents_focus(try liveHandle())
        EventLoop.kick()
        try BDKError.check(status, error: 0, subject: .contents)
    }

    // MARK: Native view

    /// The engine's native view, or nil when the runtime has none (the stub). An unexpected
    /// failure throws.
    func nativeView() throws -> NSView? {
        let handle = try liveHandle()
        var native = bdk_native_handle_t()
        do {
            try abiCall(.contents) { bdk_contents_native_view(handle, &native, $0) }
        } catch let failure as BDKError where failure.domain == .platform {
            if failure.code == "unsupported" { return nil }
            throw failure
        }
        guard native.kind == BDK_NATIVE_NSVIEW.rawValue, let pointer = native.pointer else {
            return nil
        }
        return Unmanaged<NSView>.fromOpaque(pointer).takeUnretainedValue()
    }

    /// The page's view. Always the same object. Adding it to a window registers that window; moving
    /// it to another window does not recreate the page.
    public var view: BDKWebView {
        if let hostView { return hostView }
        let created = BDKWebView(contents: self)
        hostView = created
        return created
    }
}
