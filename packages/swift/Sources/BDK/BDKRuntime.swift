import AppKit
import CBDK
import Foundation

/// The runtime: one per process. Start it once, create profiles and pages from it, shut it down.
@MainActor
public final class BDKRuntime {
    public struct Info: Sendable, Equatable {
        /// What the runtime reports. The Swift layer has already checked both against its headers.
        public let abiVersion: UInt32
        public let abiHash: String
    }

    /// Who created a page the application did not create itself.
    public enum Creator: Sendable, Equatable {
        /// The application. Not emitted: a page the application creates is already in its hands.
        case application
        /// An automation session, by its name (a session reached through the external endpoint has no
        /// `BDKAutomationSession` object in the application).
        case automation(session: String)
        /// A browser extension, by id: a page it opened (its options page, a link in its popup), when
        /// the new-window request reaches a handler.
        case `extension`(String)
    }

    public enum Event: Sendable, Equatable {
        case registrationsFrozen
        case profileOpened(BDKProfile.ID)
        case profileClosed(BDKProfile.ID)
        case shutdownStarted
        /// A page someone else created: by an automation session, which also arrives as
        /// `runtime.automation.events` `.contentsCreated` and is kept by the runtime until it closes;
        /// or by an extension, which also arrives as the `NewWindowRequest` that places it. It is not
        /// on screen until the application puts it there.
        case webContentsCreated(BDKWebContents, by: Creator)
        /// A kind this version does not know, from a newer runtime.
        case unknown(kind: UInt32, detailJSON: String)
    }

    enum State {
        case running
        case shuttingDown
        case stopped
    }

    private static var current: BDKRuntime?

    public static var isRunning: Bool {
        current != nil
    }

    /// The running runtime. Traps before `start`.
    public static var shared: BDKRuntime {
        guard let current else {
            preconditionFailure("BDKRuntime.shared was used before BDKRuntime.start succeeded")
        }
        return current
    }

    public let info: Info
    public private(set) lazy var profiles = BDKProfiles(runtime: self)
    /// The UI-request handlers the runtime delivers to.
    public private(set) lazy var ui = BDKUIHandlers()
    /// Application commands and events, and invocation of any command by name.
    public private(set) lazy var commands = BDKCommands(runtime: self)
    /// The application windows registered with the runtime.
    public private(set) lazy var windows = BDKWindows(runtime: self)
    /// Automation sessions, tasks and the external endpoint (swift-api.md section 13).
    public private(set) lazy var automation = BDKAutomation(runtime: self)
    public private(set) var areRegistrationsFrozen = false

    package private(set) var handle: bdk_runtime_t
    private(set) var state = State.running
    /// The shutdown in progress, which every later caller waits for.
    private var shutdownTask: Task<Void, Never>?
    private var liveContents: [UInt64: WeakBox<BDKWebContents>] = [:]
    private let broadcaster = EventBroadcaster<Event>()

    public var events: BDKEventSequence<Event> {
        BDKEventSequence(broadcaster)
    }

    /// Delivers an event the Swift layer raises itself (one with no ABI runtime event behind it).
    func publish(_ event: Event) {
        broadcaster.send(event)
    }

    private init(handle: bdk_runtime_t, info: Info) {
        self.handle = handle
        self.info = info
    }

    // MARK: Start

    /// Checks the ABI, starts the runtime and freezes the registrations. It either succeeds or
    /// throws with nothing left running. It can be called once per process.
    @discardableResult
    public static func start(_ configuration: BDKConfiguration) throws -> BDKRuntime {
        try startRuntime(configuration)
    }

