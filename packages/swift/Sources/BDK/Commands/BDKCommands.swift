import CBDK
import Foundation

/// An application event as the runtime delivers it, before decoding.
public struct BDKApplicationEvent: Sendable, Equatable {
    public let name: String
    public let payloadJSON: String
}

/// Application commands and events, and the generic entry point for invoking any command by
/// name: `runtime.commands` (swift-api.md sections 11.1 and 12). Calls made from Swift are not
/// capability-checked.
@MainActor
public final class BDKCommands {
    private weak var runtime: BDKRuntime?
    private var registry = BDKCommandRegistry()
    private var openStreams: [UUID: AsyncStream<BDKApplicationEvent>.Continuation] = [:]

    init(runtime: BDKRuntime) {
        self.runtime = runtime
    }

    /// What has been registered and declared, for code generation.
    public var declarations: BDKDeclarations {
        registry.declarations
    }

    // MARK: Registration

    /// Registers a command with the runtime. Allowed between start and the freeze; afterwards it
    /// throws `config.registrationAfterStart`. `BDKRuntime.start` freezes at once, so an
    /// application registers on `BDKConfiguration.commands` instead.
    public func register<Command: BDKCommand>(
        _ type: Command.Type,
        handler:
            @escaping @MainActor @Sendable (Command.Input, BDKCommandContext) async throws ->
            Command.Output
    ) throws {
        var single = BDKCommandRegistry()
        single.register(type, handler: handler)
        try adopt(single)
    }

    /// Declares an event with the runtime, between start and the freeze.
    public func declare<Event: BDKEvent>(_ type: Event.Type) throws {
        var single = BDKCommandRegistry()
        single.declare(type)
        try adopt(single)
    }

    /// Registers everything in `registry` with the runtime.
    func adopt(_ registry: BDKCommandRegistry) throws {
        guard let runtime else { throw BDKError.stale(.runtime) }
        let handle = try runtime.liveHandle()
        for entry in registry.commands {
            try register(entry, runtime: runtime, handle: handle)
            self.registry.commands.append(entry)
        }
        for event in registry.events {
            try withBDKStrings([event.name, event.json.jsonString]) { lent in
                try abiCall(.runtime) { bdk_app_event_declare(handle, lent[0], lent[1], $0) }
            }
            self.registry.events.append(event)
        }
    }

    private func register(
        _ entry: BDKCommandRegistry.CommandEntry, runtime: BDKRuntime, handle: bdk_runtime_t
    ) throws {
        let box = Unmanaged.passRetained(CommandBox(invoke: entry.invoke, runtime: runtime))
        let strings = [entry.declaration.name, entry.declaration.json.jsonString]
        do {
            try withBDKStrings(strings) { lent in
                try abiCall(.runtime) {
                    bdk_app_command_register(
                        handle, lent[0], lent[1], commandCallback, box.toOpaque(),
                        commandReleaseCallback, $0)
                }
            }
        } catch {
            // A refused registration leaves the context with the caller.
            box.release()
            throw error
        }
    }

    // MARK: Events

    /// Emits an application event. It must have been declared.
    public func emit<Event: BDKEvent>(_ event: Event, to target: BDKEventTarget = .all) throws {
        let handle = try liveHandle()
        let payload = try BDKJSON.encode(event)
        try withBDKStrings([Event.name, payload, target.json.jsonString]) { lent in
            try abiCall(.runtime) { bdk_app_event_emit(handle, lent[0], lent[1], lent[2], $0) }
        }
    }

