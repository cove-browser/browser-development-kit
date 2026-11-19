import AppKit
import BDK
import CBDK
import CBDKStub
import Foundation

extension BDKConfiguration {
    /// A configuration the stub runtime accepts: an empty resolved configuration. Each access
    /// returns a new one, because a configuration's `ui` handlers are shared by its copies.
    public static var stub: BDKConfiguration {
        BDKConfiguration(resolvedJSON: "{}")
    }
}

extension BDKRuntime {
    /// Starts the in-memory runtime in this process and, unless told not to, freezes its registrations. It renders
    /// nothing and runs no JavaScript. The stub is pumped from the main run loop whenever work is
    /// pending, so `await`ing an asynchronous call in a main-actor test does not deadlock.
    @discardableResult
    public static func startStub(
        _ configuration: BDKConfiguration = .stub, freezingRegistrations: Bool = true
    ) throws -> BDKRuntime {
        StubDriver.install()
        return try startRuntime(configuration, freezingRegistrations: freezingRegistrations)
    }

    /// Controls of the in-memory runtime. Only meaningful for a runtime from `startStub`.
    public var stub: BDKStub {
        BDKStub(runtime: self)
    }
}

/// Pumps the stub: after every ABI call, and on a short timer for work posted from other threads
/// with `bdk_post_to_ui_thread`. Chromium pumps itself in the real runtime.
@MainActor
private enum StubDriver {
    private static var isPumpScheduled = false
    private static var timer: DispatchSourceTimer?
    /// While above zero, neither the timer nor an ABI call pumps the stub.
    static var pauses = 0

    static func install() {
        EventLoop.scheduler = { schedule() }
        guard timer == nil else { return }
        let source = DispatchSource.makeTimerSource(queue: .main)
        source.schedule(deadline: .now(), repeating: .milliseconds(5))
        source.setEventHandler {
            MainActor.assumeIsolated {
                if pauses == 0 { _ = bdk_stub_pump() }
            }
        }
        source.resume()
        timer = source
    }

    static func schedule() {
        guard pauses == 0, !isPumpScheduled else { return }
        isPumpScheduled = true
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                isPumpScheduled = false
                // Paused since this was queued: the resume schedules the pump again.
                guard pauses == 0 else { return }
                _ = bdk_stub_pump()
            }
        }
    }
}

/// The hooks of the in-memory runtime: run its queue, script navigations, raise UI requests,
/// press keys.
@MainActor
public struct BDKStub {
    public enum UIOutcomeKind: Sendable, Equatable {
        case none
        case completed
        case defaulted
        case rejected
        case cancelled
        case withdrawn
        case disabled
    }

    /// What happened to the most recently raised UI request.
    public struct UIOutcome: Sendable, Equatable {
        public let kind: UIOutcomeKind
        public let surface: UInt32
        public let completion: String
        public let argumentsJSON: String
    }

    let runtime: BDKRuntime

    /// Runs every queued callback and simulated step on this thread, in order, until the queue is
    /// empty. Returns the number run. Tests call this to settle the runtime deterministically.
    @discardableResult
    public func pump() -> Int {
        bdk_stub_pump()
    }

    /// Runs `body` with automatic pumping stopped, so a test can start work and act on it before
    /// the stub advances it (a load that is still in flight when a second one starts, say). Nothing
    /// queued runs until `body` returns, except through `pump()`; then pumping resumes.
    public func withAutomaticPumpingPaused<Result>(
        _ body: () async throws -> Result
    ) async rethrows -> Result {
        StubDriver.pauses += 1
        defer {
            StubDriver.pauses -= 1
            StubDriver.schedule()
        }
        return try await body()
    }

    /// Scripts the outcome of every later navigation to `url`: it fails with `netError`, or
    /// commits with `title` (the URL when nil).
    public func scriptNavigation(to url: URL, title: String? = nil, netError: Int32 = 0) throws {
        let handle = try runtime.liveHandle()
        let status = withBDKStrings([url.absoluteString, title ?? ""]) { lent in
            var script = bdk_stub_navigation_script_t()
            script.struct_size = MemoryLayout<bdk_stub_navigation_script_t>.size
            script.net_error = netError
            script.title = lent[1]
            return bdk_stub_script_navigation(handle, lent[0], &script)
        }
        try check(status)
    }

