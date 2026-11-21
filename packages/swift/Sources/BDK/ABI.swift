import CBDK
import Foundation

/// The ABI version and hash a host was built against. Both come from the headers.
package struct HostABI: Sendable, Equatable {
    package var version: UInt32
    package var hash: String

    package init(version: UInt32, hash: String) {
        self.version = version
        self.hash = hash
    }

    /// What this build of the Swift layer was compiled against.
    package static var current: HostABI {
        HostABI(version: UInt32(BDK_ABI_VERSION), hash: BDK_ABI_HASH)
    }
}

// MARK: - Strings and buffers

func swiftString(_ string: bdk_string_t) -> String {
    guard let data = string.data, string.length > 0 else { return "" }
    let bytes = UnsafeBufferPointer(
        start: UnsafeRawPointer(data).assumingMemoryBound(to: UInt8.self), count: string.length)
    return String(decoding: bytes, as: UTF8.self)
}

/// Lends `strings` to `body` as borrowed ABI strings, in the same order.
package func withBDKStrings<Result>(
    _ strings: [String], _ body: ([bdk_string_t]) throws -> Result
) rethrows -> Result {
    try withBDKStrings(strings[...], [], body)
}

package func withBDKString<Result>(
    _ string: String, _ body: (bdk_string_t) throws -> Result
) rethrows -> Result {
    try withBDKStrings([string]) { try body($0[0]) }
}

private func withBDKStrings<Result>(
    _ rest: ArraySlice<String>, _ lent: [bdk_string_t], _ body: ([bdk_string_t]) throws -> Result
) rethrows -> Result {
    guard let first = rest.first else { return try body(lent) }
    var copy = first
    return try copy.withUTF8 { buffer in
        let string = bdk_string_t(
            data: buffer.baseAddress.map {
                UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)
            },
            length: buffer.count)
        return try withBDKStrings(rest.dropFirst(), lent + [string], body)
    }
}

/// Copies a buffer's bytes and releases the buffer.
func takeBuffer(_ buffer: bdk_buffer_t) -> Data {
    let bytes = bdk_buffer_bytes(buffer)
    let data = bytes.data.map { Data(bytes: $0, count: bytes.length) } ?? Data()
    _ = bdk_release(buffer)
    return data
}

// MARK: - The event loop hook

/// Chromium pumps itself in the real runtime. The in-memory stub has no loop, so BDKTestSupport
/// installs a scheduler here that pumps it whenever work may be pending. Every ABI call made by
/// this package ends with `kick()`.
@MainActor
package enum EventLoop {
    package static var scheduler: (@MainActor () -> Void)?

    static func kick() {
        scheduler?()
    }
}

// MARK: - Synchronous calls

/// Runs a synchronous ABI call that has an error out-parameter and maps its status.
@MainActor
func abiCall(
    _ subject: BDKError.Subject, _ body: (UnsafeMutablePointer<bdk_error_t>) -> bdk_status_t
) throws {
    var error: bdk_error_t = 0
    let status = body(&error)
    EventLoop.kick()
    try BDKError.check(status, error: error, subject: subject)
}

// MARK: - Asynchronous calls

/// The context of one asynchronous call. The ABI retains it until the release callback, and the
/// continuation is resumed exactly once, by the result callback or by a synchronous rejection.
@MainActor
final class CallBox {
    var continuation: CheckedContinuation<String, any Error>?
    var call: bdk_call_t = 0
    var isFinished = false
    var isCancelRequested = false
    var progress: (@MainActor (Double, String) -> Void)?

    func finish(_ outcome: Result<String, BDKError>) {
        isFinished = true
        guard let continuation else { return }
        self.continuation = nil
        continuation.resume(with: outcome.mapError { $0 as any Error })
    }

    func cancel() {
        isCancelRequested = true
        guard call != 0, !isFinished else { return }
        _ = bdk_call_cancel(call)
        EventLoop.kick()
    }
}

private let resultCallback: bdk_result_cb = { context, error, json, _ in
    guard let context else { return }
    let box = Unmanaged<CallBox>.fromOpaque(context).takeUnretainedValue()
    let outcome: Result<String, BDKError>
    if error != 0 {
        outcome = .failure(BDKError(handle: error))
    } else {
        outcome = .success(swiftString(json))
    }
    MainActor.assumeIsolated { box.finish(outcome) }
}

private let progressCallback: bdk_progress_cb = { context, fraction, message in
    guard let context else { return }
    let box = Unmanaged<CallBox>.fromOpaque(context).takeUnretainedValue()
    let text = swiftString(message)
    MainActor.assumeIsolated { box.progress?(fraction, text) }
}

private let releaseCallback: bdk_release_cb = { context in
    guard let context else { return }
    Unmanaged<CallBox>.fromOpaque(context).release()
}

/// Issues an asynchronous ABI call and returns its result JSON.
///
/// `issue` receives the context and the two callbacks and returns the call handle, or 0 when the
/// call was rejected synchronously (nothing is called back then). Cancelling the Swift task calls
/// `bdk_call_cancel`.
@MainActor
func abiAsync(
    _ subject: BDKError.Subject,
    _ issue: (UnsafeMutableRawPointer, bdk_result_cb, bdk_release_cb) -> bdk_call_t
) async throws -> String {
    try await abiAsync(subject, progress: nil) { context, result, _, release in
        issue(context, result, release)
    }
}

/// `abiAsync` for a call that reports progress: `progress` runs on the main actor, before the
/// result, with the fraction and the message.
@MainActor
func abiAsync(
    _ subject: BDKError.Subject, progress: (@MainActor (Double, String) -> Void)?,
    _ issue: (UnsafeMutableRawPointer, bdk_result_cb, bdk_progress_cb, bdk_release_cb) -> bdk_call_t
) async throws -> String {
    let box = CallBox()
    box.progress = progress
    return try await withTaskCancellationHandler {
        try await withCheckedThrowingContinuation { continuation in
            box.continuation = continuation
            let context = Unmanaged.passRetained(box).toOpaque()
            let handle = issue(context, resultCallback, progressCallback, releaseCallback)
            EventLoop.kick()
            if handle == 0 {
                Unmanaged<CallBox>.fromOpaque(context).release()
                box.finish(.failure(.stale(subject)))
            } else {
                box.call = handle
                if box.isCancelRequested { box.cancel() }
            }
        }
    } onCancel: {
        Task { @MainActor in box.cancel() }
    }
}

/// Decodes the JSON result of a call.
func decodeResult<Value: Decodable>(_ type: Value.Type, from json: String) throws -> Value {
    do {
        return try JSONDecoder().decode(type, from: Data(json.utf8))
    } catch {
        throw BDKError(
            domain: .runtime, code: "malformedResult",
            message: "the runtime returned a result this version cannot read: \(error)")
    }
}
