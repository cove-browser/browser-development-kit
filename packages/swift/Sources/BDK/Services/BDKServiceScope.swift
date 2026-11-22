import Foundation

/// How the generated service wrappers (`Generated/BDKServices.swift`) talk to the runtime: JSON in,
/// JSON out, timestamps as milliseconds since 1970 (conventions.md, "The broker", rule 6).
enum BDKServiceCoding {
    static func encode<Value: Encodable>(_ value: Value) throws -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        // The broker wants an integer, so a date is rounded to the millisecond.
        encoder.dateEncodingStrategy = .custom { date, encoder in
            var container = encoder.singleValueContainer()
            try container.encode(Int64((date.timeIntervalSince1970 * 1000).rounded()))
        }
        return String(decoding: try encoder.encode(value), as: UTF8.self)
    }

    static func decode<Value: Decodable>(_ type: Value.Type, from json: String) throws -> Value {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .custom { decoder in
            let container = try decoder.singleValueContainer()
            return Date(timeIntervalSince1970: Double(try container.decode(Int64.self)) / 1000)
        }
        do {
            return try decoder.decode(type, from: Data((json.isEmpty ? "null" : json).utf8))
        } catch {
            throw BDKError(
                domain: .runtime, code: "malformedResult",
                message: "the runtime returned a result this version cannot read: \(error)")
        }
    }
}

/// Where a service call goes: the runtime, and the profile it runs in (nil for application scope).
@MainActor
struct BDKServiceScope {
    let runtime: BDKRuntime
    let profile: BDKProfile?
    /// The page a page command targets, for `contents.*`, `debug.*` and `devtools.*`.
    var contents: BDKWebContents?

    /// Calls `name` and decodes its result.
    func call<Arguments: Encodable, Output: Decodable>(
        _ name: String, _ arguments: Arguments, as type: Output.Type,
        onProgress: (@MainActor (Double, String) -> Void)? = nil
    ) async throws -> Output {
        let json = try await invoke(name, arguments, onProgress: onProgress)
        return try BDKServiceCoding.decode(type, from: json)
    }

    /// Calls a command that returns nothing.
    func run<Arguments: Encodable>(
        _ name: String, _ arguments: Arguments,
        onProgress: (@MainActor (Double, String) -> Void)? = nil
    ) async throws {
        _ = try await invoke(name, arguments, onProgress: onProgress)
    }

    /// Calls a command marked binary and returns its result and its bytes.
    func callBinary<Arguments: Encodable, Output: Decodable>(
        _ name: String, _ arguments: Arguments, as type: Output.Type,
        onProgress: (@MainActor (Double, String) -> Void)? = nil
    ) async throws -> (value: Output, binary: Data?) {
        let result = try await runtime.commands.invokeBinary(
            name, arguments: try BDKServiceCoding.encode(arguments), profile: profile,
            contents: contents, onProgress: onProgress)
        return (try BDKServiceCoding.decode(type, from: result.json), result.binary)
    }

    private func invoke<Arguments: Encodable>(
        _ name: String, _ arguments: Arguments,
        onProgress: (@MainActor (Double, String) -> Void)?
    ) async throws -> String {
        try await runtime.commands.invoke(
            name, arguments: try BDKServiceCoding.encode(arguments), profile: profile,
            contents: contents, onProgress: onProgress)
    }

    /// Subscribes to the events named `names` now and merges them into one stream of decoded events.
    /// A name the runtime refuses is skipped; an event that does not decode is dropped.
    func events<Event: Sendable>(
        _ names: [String], decode: @escaping @Sendable (String, String) throws -> Event?
    ) -> AsyncStream<Event> {
        let (stream, continuation) = AsyncStream.makeStream(of: Event.self)
        let sources = names.compactMap {
            try? runtime.commands.events(named: $0, profile: profile)
        }
        let task = Task {
            await withTaskGroup(of: Void.self) { group in
                for source in sources {
                    group.addTask {
                        for await delivered in source {
                            if let event = try? decode(delivered.name, delivered.payloadJSON) {
                                continuation.yield(event)
                            }
                        }
                    }
                }
            }
            continuation.finish()
        }
        continuation.onTermination = { _ in task.cancel() }
        return stream
    }
}

extension BDKSettings {
    /// One setting, decoded as `Value`. Throws `runtime.malformedResult` when the stored value is not one.
    public func get<Value: Decodable & Sendable>(_ key: String, as type: Value.Type) async throws
        -> Value
    {
        let value: BDKJSValue = try await get(key)
        return try BDKServiceCoding.decode(type, from: value.jsonString)
    }

    /// Sets a setting from any `Encodable` value.
    public func set<Value: Encodable>(_ key: String, _ value: Value) async throws {
        let json = try BDKServiceCoding.encode(value)
        try await set(key, value: BDKJSValue(jsonString: json) ?? .null)
    }
}