    /// `start` with the ABI the host claims to have been built against, and the choice of leaving
    /// the registrations open. Tests use both.
    @discardableResult
    package static func startRuntime(
        _ configuration: BDKConfiguration, host: HostABI = .current,
        freezingRegistrations: Bool = true
    ) throws -> BDKRuntime {
        try configuration.commands.validate()
        try configuration.settings.validate()
        var handle: bdk_runtime_t = 0
        var error: bdk_error_t = 0
        let strings = [
            configuration.resolvedJSON,
            configuration.bundleURL?.path ?? "",
            configuration.launch.userDataDirectory?.path ?? "",
            host.hash,
            configuration.services.google.apiKey ?? "",
        ]
        let status = withBDKStrings(strings) { lent in
            var config = bdk_start_config_t()
            config.struct_size = MemoryLayout<bdk_start_config_t>.size
            config.resolved_config_json = lent[0]
            config.bundle_path = lent[1]
            config.user_data_path_override = lent[2]
            config.host_abi_hash = lent[3]
            config.google_api_key = lent[4]
            config.host_abi_version = host.version
            if let application: NSApplication = NSApp {
                config.native_app = bdk_native_handle_t(
                    kind: BDK_NATIVE_NSAPPLICATION.rawValue,
                    pointer: Unmanaged.passUnretained(application).toOpaque(), id: 0)
            }
            return bdk_runtime_start(&config, &handle, &error)
        }
        do {
            try BDKError.check(status, error: error, subject: .runtime)
        } catch let failure as BDKError where failure.domain == .abi {
            throw mismatch(failure, host: host)
        }
        let runtime = BDKRuntime(
            handle: handle,
            info: Info(
                abiVersion: bdk_abi_version(), abiHash: swiftString(bdk_abi_hash())))
        current = runtime
        runtime.subscribeToEvents()
        do {
            // The core checks some registrations only now (a custom UI surface needs its handler
            // by the freeze), after the native runtime has started.
            try runtime.register(configuration)
            if freezingRegistrations { try runtime.freezeRegistrations() }
        } catch {
            runtime.stopAfterFailedStart()
            throw error
        }
        EventLoop.kick()
        return runtime
    }

    /// Keeps the promise of `start` when it fails after the native runtime started: the runtime is
    /// no longer current at once, and it shuts down on the main actor, as `shutdown()` does.
    private func stopAfterFailedStart() {
        if Self.current === self { Self.current = nil }
        terminate(completion: {})
        EventLoop.kick()
    }

    /// An ABI error that names what the host and the runtime each have, so that the fix is clear.
    private static func mismatch(_ failure: BDKError, host: HostABI) -> BDKError {
        let runtimeVersion = bdk_abi_version()
        let runtimeHash = swiftString(bdk_abi_hash())
        var details = failure.details
        details["hostVersion"] = .number(Double(host.version))
        details["hostHash"] = .string(host.hash)
        details["runtimeVersion"] = .number(Double(runtimeVersion))
        details["runtimeHash"] = .string(runtimeHash)
        return BDKError(
            domain: failure.domain, code: failure.code,
            message: "the application was built against BDK ABI version \(host.version) "
                + "(hash \(host.hash)) but the runtime implements version \(runtimeVersion) "
                + "(hash \(runtimeHash)): \(failure.message). Rebuild the application with the "
                + "BDK SDK that matches the runtime.",
            details: details)
    }

    // MARK: Registrations

    /// Hands the configuration's UI handlers, page routes, commands, events, settings and providers to
    /// the runtime. All are registrations, so this runs before the freeze.
    private func register(_ configuration: BDKConfiguration) throws {
        try ui.attach(to: self, from: configuration)
        try configuration.pages.adopt(into: self)
        try commands.adopt(configuration.commands)
        try configuration.settings.adopt(into: self)
        try configuration.services.adopt(into: self)
    }

    /// Freezes the route and capability tables. Registrations after this fail with
    /// `config.registrationAfterStart`, and a second freeze does too.
    package func freezeRegistrations() throws {
        try abiCall(.runtime) { bdk_runtime_freeze_registrations(handle, $0) }
        areRegistrationsFrozen = true
    }

    // MARK: Events

    private func subscribeToEvents() {
        let sink = Unmanaged.passRetained(RuntimeEventSink(broadcaster))
        _ = bdk_runtime_subscribe(
            handle,
            { context, kind, detail in
                guard let context else { return }
                let sink = Unmanaged<RuntimeEventSink>.fromOpaque(context).takeUnretainedValue()
                sink.broadcaster.send(Event(kind: kind, detailJSON: swiftString(detail)))
            }, sink.toOpaque(),
            { context in
                guard let context else { return }
                Unmanaged<RuntimeEventSink>.fromOpaque(context).release()
            })
    }

    // MARK: Shutdown

    /// Shuts the runtime down. Every profile and page is stale as soon as this starts, a pending
    /// call completes with `broker.cancelled`, and event sequences end. Chromium cannot be
    /// restarted in the same process. A call made while a shutdown is in progress returns when that
    /// shutdown has finished.
    public func shutdown() async {
        if let shutdownTask { return await shutdownTask.value }
        guard state == .running else { return }
        state = .shuttingDown
        invalidateChildren()
        let handle = handle
        // Not cancelled with the caller: once started, the runtime stops either way.
        let task = Task { @MainActor in
            // The shutdown result is "{}" or a rejection because the runtime is already gone;
            // either way the runtime is stopped afterwards, so there is nothing to report.
            _ = try? await abiAsync(.runtime) { context, result, release in
                bdk_runtime_shutdown(handle, result, context, release)
            }
            self.state = .stopped
            self.handle = 0
            if Self.current === self { Self.current = nil }
            self.broadcaster.finish()
            EventLoop.kick()
        }
        shutdownTask = task
        await task.value
    }

