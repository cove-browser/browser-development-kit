import CBDK
import Foundation

/// What an external tool presents when it connects: the token and any description of itself.
public struct BDKAutomationHello: Sendable, Equatable {
    /// Whatever the client presented. The application decides who it is.
    public let token: String
    /// The client's free-form description of itself, or `.null`.
    public let client: BDKJSValue
}

/// How the application answers a hello.
public enum BDKAutomationHelloAnswer: Sendable {
    /// Binds the connection to a live session, which the connection then calls commands as.
    case accept(session: BDKAutomationSession)
    /// Closes the connection with `message`.
    case refuse(message: String = "the application refused the connection")
}

/// The external endpoint: a user-restricted Unix-domain socket that tools outside the application
/// connect to, off unless `[automation.external] enabled` is true. Nothing listens on a network
/// port. `runtime.automation.external`.
@MainActor
public final class BDKAutomationExternal {
    private unowned let runtime: BDKRuntime
    private var current: BDKAutomationEndpoint?

    init(runtime: BDKRuntime) {
        self.runtime = runtime
    }

    /// Opens the endpoint. `authenticate` runs on the main actor for every connection's hello and
    /// may take its time: the connection waits for the answer. It throws to refuse. At most one
    /// endpoint is open per runtime. Throws `automation.disabled` unless
    /// `[automation.external] enabled` is true.
    ///
    /// `directory` must be a directory of the user's that no one else may write; the socket goes
    /// in a new 0700 directory inside it. The default is the temporary directory.
    public func open(
        directory: URL? = nil,
        authenticate:
            @escaping @MainActor (BDKAutomationHello) async throws ->
            BDKAutomationHelloAnswer
    ) throws -> BDKAutomationEndpoint {
        let handle = try runtime.liveHandle()
        let options =
            directory.map { BDKJSValue.object(["directory": .string($0.path)]) } ?? .object([:])
        let box = Unmanaged.passRetained(HelloBox(authenticate))
        var buffer: bdk_buffer_t = 0
        do {
            try withBDKString(options.jsonString) { lent in
                try abiCall(.runtime) {
                    bdk_automation_endpoint_open(
                        handle, lent, helloCallback, box.toOpaque(), helloReleaseCallback, &buffer,
                        $0)
                }
            }
        } catch {
            // A refused open leaves the context with the caller.
            box.release()
            throw error
        }
        let endpoint = BDKAutomationEndpoint(
            external: self, socketPath: String(decoding: takeBuffer(buffer), as: UTF8.self))
        current = endpoint
        return endpoint
    }

    func close(_ endpoint: BDKAutomationEndpoint) {
        guard current === endpoint, let handle = try? runtime.liveHandle() else { return }
        var error: bdk_error_t = 0
        _ = bdk_automation_endpoint_close(handle, &error)
        if error != 0 { bdk_error_release(error) }
        EventLoop.kick()
        current = nil
    }

    func invalidate() {
        current?.markClosed()
        current = nil
    }
}

/// The open endpoint of `BDKAutomationExternal.open`.
@MainActor
public final class BDKAutomationEndpoint {
    /// The socket tools connect to. Hand it to the application's own CLI or gateway.
    public let socketPath: String
    public private(set) var isOpen = true

    private unowned let external: BDKAutomationExternal

    init(external: BDKAutomationExternal, socketPath: String) {
        self.external = external
        self.socketPath = socketPath
    }

    /// Closes every connection and removes the socket. Does nothing when it is closed.
    public func close() {
        guard isOpen else { return }
        isOpen = false
        external.close(self)
    }

    func markClosed() {
        isOpen = false
    }
}

// MARK: - The hello callback

@MainActor
private final class HelloBox {
    let authenticate: @MainActor (BDKAutomationHello) async throws -> BDKAutomationHelloAnswer

    init(
        _ authenticate:
            @escaping @MainActor (BDKAutomationHello) async throws ->
            BDKAutomationHelloAnswer
    ) {
        self.authenticate = authenticate
    }

    func receive(hello: bdk_context_t, json: String) {
        let fields = BDKJSValue(jsonString: json)?.objectValue ?? [:]
        let request = BDKAutomationHello(
            token: fields["token"]?.stringValue ?? "", client: fields["client"] ?? .null)
        let authenticate = authenticate
        Task { @MainActor in
            let answer: BDKAutomationHelloAnswer
            do {
                answer = try await authenticate(request)
            } catch {
                answer = .refuse(message: "\(error)")
            }
            Self.answer(hello, with: answer)
        }
    }

    /// Answers the hello. The connection may be gone by now, and a session may have been revoked
    /// meanwhile; neither is the application's error.
    private static func answer(_ hello: bdk_context_t, with answer: BDKAutomationHelloAnswer) {
        var error: bdk_error_t = 0
        switch answer {
        case .accept(let session):
            if let handle = try? session.liveHandle() {
                _ = bdk_automation_hello_accept(hello, handle, &error)
            } else {
                refuse(hello, "the session was revoked", &error)
            }
        case .refuse(let message):
            refuse(hello, message, &error)
        }
        if error != 0 { bdk_error_release(error) }
        EventLoop.kick()
    }

    private static func refuse(
        _ hello: bdk_context_t, _ message: String, _ error: inout bdk_error_t
    ) {
        _ = withBDKString(message) { bdk_automation_hello_refuse(hello, $0, &error) }
    }
}

private let helloCallback: bdk_automation_hello_cb = { context, hello, json in
    guard let context else { return }
    let box = Unmanaged<HelloBox>.fromOpaque(context).takeUnretainedValue()
    let text = swiftString(json)
    MainActor.assumeIsolated { box.receive(hello: hello, json: text) }
}

private let helloReleaseCallback: bdk_release_cb = { context in
    guard let context else { return }
    Unmanaged<HelloBox>.fromOpaque(context).release()
}
