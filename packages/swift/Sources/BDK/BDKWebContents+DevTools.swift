import Foundation

/// DevTools for one page (swift-api.md section 14). Get it from `BDKWebContents.devTools`.
///
/// `present()` asks for the front end through the `devtools` surface. `attachProtocol()` opens a
/// session of the DevTools protocol on the page, in this process only: nothing listens on a port.
@MainActor
public final class DevTools {
    private unowned let contents: BDKWebContents
    private let scope: BDKServiceScope
    private var stateTask: Task<Void, Never>?

    /// Whether the front end is open. It starts false when this object is created, follows the
    /// `devtools.stateChanged` event (the user closing the window included) and is read from the
    /// engine again by `present()`, `close()` and `state()`.
    public private(set) var isOpen = false

    init(contents: BDKWebContents) {
        self.contents = contents
        scope = BDKServiceScope(runtime: contents.profile.runtime, profile: nil, contents: contents)
        watchState()
    }

    isolated deinit {
        stateTask?.cancel()
    }

    /// Asks for the DevTools front end, as the application's `devtools` handler or the default
    /// presenter shows it. Throws `devtools.disabled` when `[devtools] frontend` is "disabled".
    public func present() async throws {
        try await scope.run("devtools.open", BDKVoid())
        await refresh()
    }

    /// Closes the front end. Does nothing when it is not open.
    public func close() async throws {
        try await scope.run("devtools.close", BDKVoid())
        await refresh()
    }

    /// Reads the front end's state from the engine and updates `isOpen`.
    @discardableResult
    public func state() async throws -> BDKDevToolsState {
        let state = try await scope.call("devtools.state", BDKVoid(), as: BDKDevToolsState.self)
        isOpen = state.open
        return state
    }

    /// Opens a protocol session on the page. Throws `devtools.protocolDisabled` when
    /// `[devtools] protocol` is false. The session lasts until `detach()`, until the engine detaches
    /// it (the page closed, the front end took over) or until the runtime shuts down.
    public func attachProtocol() async throws -> BDKDevToolsProtocol {
        try await BDKDevToolsProtocol.attach(to: contents)
    }

    private func refresh() async {
        _ = try? await state()
    }

    private func watchState() {
        guard
            let source = try? contents.profile.runtime.commands.events(
                named: "devtools.stateChanged", contents: contents)
        else { return }
        let page = String(contents.id.rawValue)
        stateTask = Task { [weak self] in
            for await event in source {
                guard
                    case .object(let payload) = BDKJSValue(jsonString: event.payloadJSON),
                    payload["contents"]?.stringValue == page,
                    let isOpen = payload["open"]?.boolValue
                else { continue }
                self?.isOpen = isOpen
            }
        }
    }
}

/// A session of the DevTools protocol on one page (`DevTools.attachProtocol()`).
@MainActor
public final class BDKDevToolsProtocol {
    /// One protocol event of this session.
    public struct Event: Sendable, Equatable {
        /// For example "Network.requestWillBeSent".
        public let method: String
        public let params: BDKJSValue
    }

    /// The engine's identifier of the session.
    public let sessionID: String
    /// The events of this session; it ends at `detach()` or when the engine detaches the session.
    /// Each iterator sees every event from when it was created.
    public var events: BDKEventSequence<Event> {
        BDKEventSequence(broadcaster)
    }

    public private(set) var isDetached = false

    private let scope: BDKServiceScope
    private let broadcaster = EventBroadcaster<Event>()
    private var tasks: [Task<Void, Never>] = []

    private init(scope: BDKServiceScope, sessionID: String) {
        self.scope = scope
        self.sessionID = sessionID
    }

    static func attach(to contents: BDKWebContents) async throws -> BDKDevToolsProtocol {
        let runtime = contents.profile.runtime
        let scope = BDKServiceScope(runtime: runtime, profile: nil, contents: contents)
        // Subscribe first: events that come before `attach` answers wait in the streams.
        let events = try runtime.commands.events(named: "debug.event", contents: contents)
        let detached = try runtime.commands.events(named: "debug.detached", contents: contents)
        struct Output: Decodable {
            var session: String
        }
        let output = try await scope.call("debug.attach", BDKVoid(), as: Output.self)
        let session = BDKDevToolsProtocol(scope: scope, sessionID: output.session)
        session.forward(events: events, detached: detached)
        return session
    }

    /// Sends one protocol command and returns its result. The engine's error, such as
    /// `capability.denied` for a domain a session may not use, is thrown as it is.
    public func send(_ method: String, _ params: [String: BDKJSValue] = [:]) async throws
        -> BDKJSValue
    {
        struct Arguments: Encodable {
            var session: String
            var method: String
            var params: [String: BDKJSValue]
        }
        struct Output: Decodable {
            var result: BDKJSValue
        }
        guard !isDetached else { throw Self.detachedError }
        let arguments = Arguments(session: sessionID, method: method, params: params)
        return try await scope.call("debug.send", arguments, as: Output.self).result
    }

    /// Ends the session and its `events`. Does nothing when it already ended.
    public func detach() {
        guard !isDetached else { return }
        end()
        struct Arguments: Encodable {
            var session: String
        }
        let scope = scope
        let session = sessionID
        // A failure here means the engine had detached the session already.
        Task { try? await scope.run("debug.detach", Arguments(session: session)) }
    }

    private static var detachedError: BDKError {
        BDKError(
            domain: .devtools, code: "detached", message: "the protocol session was detached")
    }

    private func end() {
        isDetached = true
        for task in tasks { task.cancel() }
        tasks = []
        broadcaster.finish()
    }

    private func forward(
        events: AsyncStream<BDKApplicationEvent>, detached: AsyncStream<BDKApplicationEvent>
    ) {
        let session = sessionID
        let broadcaster = broadcaster
        tasks.append(
            Task {
                for await delivered in events {
                    guard
                        case .object(let payload) = BDKJSValue(jsonString: delivered.payloadJSON),
                        payload["session"]?.stringValue == session,
                        let method = payload["method"]?.stringValue
                    else { continue }
                    broadcaster.send(Event(method: method, params: payload["params"] ?? .null))
                }
                // The runtime shut down, or the session ended.
                broadcaster.finish()
            })
        tasks.append(
            Task { [weak self] in
                for await delivered in detached {
                    guard
                        case .object(let payload) = BDKJSValue(jsonString: delivered.payloadJSON),
                        payload["session"]?.stringValue == session
                    else { continue }
                    self?.end()
                    return
                }
            })
    }
}

extension BDKWebContents {
    /// The DevTools front end and the protocol.
    public var devTools: DevTools {
        if let existing = devToolsSession { return existing }
        let session = DevTools(contents: self)
        devToolsSession = session
        return session
    }
}