    /// The events named `name`, as the runtime delivers them. The stream ends when it is no longer
    /// iterated, or when the runtime shuts down.
    public func events(named name: String, profile: BDKProfile? = nil) throws -> AsyncStream<
        BDKApplicationEvent
    > {
        let handle = try liveHandle()
        let (stream, continuation) = AsyncStream.makeStream(of: BDKApplicationEvent.self)
        let box = Unmanaged.passRetained(EventBox(continuation))
        let subscription = withBDKStrings([name, ""]) { lent in
            withInvokeOptions(profile: profile, contents: nil) {
                bdk_subscribe_named(
                    handle, lent[0], lent[1], $0, eventCallback, box.toOpaque(),
                    eventReleaseCallback)
            }
        }
        EventLoop.kick()
        guard subscription != 0 else {
            box.release()
            throw BDKError(
                domain: .broker, code: "invalidArguments",
                message: "the runtime rejected the subscription to \(name)")
        }
        let key = UUID()
        openStreams[key] = continuation
        continuation.onTermination = { [weak self] _ in
            Task { @MainActor in
                self?.openStreams[key] = nil
                bdk_unsubscribe(subscription)
                EventLoop.kick()
            }
        }
        return stream
    }

    /// The events of one type, decoded. An event whose payload does not decode is skipped.
    public func events<Event: BDKEvent>(_ type: Event.Type, profile: BDKProfile? = nil) throws
        -> AsyncCompactMapSequence<AsyncStream<BDKApplicationEvent>, Event>
    {
        try events(named: type.name, profile: profile).compactMap {
            try? BDKJSON.decode(Event.self, from: $0.payloadJSON)
        }
    }

    // MARK: Invocation

    /// Invokes a command by name with JSON arguments and returns the JSON result. `onProgress`
    /// runs on the main actor for each progress report. Cancelling the task cancels the call.
    public func invoke(
        _ name: String, arguments: String = "{}", profile: BDKProfile? = nil,
        contents: BDKWebContents? = nil,
        onProgress: (@MainActor (Double, String) -> Void)? = nil
    ) async throws -> String {
        let handle = try liveHandle()
        let profileHandle = try profile?.liveHandle()
        let contentsHandle = try contents?.liveHandle()
        let call = InvokeCall(
            runtime: handle, name: name, arguments: arguments, profile: profileHandle,
            contents: contentsHandle)
        return try await abiAsync(.runtime, progress: onProgress, call.issue)
    }

    /// Invokes a typed command.
    public func invoke<Command: BDKCommand>(
        _ type: Command.Type, _ input: Command.Input, profile: BDKProfile? = nil,
        contents: BDKWebContents? = nil,
        onProgress: (@MainActor (Double, String) -> Void)? = nil
    ) async throws -> Command.Output {
        let json = try await invoke(
            type.name, arguments: try BDKJSON.encode(input), profile: profile, contents: contents,
            onProgress: onProgress)
        return try decodeResult(Command.Output.self, from: json)
    }

    // MARK: Lifetime

    private func liveHandle() throws -> bdk_runtime_t {
        guard let runtime else { throw BDKError.stale(.runtime) }
        return try runtime.liveHandle()
    }

    /// Ends every event stream after the runtime released its subscriptions.
    func invalidate() {
        let streams = openStreams.values
        openStreams.removeAll()
        for stream in streams { stream.finish() }
    }
}

extension BDKEventTarget {
    /// The `target_json` of `bdk_app_event_emit`. Provisional: c-abi.md does not fix it.
    var json: BDKJSValue {
        switch self {
        case .all: .object(["scope": .string("all")])
        case .profile(let id):
            .object(["scope": .string("profile"), "profile": .string(id.rawValue)])
        case .page(let id):
            .object(["scope": .string("page"), "contents": .number(Double(id.rawValue))])
        case .session(let id): .object(["scope": .string("session"), "session": .string(id)])
        }
    }
}

/// Lends the invoke options for a profile and a page, or nil for neither.
@MainActor
private func withInvokeOptions<Result>(
    profile: BDKProfile?, contents: BDKWebContents?,
    _ body: (UnsafePointer<bdk_invoke_options_t>?) -> Result
) -> Result {
    withInvokeOptions(profileHandle: profile?.handle, contentsHandle: contents?.handle, body)
}