    /// Raises a UI request as Chromium would. The handler for the surface runs on the next pump.
    /// With no handler the request takes the default.
    public func raiseUIRequest(
        surface: UInt32, on page: BDKWebContents? = nil, payloadJSON: String = "{}"
    ) throws {
        let handle = try runtime.liveHandle()
        let status = withBDKStrings([payloadJSON]) { lent in
            bdk_stub_raise_ui_request(handle, surface, page?.handle ?? 0, lent[0])
        }
        try check(status)
    }

    /// The raiser withdraws every unfinished request.
    public func withdrawUIRequests() throws {
        try check(bdk_stub_withdraw_ui_requests(try runtime.liveHandle()))
    }

    public var lastUIOutcome: UIOutcome {
        get throws {
            var abi = bdk_stub_ui_outcome_t()
            abi.struct_size = MemoryLayout<bdk_stub_ui_outcome_t>.size
            try check(bdk_stub_last_ui_outcome(try runtime.liveHandle(), &abi))
            let kinds: [UInt32: UIOutcomeKind] = [
                BDK_STUB_UI_NONE: .none, BDK_STUB_UI_COMPLETED: .completed,
                BDK_STUB_UI_DEFAULTED: .defaulted, BDK_STUB_UI_REJECTED: .rejected,
                BDK_STUB_UI_CANCELLED: .cancelled, BDK_STUB_UI_WITHDRAWN: .withdrawn,
                BDK_STUB_UI_DISABLED: .disabled,
            ]
            return UIOutcome(
                kind: kinds[abi.kind] ?? .none, surface: abi.surface,
                completion: Self.text(abi.completion), argumentsJSON: Self.text(abi.arguments_json))
        }
    }

    /// Delivers a key event to the page's keyboard handler, as the engine does. Returns whether the
    /// reserve callback kept the key for the application.
    @discardableResult
    public func simulateKey(on page: BDKWebContents, event: NSEvent) throws -> Bool {
        var reserved: UInt8 = 0
        let native = bdk_native_handle_t(
            kind: BDK_NATIVE_NSEVENT.rawValue,
            pointer: Unmanaged.passUnretained(event).toOpaque(), id: UInt64(event.timestamp))
        try check(bdk_stub_simulate_key(page.handle, native, &reserved))
        return reserved != 0
    }

    // MARK: Services

    /// How the stub's operating system answers an authentication request.
    public enum Authentication: UInt32, Sendable {
        /// The user cancels the prompt: the call fails with `capability.reauthenticationFailed`.
        case refuse = 0
        case accept = 1
        /// The prompt stays open until `answerAuthentication(granted:)`.
        case wait = 2
    }

    /// The counters of `serviceCount`.
    public enum ServiceCount: UInt32, Sendable {
        /// Calls that reached a service.
        case executed = 0
        /// Calls a service is holding.
        case held = 1
        /// Calls a service was told to stop.
        case stopped = 2
        /// Times the operating system was asked to authenticate.
        case authentications = 3
    }

    /// How the operating system answers the next authentication requests. The default is `.refuse`.
    public func setAuthentication(_ mode: Authentication) throws {
        try check(bdk_stub_set_authentication(try runtime.liveHandle(), mode.rawValue))
    }

    /// Answers the authentication request that waits in `.wait` mode.
    public func answerAuthentication(granted: Bool) throws {
        try check(bdk_stub_answer_authentication(try runtime.liveHandle(), granted ? 1 : 0))
    }

    /// While held, the stub's services take every built-in command and never answer it, so a test
    /// can cancel it.
    public func holdServices(_ hold: Bool) throws {
        try check(bdk_stub_hold_services(try runtime.liveHandle(), hold ? 1 : 0))
    }

    /// Answers every held command, late.
    public func releaseHeldCommands() throws {
        try check(bdk_stub_release_held_commands(try runtime.liveHandle()))
    }

    public func serviceCount(_ which: ServiceCount) throws -> UInt32 {
        bdk_stub_service_count(try runtime.liveHandle(), which.rawValue)
    }

    // MARK: Helpers

    private func check(_ status: bdk_status_t) throws {
        guard status != BDK_OK else { return }
        throw BDKError(
            domain: .runtime, code: "stubRejected",
            message: "the stub rejected the call with status \(status.rawValue)")
    }

    private static func text<Chars>(_ tuple: Chars) -> String {
        withUnsafeBytes(of: tuple) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
    }
}
