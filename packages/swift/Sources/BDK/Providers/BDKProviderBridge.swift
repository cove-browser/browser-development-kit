import CBDK
import Foundation

/// The operations of a provider vtable, without `cancel`.
enum BDKProviderSlot: CaseIterable {
    case supportedPairs
    case prepare
    case translate
    case detect
    case removeModel
    case checkURL
    case checkDownload
    case update
    case start
}

/// One call to a provider, as an operation sees it: the answer is the operation's return value or
/// thrown error, and the call can report progress or positions before that.
struct BDKProviderCall: Sendable {
    let handle: bdk_context_t

    @MainActor
    func progress(_ fraction: Double) {
        bdk_provider_progress(handle, fraction)
        EventLoop.kick()
    }

    @MainActor
    func report(_ json: String) {
        withBDKString(json) { bdk_provider_report(handle, $0) }
        EventLoop.kick()
    }

    @MainActor
    func resolve(_ json: String) {
        withBDKString(json) { bdk_provider_resolve(handle, $0) }
        EventLoop.kick()
    }

    @MainActor
    func reject(_ error: BDKError) {
        withBDKStrings([BDKError.Domain.provider.rawValue, error.code, error.message]) {
            bdk_provider_reject(handle, $0[0], $0[1], $0[2])
        }
        EventLoop.kick()
    }
}

/// An application's provider as the core holds it: the operations the provider has, and the task
/// that runs each call until it answers or BDK cancels it.
@MainActor
final class BDKProviderBridge {
    /// Takes the call and the JSON input and returns the JSON output of the contract.
    typealias Operation = @MainActor @Sendable (BDKProviderCall, String) async throws -> String

    let operations: [BDKProviderSlot: Operation]
    private var tasks: [bdk_context_t: Task<Void, Never>] = [:]

    init(operations: [BDKProviderSlot: Operation]) {
        self.operations = operations
    }

    /// Registers `registration` for `service`, or throws with nothing retained.
    static func register(
        _ registration: BDKProviderRegistration, service: String, with handle: bdk_runtime_t
    ) throws {
        let box = Unmanaged.passRetained(BDKProviderBridge(operations: registration.operations))
        do {
            let descriptor = try BDKJSON.encode(registration.descriptor)
            try withBDKStrings([service, descriptor]) { lent in
                var vtable = bdk_provider_vtable_t()
                vtable.struct_size = MemoryLayout<bdk_provider_vtable_t>.size
                vtable.descriptor_json = lent[1]
                vtable.cancel = providerCancelCallback
                func callback(_ slot: BDKProviderSlot) -> bdk_provider_op_cb? {
                    registration.operations[slot] == nil ? nil : providerCallbacks[slot]
                }
                vtable.supported_pairs = callback(.supportedPairs)
                vtable.prepare = callback(.prepare)
                vtable.translate = callback(.translate)
                vtable.detect = callback(.detect)
                vtable.remove_model = callback(.removeModel)
                vtable.check_url = callback(.checkURL)
                vtable.check_download = callback(.checkDownload)
                vtable.update = callback(.update)
                vtable.start = callback(.start)
                try abiCall(.runtime) {
                    bdk_provider_register(
                        handle, lent[0], &vtable, box.toOpaque(), providerReleaseCallback, $0)
                }
            }
        } catch {
            // A refused registration leaves the context with the caller.
            box.release()
            throw error
        }
    }

    /// Runs the operation in a task on the main actor and answers the call when it ends. A
    /// `BDKError` in the provider domain keeps its code; any other error is
    /// `provider.remoteError`.
    func start(_ slot: BDKProviderSlot, call: bdk_context_t, input: String) {
        let handle = BDKProviderCall(handle: call)
        guard let operation = operations[slot] else {
            handle.reject(
                BDKError(
                    domain: .provider, code: "unavailable",
                    message: "the provider does not implement this operation"))
            return
        }
        tasks[call] = Task { @MainActor [weak self] in
            do {
                let output = try await operation(handle, input)
                self?.tasks[call] = nil
                handle.resolve(output)
            } catch let error as BDKError where error.domain == .provider {
                self?.tasks[call] = nil
                handle.reject(error)
            } catch {
                self?.tasks[call] = nil
                handle.reject(
                    BDKError(domain: .provider, code: "remoteError", message: "\(error)"))
            }
        }
    }

    /// BDK no longer wants the answer: ends the provider's work.
    func cancel(call: bdk_context_t) {
        tasks.removeValue(forKey: call)?.cancel()
    }

    func cancelAll() {
        let running = tasks.values
        tasks.removeAll()
        for task in running { task.cancel() }
    }
}

// MARK: - Callbacks

private func bridge(_ context: UnsafeMutableRawPointer?) -> BDKProviderBridge? {
    context.map { Unmanaged<BDKProviderBridge>.fromOpaque($0).takeUnretainedValue() }
}

private func run(
    _ slot: BDKProviderSlot, _ context: UnsafeMutableRawPointer?, _ call: bdk_context_t,
    _ input: bdk_string_t
) {
    guard let bridge = bridge(context) else { return }
    let json = swiftString(input)
    MainActor.assumeIsolated { bridge.start(slot, call: call, input: json) }
}

private let providerCancelCallback: bdk_provider_op_cb = { context, call, _ in
    guard let bridge = bridge(context) else { return }
    MainActor.assumeIsolated { bridge.cancel(call: call) }
}

private let providerReleaseCallback: bdk_release_cb = { context in
    guard let context else { return }
    let box = Unmanaged<BDKProviderBridge>.fromOpaque(context)
    MainActor.assumeIsolated { box.takeUnretainedValue().cancelAll() }
    box.release()
}

private let providerCallbacks: [BDKProviderSlot: bdk_provider_op_cb] = [
    .supportedPairs: { run(.supportedPairs, $0, $1, $2) },
    .prepare: { run(.prepare, $0, $1, $2) },
    .translate: { run(.translate, $0, $1, $2) },
    .detect: { run(.detect, $0, $1, $2) },
    .removeModel: { run(.removeModel, $0, $1, $2) },
    .checkURL: { run(.checkURL, $0, $1, $2) },
    .checkDownload: { run(.checkDownload, $0, $1, $2) },
    .update: { run(.update, $0, $1, $2) },
    .start: { run(.start, $0, $1, $2) },
]
