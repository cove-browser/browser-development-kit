import Foundation

/// A JSON value. Error details use it today; scripting results use it later (T-AUT-03).
public enum BDKJSValue: Sendable, Equatable {
    case null
    case bool(Bool)
    case number(Double)
    case string(String)
    case array([BDKJSValue])
    case object([String: BDKJSValue])

    /// Parses a JSON text. Returns nil when it is not valid JSON.
    public init?(jsonString: String) {
        guard
            let parsed = try? JSONSerialization.jsonObject(
                with: Data(jsonString.utf8), options: [.fragmentsAllowed])
        else { return nil }
        self.init(foundation: parsed)
    }

    private init(foundation value: Any) {
        switch value {
        case let number as NSNumber:
            // CFBoolean is the only NSNumber that is a Bool.
            if CFGetTypeID(number) == CFBooleanGetTypeID() {
                self = .bool(number.boolValue)
            } else {
                self = .number(number.doubleValue)
            }
        case let string as String:
            self = .string(string)
        case let array as [Any]:
            self = .array(array.map { BDKJSValue(foundation: $0) })
        case let object as [String: Any]:
            self = .object(object.mapValues { BDKJSValue(foundation: $0) })
        default:
            self = .null
        }
    }
}

extension BDKJSValue: Codable {
    public init(from decoder: any Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            self = .null
        } else if let value = try? container.decode(Bool.self) {
            self = .bool(value)
        } else if let value = try? container.decode(Double.self) {
            self = .number(value)
        } else if let value = try? container.decode(String.self) {
            self = .string(value)
        } else if let value = try? container.decode([BDKJSValue].self) {
            self = .array(value)
        } else {
            self = .object(try container.decode([String: BDKJSValue].self))
        }
    }

    public func encode(to encoder: any Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .null: try container.encodeNil()
        case .bool(let value): try container.encode(value)
        case .number(let value): try container.encode(value)
        case .string(let value): try container.encode(value)
        case .array(let values): try container.encode(values)
        case .object(let fields): try container.encode(fields)
        }
    }
}

extension BDKJSValue {
    /// The compact JSON text of this value, with sorted keys so that the output is stable.
    package var jsonString: String {
        let data =
            (try? JSONSerialization.data(
                withJSONObject: foundationValue,
                options: [.fragmentsAllowed, .sortedKeys, .withoutEscapingSlashes])) ?? Data()
        return String(decoding: data, as: UTF8.self)
    }

    private var foundationValue: Any {
        switch self {
        case .null: NSNull()
        case .bool(let value): value
        case .number(let value): value
        case .string(let value): value
        case .array(let values): values.map(\.foundationValue)
        case .object(let fields): fields.mapValues(\.foundationValue)
        }
    }

    package var stringValue: String? {
        if case .string(let value) = self { value } else { nil }
    }

    package var boolValue: Bool? {
        if case .bool(let value) = self { value } else { nil }
    }

    package var doubleValue: Double? {
        if case .number(let value) = self { value } else { nil }
    }

    package var arrayValue: [BDKJSValue]? {
        if case .array(let values) = self { values } else { nil }
    }

    package var objectValue: [String: BDKJSValue]? {
        if case .object(let fields) = self { fields } else { nil }
    }
}
