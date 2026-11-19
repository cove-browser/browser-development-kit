import Foundation
import Network

/// A tiny HTTP/1.1 server on 127.0.0.1 for the tests that check what a profile keeps: it sets a cookie,
/// serves a cacheable image and counts how often the image is fetched. Requests are read up to the end of
/// the headers; every response closes the connection.
final class LocalServer: @unchecked Sendable {
    private let listener: NWListener
    private let queue = DispatchQueue(label: "minimal-browser-e2e.server")
    private var imageFetches = 0
    private(set) var port: UInt16 = 0

    init() throws {
        let parameters = NWParameters.tcp
        parameters.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: .any)
        listener = try NWListener(using: parameters)
        listener.newConnectionHandler = { [weak self] connection in self?.serve(connection) }
        let ready = DispatchSemaphore(value: 0)
        listener.stateUpdateHandler = { state in
            if case .ready = state { ready.signal() }
        }
        listener.start(queue: queue)
        guard ready.wait(timeout: .now() + 5) == .success, let port = listener.port?.rawValue else {
            throw AppRun.Failure(description: "the local test server did not start")
        }
        self.port = port
    }

    deinit { listener.cancel() }

    func url(_ path: String) -> URL {
        URL(string: "http://127.0.0.1:\(port)\(path)")!
    }

    /// How many times /cached.png was fetched from the server (not from a cache).
    var cachedImageFetches: Int {
        queue.sync { imageFetches }
    }

    private func serve(_ connection: NWConnection) {
        connection.start(queue: queue)
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65536) {
            [weak self] data, _, _, _ in
            guard let self, let data, let request = String(data: data, encoding: .utf8) else {
                connection.cancel()
                return
            }
            let path = request.split(separator: " ").dropFirst().first.map(String.init) ?? "/"
            connection.send(
                content: self.response(for: path), isComplete: true,
                completion: .contentProcessed { _ in connection.cancel() })
        }
    }

    /// 1x1 transparent PNG.
    private static let png = Data(
        base64Encoded:
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII="
    )!

    private func response(for path: String) -> Data {
        switch path {
        case "/set":
            // The cookie comes with the page; storage and the image are written once it has loaded.
            return Self.html(
                """
                <title>set</title><img src="/cached.png" onload="localStorage.setItem('bdk','1');\
                document.title='set-done'">
                """, headers: ["Set-Cookie: bdk=1; Path=/"])
        case "/get":
            return Self.html(
                """
                <title>get</title><img src="/cached.png" onload="document.title=\
                (document.cookie||'no-cookie')+'|'+(localStorage.getItem('bdk')||'no-storage')">
                """)
        case "/cached.png":
            imageFetches += 1
            return Self.message(
                status: "200 OK",
                headers: ["Content-Type: image/png", "Cache-Control: public, max-age=3600"],
                body: Self.png)
        default:
            return Self.message(status: "404 Not Found", headers: [], body: Data())
        }
    }

    private static func html(_ body: String, headers: [String] = []) -> Data {
        message(
            status: "200 OK",
            headers: ["Content-Type: text/html; charset=utf-8", "Cache-Control: no-store"]
                + headers,
            body: Data("<!doctype html>\(body)".utf8))
    }

    private static func message(status: String, headers: [String], body: Data) -> Data {
        let head =
            (["HTTP/1.1 \(status)", "Content-Length: \(body.count)", "Connection: close"]
            + headers).joined(separator: "\r\n") + "\r\n\r\n"
        return Data(head.utf8) + body
    }
}
