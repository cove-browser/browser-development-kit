import Foundation

// Application commands and events as Swift types (swift-api.md section 12).
//
// Swift is the source of truth for `app.` commands and events. A type declares its name and the
// shape of its input and output next to the `Codable` types, and the declaration is what the
// runtime registers and what code generation reads (BDKCommandRegistry.writeDeclarations).
// Swift cannot reflect a `Codable` type's fields, so the shape is written once, in the same place.

/// A type expression in the interface language of `packages/core/interface` (c-abi.md section 7):
/// `string`, `int`, `double`, `bool`, `url`, `timestamp`, `bytes`, `json`, the name of a type in
/// `types`, `T?` for optional and `T[]` for an array.
public struct BDKType: Sendable, Hashable, ExpressibleByStringLiteral {
    public let expression: String

    public init(_ expression: String) {
        self.expression = expression
    }

    public init(stringLiteral value: String) {
        expression = value
    }

    public static let string = BDKType("string")
    public static let int = BDKType("int")
    public static let double = BDKType("double")
    public static let bool = BDKType("bool")
    public static let url = BDKType("url")
    public static let timestamp = BDKType("timestamp")
    public static let bytes = BDKType("bytes")
    public static let json = BDKType("json")

    /// A struct or enum declared in `types`.
    public static func named(_ name: String) -> BDKType {
        BDKType(name)
    }

    public var optional: BDKType {
        BDKType(expression + "?")
    }

    public var array: BDKType {
        BDKType(expression + "[]")
    }
}

/// The arguments or the result of a command, or the payload of an event.
public enum BDKShape: Sendable, Equatable {
    /// No arguments, or no result.
    case void
    /// A struct written inline: field name to type.
    case fields([String: BDKType])
    /// Anything else, such as a named type or a scalar.
    case type(BDKType)
}

/// A named type in the `types` section of a declaration.
public enum BDKTypeDefinition: Sendable, Equatable {
    case structure([String: BDKType])
    case enumeration([String])
    case alias(BDKType)
}

public enum BDKProfileRequirement: String, Sendable {
    case required
    case optional
    case none
}

/// An input or output with no fields: `{}`.
public struct BDKVoid: Codable, Sendable, Equatable {
    public init() {}
}

/// An application command. `name` must start with `app.`; a caller needs the capability with the
/// same name, or a matching `app.….*` wildcard.
public protocol BDKCommand: Sendable {
    associatedtype Input: Codable & Sendable
    associatedtype Output: Codable & Sendable

    static var name: String { get }
    static var input: BDKShape { get }
    static var output: BDKShape { get }
    /// Structs and enums that `input` and `output` refer to by name.
    static var types: [String: BDKTypeDefinition] { get }
    /// A page caller must have come from a user gesture in that page.
    static var requiresUserGesture: Bool { get }
    static var profile: BDKProfileRequirement { get }
    /// The handler reports progress through `BDKCommandContext.report`.
    static var reportsProgress: Bool { get }
}

extension BDKCommand {
    public static var input: BDKShape { .type(.json) }
    public static var output: BDKShape { .type(.json) }
    public static var types: [String: BDKTypeDefinition] { [:] }
    public static var requiresUserGesture: Bool { false }
    public static var profile: BDKProfileRequirement { .optional }
    public static var reportsProgress: Bool { false }
}

/// An application event. A page or session receives it only with the capability named like the
/// event.
public protocol BDKEvent: Codable, Sendable {
    static var name: String { get }
    static var payload: BDKShape { get }
    static var types: [String: BDKTypeDefinition] { get }
}

extension BDKEvent {
    public static var payload: BDKShape { .type(.json) }
    public static var types: [String: BDKTypeDefinition] { [:] }
}

/// Who receives an application event (swift-api.md section 12.2).
public enum BDKEventTarget: Sendable, Equatable {
    case all
    case profile(BDKProfile.ID)
    case page(BDKWebContents.ID)
    case session(String)
}

// MARK: - Declarations

/// What code generation reads for one command.
public struct BDKCommandDeclaration: Sendable, Equatable {
    public let name: String
    public let input: BDKShape
    public let output: BDKShape
    public let types: [String: BDKTypeDefinition]
    public let requiresUserGesture: Bool
    public let profile: BDKProfileRequirement
    public let reportsProgress: Bool

    init<Command: BDKCommand>(_ command: Command.Type) {
        name = command.name
        input = command.input
        output = command.output
        types = command.types
        requiresUserGesture = command.requiresUserGesture
        profile = command.profile
        reportsProgress = command.reportsProgress
    }

    /// The JSON the runtime registers: the interface-definition entry for a command, with
    /// `callers` and `capability` as the broker applies them.
    var json: BDKJSValue {
        var fields: [String: BDKJSValue] = [
            "name": .string(name), "capability": .string(name),
            "callers": .array([.string("swift"), .string("page"), .string("session")]),
            "profile": .string(profile.rawValue), "args": input.json, "result": output.json,
        ]
        if requiresUserGesture { fields["requires"] = .array([.string("gesture")]) }
        if reportsProgress { fields["progress"] = .bool(true) }
        if !types.isEmpty { fields["types"] = types.json }
        return .object(fields)
    }
}

/// What code generation reads for one event.
public struct BDKEventDeclaration: Sendable, Equatable {
    public let name: String
    public let payload: BDKShape
    public let types: [String: BDKTypeDefinition]

    init<Event: BDKEvent>(_ event: Event.Type) {
        name = event.name
        payload = event.payload
        types = event.types
    }

    var json: BDKJSValue {
        var fields: [String: BDKJSValue] = [
            "name": .string(name), "capability": .string(name), "payload": payload.json,
        ]
        if !types.isEmpty { fields["types"] = types.json }
        return .object(fields)
    }
}

extension BDKShape {
    var json: BDKJSValue {
        switch self {
        case .void: .string("void")
        case .fields(let fields): .object(fields.mapValues { .string($0.expression) })
        case .type(let type): .string(type.expression)
        }
    }
}

extension BDKTypeDefinition {
    var json: BDKJSValue {
        switch self {
        case .structure(let fields): .object(fields.mapValues { .string($0.expression) })
        case .enumeration(let cases): .object(["enum": .array(cases.map(BDKJSValue.string))])
        case .alias(let type): .string(type.expression)
        }
    }
}

extension Dictionary where Key == String, Value == BDKTypeDefinition {
    fileprivate var json: BDKJSValue {
        .object(mapValues(\.json))
    }
}

/// The application's commands and events as one JSON file, for code generation.
public struct BDKDeclarations: Sendable, Equatable {
    /// The format version of the file.
    public static let formatVersion = 1
    /// The conventional file name.
    public static let fileName = "bdk.commands.json"

    public let commands: [BDKCommandDeclaration]
    public let events: [BDKEventDeclaration]

    /// Deterministic: commands and events sorted by name, keys sorted, so that the same input
    /// gives the same bytes.
    public var jsonString: String {
        BDKJSValue.object([
            "version": .number(Double(Self.formatVersion)),
            "commands": .array(commands.sorted { $0.name < $1.name }.map(\.json)),
            "events": .array(events.sorted { $0.name < $1.name }.map(\.json)),
        ]).jsonString
    }

    /// Writes `bdk.commands.json`-style output to `url`, replacing the file.
    public func write(to url: URL) throws {
        try Data(jsonString.utf8).write(to: url, options: .atomic)
    }
}
