import BDKTestSupport
import Darwin
import Foundation
import Testing

@testable import BDK

/// A tool outside the application: a Unix-domain socket client that speaks the wire protocol, one
/// frame being a 4-byte big-endian length and that many bytes of JSON.
@MainActor
private final class ExternalTool {
    private let descriptor: Int32
    private var pending = Data()

    init(connectingTo path: String) throws {
        descriptor = socket(AF_UNIX, SOCK_STREAM, 0)
        guard descriptor >= 0 else { throw POSIXError(.EIO) }
        var address = sockaddr_un()
        address.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8)
        guard bytes.count < MemoryLayout.size(ofValue: address.sun_path) else {
            throw POSIXError(.ENAMETOOLONG)
        }
        withUnsafeMutableBytes(of: &address.sun_path) { $0.copyBytes(from: bytes) }
        let status = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(descriptor, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard status == 0 else {
            close(descriptor)
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
        _ = fcntl(descriptor, F_SETFL, O_NONBLOCK)
    }

    deinit {
        close(descriptor)
    }

    func send(_ json: String) {
        let body = Array(json.utf8)
        let length = UInt32(body.count).bigEndian
        let frame = withUnsafeBytes(of: length) { Array($0) } + body
        _ = frame.withUnsafeBytes { write(descriptor, $0.baseAddress, frame.count) }
    }

    /// The next frame, or nil when the runtime closed the connection.
    func receive() async throws -> BDKJSValue? {
        let deadline = ContinuousClock.now + .seconds(5)
        while true {
            if pending.count >= 4 {
                let length = pending.prefix(4).reduce(0) { $0 << 8 | Int($1) }
                if pending.count >= 4 + length {
                    let body = pending.dropFirst(4).prefix(length)
                    pending = Data(pending.dropFirst(4 + length))
                    return BDKJSValue(jsonString: String(decoding: body, as: UTF8.self))
                }
            }
            var buffer = [UInt8](repeating: 0, count: 4096)
            let count = read(descriptor, &buffer, buffer.count)
            if count > 0 {
                pending.append(contentsOf: buffer.prefix(count))
            } else if count == 0 {
                return nil
            } else if errno == EAGAIN {
                if ContinuousClock.now > deadline { throw WaitTimedOut(what: "a frame") }
                try await Task.sleep(for: .milliseconds(5))
            } else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        }
    }
}

extension BDKStubTests {
    /// The external endpoint (T-AUT-04), with a real socket as the external tool.
    @MainActor
    @Suite("External automation")
    struct EndpointTests {
        @Test func theEndpointIsOffByDefault() async throws {
            try await withStubRuntime { runtime in
                await expectError(.automation, "disabled") {
                    _ = try runtime.automation.external.open { _ in .refuse() }
                }
            }
        }

        @Test func anAcceptedToolCallsCommandsAsItsSession() async throws {
            try await withStubRuntime(automationConfiguration(external: true)) { runtime in
                var hellos: [BDKAutomationHello] = []
                let endpoint = try runtime.automation.external.open { hello in
                    hellos.append(hello)
                    return .accept(
                        session: try runtime.automation.makeSession(
                            .init(name: "cli", capabilities: ["runtime.read"])))
                }
                #expect(endpoint.isOpen)
                var status = stat()
                #expect(stat(endpoint.socketPath, &status) == 0)
                #expect(status.st_mode & 0o777 == 0o600)

                let tool = try ExternalTool(connectingTo: endpoint.socketPath)
                tool.send(
                    #"{"type":"hello","protocol":1,"token":"good","client":{"n":1}}"#)
                let welcome = try #require(try await tool.receive())
                #expect(welcome["type"] == .string("welcome"))
                #expect(welcome["session", "name"] == .string("cli"))
                #expect(hellos.count == 1)
                #expect(hellos[0].token == "good")
                #expect(hellos[0].client == .object(["n": .number(1)]))

                tool.send(#"{"type":"call","id":1,"command":"runtime.caller","args":{}}"#)
                let result = try #require(try await tool.receive())
                #expect(result["type"] == .string("result") && result["id"] == .number(1))
                #expect(result["value", "kind"] == .string("session"))
                tool.send(#"{"type":"call","id":2,"command":"history.search","args":{}}"#)
                let refused = try #require(try await tool.receive())
                #expect(refused["error", "code"] == .string("denied"))

                endpoint.close()
                #expect(!endpoint.isOpen)
                #expect(stat(endpoint.socketPath, &status) != 0)
            }
        }

        @Test func aRefusedToolIsClosedWithTheMessage() async throws {
            try await withStubRuntime(automationConfiguration(external: true)) { runtime in
                let endpoint = try runtime.automation.external.open { hello in
                    hello.token == "good" ? .refuse(message: "unknown agent") : .refuse()
                }
                let tool = try ExternalTool(connectingTo: endpoint.socketPath)
                tool.send(#"{"type":"hello","protocol":1,"token":"wrong"}"#)
                let closed = try #require(try await tool.receive())
                #expect(closed["type"] == .string("closed"))
                #expect(closed["error", "code"] == .string("denied"))
                #expect(try await tool.receive() == nil)
            }
        }

        @Test func aThrowingAuthenticatorRefuses() async throws {
            struct Unknown: Error {}
            try await withStubRuntime(automationConfiguration(external: true)) { runtime in
                let endpoint = try runtime.automation.external.open { _ in throw Unknown() }
                let tool = try ExternalTool(connectingTo: endpoint.socketPath)
                tool.send(#"{"type":"hello","protocol":1,"token":"t"}"#)
                let closed = try #require(try await tool.receive())
                #expect(closed["type"] == .string("closed"))
                #expect(try await tool.receive() == nil)
            }
        }

        @Test func revokingTheSessionClosesTheConnection() async throws {
            try await withStubRuntime(automationConfiguration(external: true)) { runtime in
                let session = try runtime.automation.makeSession(.init(name: "cli"))
                let endpoint = try runtime.automation.external.open { _ in
                    .accept(session: session)
                }
                let tool = try ExternalTool(connectingTo: endpoint.socketPath)
                tool.send(#"{"type":"hello","protocol":1,"token":"t"}"#)
                _ = try await tool.receive()
                session.revoke()
                let closed = try #require(try await tool.receive())
                #expect(closed["error", "code"] == .string("sessionRevoked"))
                #expect(try await tool.receive() == nil)
            }
        }
    }
}
