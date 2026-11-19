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
