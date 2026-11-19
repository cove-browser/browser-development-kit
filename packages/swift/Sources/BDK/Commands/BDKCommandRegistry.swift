import CBDK
import Foundation

/// Who called an application command.
public enum BDKCommandCaller: Sendable, Equatable {
    case application
    case page(route: String?, contents: BDKWebContents.ID?)
    case automation(session: String)
    /// A kind this version does not know, with the runtime's JSON.
    case unknown(json: String)

    /// Reads `bdk_app_command_caller_json`: `{"caller": "host" | "page" | "session", ...}`. The
    /// encoding is provisional; the stub answers `{"caller":"host"}`.
    init(json: String) {
        let fields = BDKJSValue(jsonString: json)?.objectValue ?? [:]
        switch fields["caller"]?.stringValue {
        case "host", "swift", "application":
            self = .application
        case "page":
            self = .page(
                route: fields["route"]?.stringValue,
                contents: BDKUIPayload.handle(fields["contents"]).map {
                    BDKWebContents.ID(rawValue: $0)
                })
        case "session", "automation":
            self = .automation(session: fields["session"]?.stringValue ?? "")
        default:
            self = .unknown(json: json)
        }
    }
}

/// What a command handler knows about its call (swift-api.md section 12.1).
public struct BDKCommandContext: Sendable {
    public let caller: BDKCommandCaller
    public let profile: BDKProfile?
    private let call: bdk_context_t

    init(call: bdk_context_t, callerJSON: String, profile: BDKProfile?) {
        self.call = call
        caller = BDKCommandCaller(json: callerJSON)
        self.profile = profile
    }

    /// True once the caller cancelled the call. A handler that is cancelled also has its task
    /// cancelled, so `Task.isCancelled` and `Task.checkCancellation()` work too; this polls the
    /// ABI, which has no cancellation callback.
    public var isCancelled: Bool {
        bdk_app_command_is_cancelled(call) != 0
    }

    /// Throws `broker.cancelled` once the caller cancelled the call.
    public func checkCancelled() throws {
        if isCancelled {
            throw BDKError(
                domain: .broker, code: "cancelled", message: "the caller cancelled the command")
        }
    }

    /// Reports progress to a caller that asked for it. `progress` is a fraction from 0 to 1.
    @MainActor
    public func report(progress: Double, message: String? = nil) {
        withBDKString(message ?? "") { bdk_app_command_progress(call, progress, $0) }
        EventLoop.kick()
    }

    @MainActor
    func resolve(_ outputJSON: String) {
        withBDKString(outputJSON) { bdk_app_command_resolve(call, $0) }
        EventLoop.kick()
    }

    @MainActor
    func reject(_ error: BDKError) {
        withBDKStrings([error.domain.rawValue, error.code, error.message]) {
            bdk_app_command_reject(call, $0[0], $0[1], $0[2])
        }
        EventLoop.kick()
    }
}

/// The application's commands and events, collected before start: `config.commands`. It is a
/// value; code generation builds one without a runtime (`declarations`).
public struct BDKCommandRegistry: Sendable {
    typealias Invocation = @MainActor @Sendable (String, BDKCommandContext) async throws -> String

    struct CommandEntry: Sendable {
        let declaration: BDKCommandDeclaration
        let invoke: Invocation
    }

    var commands: [CommandEntry] = []
    var events: [BDKEventDeclaration] = []

    public init() {}

    /// Registers a command. The name is checked when the runtime starts: one that does not start
    /// with `app.`, or is used twice, is a start error (`config.nameNotApp`, `config.duplicateName`).
    public mutating func register<Command: BDKCommand>(
        _ type: Command.Type,
        handler:
            @escaping @MainActor @Sendable (Command.Input, BDKCommandContext) async throws ->
            Command.Output
    ) {
        commands.append(
            CommandEntry(
                declaration: BDKCommandDeclaration(type), invoke: Self.invocation(type, handler)))
    }

    /// Declares an event the application will emit.
    public mutating func declare<Event: BDKEvent>(_ type: Event.Type) {
        events.append(BDKEventDeclaration(type))
    }

    /// Everything declared so far, for code generation.
    public var declarations: BDKDeclarations {
        BDKDeclarations(commands: commands.map(\.declaration), events: events)
    }

    /// The start errors a registry can have without a runtime: a name that does not start with
    /// `app.` (`config.nameNotApp`) or is used twice (`config.duplicateName`).
    func validate() throws {
        var seen: Set<String> = []
        let names =
            commands.map { ("command", $0.declaration.name) } + events.map { ("event", $0.name) }
        for (kind, name) in names {
            guard name.hasPrefix("app."), name.count > 4 else {
                throw BDKError(
                    domain: .config, code: "nameNotApp",
                    message: "the \(kind) '\(name)' must start with 'app.'")
            }
            guard seen.insert("\(kind) \(name)").inserted else {
                throw BDKError(
                    domain: .config, code: "duplicateName",
                    message: "the \(kind) '\(name)' is registered twice")
            }
        }
    }

    /// Decodes the input, runs the handler and encodes the output. A decoding failure is
    /// `broker.invalidArguments`.
    static func invocation<Command: BDKCommand>(
        _ type: Command.Type,
        _ handler:
            @escaping @MainActor @Sendable (Command.Input, BDKCommandContext) async throws ->
            Command.Output
    ) -> Invocation {
        { inputJSON, context in
            let input: Command.Input
            do {
                input = try BDKJSON.decode(Command.Input.self, from: inputJSON)
            } catch {
                throw BDKError(
                    domain: .broker, code: "invalidArguments",
                    message: "the arguments of \(Command.name) do not match its input: \(error)")
            }
            return try BDKJSON.encode(try await handler(input, context))
        }
    }
}

/// JSON in and out of `Codable` values, with sorted keys.
enum BDKJSON {
    static func decode<Value: Decodable>(_ type: Value.Type, from json: String) throws -> Value {
        try JSONDecoder().decode(type, from: Data((json.isEmpty ? "{}" : json).utf8))
    }

    static func encode<Value: Encodable>(_ value: Value) throws -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return String(decoding: try encoder.encode(value), as: UTF8.self)
    }
}
