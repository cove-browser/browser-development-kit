import CBDK
import Foundation

/// Automation: sessions that run bundled scripts or drive pages, the audit events, and the external
/// endpoint (swift-api.md section 13). `runtime.automation`.
///
/// The runtime keeps every session until it is revoked and every page a session created until it
/// closes, so an application does not have to hold them.
@MainActor
public final class BDKAutomation {
    /// What a session may see: the profiles, and which pages within them.
    public struct Scope {
        /// The profiles the session works in; nil is every profile.
        public var profiles: [BDKProfile]?
        public var contents: ContentsScope

        public init(profiles: [BDKProfile]? = nil, contents: ContentsScope = .created) {
            self.profiles = profiles
            self.contents = contents
        }
    }

    /// Which pages a session may address.
    public enum ContentsScope {
        case all
        /// Only pages the session creates. The default.
        case created
        /// Only the pages named here, and those `BDKAutomationSession.grant(contents:)` adds.
        case explicit([BDKWebContents])
    }

    public struct SessionOptions {
        public var name: String
        /// Capabilities from the broker's table; a wildcard only as `app.<prefix>.*`.
        public var capabilities: [String]
        public var scope: Scope

        public init(name: String, capabilities: [String] = [], scope: Scope = Scope()) {
            self.name = name
            self.capabilities = capabilities
            self.scope = scope
        }
    }

    /// One command a session invoked, reported after it ended (refused calls included).
    public struct CommandRecord: Sendable, Equatable, Decodable {
        public let session: String
        public let task: String?
        public let command: String
        public let profile: String?
        public let contents: String?
        public let error: BDKCallError?
    }

    public enum Event: Sendable {
        case sessionCreated(BDKSessionInfo)
        case sessionRevoked(BDKSessionInfo)
        case commandInvoked(CommandRecord)
        /// A session created this page. The runtime keeps it alive until it closes; the page is not
        /// on screen, and the application decides whether to show it.
        case contentsCreated(BDKWebContents, bySession: String)
    }

    /// External tools, off unless `[automation.external] enabled` is true.
    public private(set) lazy var external = BDKAutomationExternal(runtime: runtime)

    private unowned let runtime: BDKRuntime
    private var sessions: [UInt64: BDKAutomationSession] = [:]
    private var pages: [UInt64: BDKWebContents] = [:]
    private let broadcaster = EventBroadcaster<Event>()
    private var watcher: Task<Void, Never>?

    init(runtime: BDKRuntime) {
        self.runtime = runtime
    }

    /// Session, command and page events. Each iterator sees every event from when it was created.
    public var events: BDKEventSequence<Event> {
        watch()
        return BDKEventSequence(broadcaster)
    }

    /// Creates a session. Throws `capability.denied` for a capability that may not be granted to a
    /// session (`passwords.reveal`, a wildcard outside `app.<prefix>.*`), `profile.notFound` for a
    /// profile that is not open and `broker.invalidArguments` for a bad shape.
    public func makeSession(_ options: SessionOptions) throws -> BDKAutomationSession {
        let runtimeHandle = try runtime.liveHandle()
        watch()
        var profiles: BDKJSValue = .string("all")
        if let listed = options.scope.profiles {
            profiles = .array(
                try listed.map { profile in
                    _ = try profile.liveHandle()
                    return .string(profile.id.rawValue)
                })
        }
        var granted: [BDKWebContents] = []
        var contents = "all"
        switch options.scope.contents {
        case .all:
            break
        case .created:
            contents = "created"
        case .explicit(let pages):
            contents = "explicit"
            granted = pages
        }
        let json = BDKJSValue.object([
            "name": .string(options.name),
            "capabilities": .array(options.capabilities.map { .string($0) }),
            "scope": .object(["profiles": profiles, "contents": .string(contents)]),
        ]).jsonString
        var handle: bdk_session_t = 0
        try withBDKString(json) { lent in
            try abiCall(.session) { bdk_session_create(runtimeHandle, lent, &handle, $0) }
        }
        let session = BDKAutomationSession(
            automation: self, handle: handle, name: options.name,
            capabilities: options.capabilities)
        sessions[handle] = session
        do {
            for page in granted { try session.grant(contents: page) }
        } catch {
            session.revoke()
            throw error
        }
        return session
    }

    func forget(_ session: BDKAutomationSession, handle: bdk_session_t) {
        sessions[handle] = nil
    }

    /// Ends the sessions and the streams after the runtime released its objects.
    func invalidate() {
        watcher?.cancel()
        watcher = nil
        for session in sessions.values { session.invalidate() }
        sessions.removeAll()
        pages.removeAll()
        external.invalidate()
        broadcaster.finish()
    }

    // MARK: Events

    /// Subscribes to the audit events and to `contents.created`, once.
    private func watch() {
        guard watcher == nil, runtime.state == .running else { return }
        let names = [
            "automation.sessionCreated", "automation.sessionRevoked",
            "automation.commandInvoked", "contents.created",
        ]
        let sources = names.compactMap { try? runtime.commands.events(named: $0) }
        watcher = Task { [weak self] in
            await withTaskGroup(of: Void.self) { group in
                for source in sources {
                    group.addTask {
                        for await delivered in source { await self?.handle(delivered) }
                    }
                }
            }
        }
    }

    private func handle(_ delivered: BDKApplicationEvent) {
        struct Session: Decodable {
            var session: BDKSessionInfo
        }
        struct Created: Decodable {
            var contents: BDKContentsInfo
            var createdBy: BDKCreator
        }
        let json = delivered.payloadJSON
        switch delivered.name {
        case "automation.sessionCreated":
            if let event = try? BDKServiceCoding.decode(Session.self, from: json) {
                broadcaster.send(.sessionCreated(event.session))
            }
        case "automation.sessionRevoked":
            if let event = try? BDKServiceCoding.decode(Session.self, from: json) {
                broadcaster.send(.sessionRevoked(event.session))
            }
        case "automation.commandInvoked":
            if let event = try? BDKServiceCoding.decode(CommandRecord.self, from: json) {
                broadcaster.send(.commandInvoked(event))
            }
        case "contents.created":
            if let event = try? BDKServiceCoding.decode(Created.self, from: json),
                event.createdBy.kind == .session, let name = event.createdBy.session
            {
                adopt(event.contents, createdBy: name)
            }
        default:
            break
        }
    }

    /// Wraps a page a session created and keeps it until it closes.
    private func adopt(_ info: BDKContentsInfo, createdBy session: String) {
        guard let handle = UInt64(info.id),
            let profile = runtime.profiles.loaded.first(where: { $0.id.rawValue == info.profile })
        else { return }
        let page = BDKWebContents.adopt(handle: handle, in: profile)
        pages[handle] = page
        // Subscribe before anything else runs, so the page's `closed` cannot be missed.
        let events = page.events
        Task { [weak self] in
            for await event in events {
                if case .closed = event { break }
            }
            // The page reported closed (or went away); the runtime's hold on it ends.
            page.destroy()
            self?.pages[handle] = nil
        }
        broadcaster.send(.contentsCreated(page, bySession: session))
        runtime.publish(.webContentsCreated(page, by: .automation(session: session)))
    }
}