    /// For `applicationShouldTerminate`: cancels this attempt, shuts the runtime down, then asks the
    /// application to terminate again; the second call returns `.terminateNow`.
    ///
    /// It does not use `.terminateLater`: AppKit then waits for the reply in a modal run loop nested inside
    /// Chromium's, and Chromium cannot stop until that loop returns. `applicationShouldTerminate` therefore
    /// runs twice; work that must happen once should check `BDKRuntime.isRunning`.
    public func terminate(replyingTo application: NSApplication) -> NSApplication.TerminateReply {
        terminateReply { application.terminate(nil) }
    }

    /// `.terminateNow` only once the runtime has stopped. Before that, including while a shutdown
    /// is in progress, it cancels this attempt and calls `requestTermination` after the shutdown.
    package func terminateReply(
        _ requestTermination: @escaping @MainActor () -> Void
    ) -> NSApplication.TerminateReply {
        guard state != .stopped else { return .terminateNow }
        terminate(completion: requestTermination)
        return .terminateCancel
    }

    package func terminate(completion: @escaping @MainActor () -> Void) {
        Task {
            await shutdown()
            completion()
        }
    }

    // MARK: Children

    package func liveHandle() throws -> bdk_runtime_t {
        guard state == .running, handle != 0 else { throw BDKError.stale(.runtime) }
        return handle
    }

    func register(_ contents: BDKWebContents) {
        liveContents[contents.id.rawValue] = WeakBox(contents)
    }

    func unregister(_ contents: BDKWebContents) {
        liveContents[contents.id.rawValue] = nil
    }

    /// Every live page, in creation order of their handles.
    public var webContents: [BDKWebContents] {
        liveContents.keys.sorted().compactMap { liveContents[$0]?.value }
    }

    /// The page with this handle, if the application has one for it.
    func webContents(handle: UInt64) -> BDKWebContents? {
        handle == 0 ? nil : liveContents[handle]?.value
    }

    private func invalidateChildren() {
        automation.invalidate()
        commands.invalidate()
        for contents in webContents { contents.invalidate() }
        for profile in profiles.loaded { profile.invalidate() }
        windows.removeAll()
    }
}

extension BDKRuntime.Event {
    /// Pages compare by identity.
    public static func == (lhs: Self, rhs: Self) -> Bool {
        switch (lhs, rhs) {
        case (.registrationsFrozen, .registrationsFrozen), (.shutdownStarted, .shutdownStarted):
            true
        case (.profileOpened(let left), .profileOpened(let right)),
            (.profileClosed(let left), .profileClosed(let right)):
            left == right
        case (.webContentsCreated(let left, let by), .webContentsCreated(let right, let other)):
            left === right && by == other
        case (.unknown(let kind, let detail), .unknown(let otherKind, let otherDetail)):
            kind == otherKind && detail == otherDetail
        default:
            false
        }
    }

    fileprivate init(kind: UInt32, detailJSON: String) {
        let identifier: BDKProfile.ID? = {
            guard case .object(let object) = BDKJSValue(jsonString: detailJSON),
                case .string(let value) = object["id"]
            else { return nil }
            return BDKProfile.ID(rawValue: value)
        }()
        switch (kind, identifier) {
        case (BDK_RUNTIME_EVENT_REGISTRATIONS_FROZEN, _): self = .registrationsFrozen
        case (BDK_RUNTIME_EVENT_PROFILE_OPENED, let identifier?): self = .profileOpened(identifier)
        case (BDK_RUNTIME_EVENT_PROFILE_CLOSED, let identifier?): self = .profileClosed(identifier)
        case (BDK_RUNTIME_EVENT_SHUTDOWN_STARTED, _): self = .shutdownStarted
        default: self = .unknown(kind: kind, detailJSON: detailJSON)
        }
    }
}

/// What the runtime-event callback needs, and nothing that could keep the runtime alive.
private final class RuntimeEventSink: Sendable {
    let broadcaster: EventBroadcaster<BDKRuntime.Event>

    init(_ broadcaster: EventBroadcaster<BDKRuntime.Event>) {
        self.broadcaster = broadcaster
    }
}

final class WeakBox<Object: AnyObject> {
    weak var value: Object?

    init(_ value: Object) {
        self.value = value
    }
}