private func withInvokeOptions<Result>(
    profileHandle: bdk_profile_t?, contentsHandle: bdk_contents_t?,
    _ body: (UnsafePointer<bdk_invoke_options_t>?) -> Result
) -> Result {
    guard profileHandle != nil || contentsHandle != nil else { return body(nil) }
    var options = bdk_invoke_options_t()
    options.struct_size = MemoryLayout<bdk_invoke_options_t>.size
    options.profile = profileHandle ?? 0
    options.contents = contentsHandle ?? 0
    return withUnsafePointer(to: &options) { body($0) }
}

/// One `bdk_invoke_named` call, ready to issue.
private struct InvokeCall {
    let runtime: bdk_runtime_t
    let name: String
    let arguments: String
    let profile: bdk_profile_t?
    let contents: bdk_contents_t?

    func issue(
        context: UnsafeMutableRawPointer, result: bdk_result_cb, progress: bdk_progress_cb,
        release: bdk_release_cb
    ) -> bdk_call_t {
        withBDKStrings([name, arguments]) { lent in
            withInvokeOptions(profileHandle: profile, contentsHandle: contents) {
                bdk_invoke_named(
                    runtime, lent[0], lent[1], $0, result, progress, context, release)
            }
        }
    }
}

// MARK: - Callbacks

@MainActor
private final class CommandBox {
    let invoke: BDKCommandRegistry.Invocation
    weak var runtime: BDKRuntime?

    init(invoke: @escaping BDKCommandRegistry.Invocation, runtime: BDKRuntime) {
        self.invoke = invoke
        self.runtime = runtime
    }

    /// Runs the handler in a task, answers the call when it ends, and cancels the task when the
    /// caller cancels the call.
    func run(call: bdk_context_t, inputJSON: String, callerJSON: String) {
        // The profile the call addresses, which the caller's identity does not carry for the
        // host: the core resolved it from the invoke options or the page caller.
        let profile = runtime?.profiles.loaded(handle: bdk_app_command_profile(call))
        let context = BDKCommandContext(call: call, callerJSON: callerJSON, profile: profile)
        let invoke = self.invoke
        let run = CommandRun()
        let task = Task { @MainActor in
            do {
                context.resolve(try await invoke(inputJSON, context))
            } catch let error as BDKError {
                context.reject(error)
            } catch is CancellationError {
                context.reject(
                    BDKError(
                        domain: .broker, code: "cancelled",
                        message: "the caller cancelled the command"))
            } catch {
                context.reject(
                    BDKError(
                        domain: .broker, code: "handlerFailed", message: "\(error)"))
            }
            run.isFinished = true
        }
        Task { @MainActor in
            while !run.isFinished {
                if context.isCancelled {
                    task.cancel()
                    return
                }
                try? await Task.sleep(for: .milliseconds(10))
            }
        }
    }
}

@MainActor
private final class CommandRun {
    var isFinished = false
}

private let commandCallback: bdk_app_command_cb = { context, call, input in
    guard let context else { return }
    let box = Unmanaged<CommandBox>.fromOpaque(context).takeUnretainedValue()
    let inputJSON = swiftString(input)
    let callerJSON = swiftString(bdk_app_command_caller_json(call))
    MainActor.assumeIsolated {
        box.run(call: call, inputJSON: inputJSON, callerJSON: callerJSON)
    }
}

private let commandReleaseCallback: bdk_release_cb = { context in
    guard let context else { return }
    Unmanaged<CommandBox>.fromOpaque(context).release()
}

private final class EventBox: Sendable {
    let continuation: AsyncStream<BDKApplicationEvent>.Continuation

    init(_ continuation: AsyncStream<BDKApplicationEvent>.Continuation) {
        self.continuation = continuation
    }
}

private let eventCallback: bdk_event_cb = { context, _, name, payload in
    guard let context else { return }
    let box = Unmanaged<EventBox>.fromOpaque(context).takeUnretainedValue()
    box.continuation.yield(
        BDKApplicationEvent(name: swiftString(name), payloadJSON: swiftString(payload)))
}

private let eventReleaseCallback: bdk_release_cb = { context in
    guard let context else { return }
    Unmanaged<EventBox>.fromOpaque(context).release()
}
