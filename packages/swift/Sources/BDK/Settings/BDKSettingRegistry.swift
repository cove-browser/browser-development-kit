import CBDK
import Foundation

/// The name and value type of an application setting (swift-api.md section 12.3). The name starts
/// with `app.`; the type is what `profile.settings.get(_:)` decodes.
public struct BDKSettingKey<Value: Codable & Sendable>: Sendable, Hashable {
    public let name: String

    public init(_ name: String) {
        self.name = name
    }
}

/// What code generation and the runtime read for one application setting.
public struct BDKSettingDeclaration: Sendable, Equatable {
    public let name: String
    /// `boolean`, `number`, `string`, `array` or `object`, taken from the default; `json` when the
    /// default is null, which accepts any value.
    public let type: String
    public let defaultValue: BDKJSValue
    public let scope: BDKSettingScope
    public let exposedToPages: Bool
    /// Why the default could not be encoded, reported when the runtime starts.
    let encodingProblem: String?

    init<Value: Codable & Sendable>(
        _ key: BDKSettingKey<Value>, default value: Value, scope: BDKSettingScope,
        exposedToPages: Bool
    ) {
        name = key.name
        self.scope = scope
        self.exposedToPages = exposedToPages
        var encoded = BDKJSValue.null
        var problem: String?
        do {
            let json = try BDKServiceCoding.encode(value)
            if let parsed = BDKJSValue(jsonString: json) {
                encoded = parsed
            } else {
                problem = "its default is not JSON"
            }
        } catch {
            problem = "its default cannot be encoded: \(error)"
        }
        defaultValue = encoded
        encodingProblem = problem
        type =
            switch encoded {
            case .bool: "boolean"
            case .number: "number"
            case .string: "string"
            case .array: "array"
            case .object: "object"
            case .null: "json"
            }
    }

    /// The `declaration_json` of `bdk_app_setting_register`.
    var registrationJSON: BDKJSValue {
        .object([
            "key": .string(name), "default": defaultValue, "scope": .string(scope.rawValue),
            "exposedToPages": .bool(exposedToPages),
        ])
    }

    /// The entry of `bdk.commands.json`.
    var json: BDKJSValue {
        .object([
            "key": .string(name), "type": .string(type), "default": defaultValue,
            "scope": .string(scope.rawValue), "exposedToPages": .bool(exposedToPages),
        ])
    }
}

/// The application's settings, collected before start: `config.settings`. It is a value; code
/// generation builds one without a runtime (`declarations`).
public struct BDKSettingRegistry: Sendable {
    var settings: [BDKSettingDeclaration] = []

    public init() {}

    /// Registers an application setting. The name is checked when the runtime starts: one that does
    /// not start with `app.`, or is used twice, is a start error (`config.nameNotApp`,
    /// `config.duplicateName`). The setting is stored with the browser settings of its scope and
    /// read and written through `settings.get`, `set`, `describe` and `reset`. `exposedToPages:
    /// false` keeps it out of reach of every page and session, whatever their capabilities.
    public mutating func register<Value: Codable & Sendable>(
        _ key: BDKSettingKey<Value>, default value: Value, scope: BDKSettingScope = .profile,
        exposedToPages: Bool = false
    ) {
        settings.append(
            BDKSettingDeclaration(
                key, default: value, scope: scope, exposedToPages: exposedToPages))
    }

    /// The start errors a registry can have without a runtime.
    func validate() throws {
        var seen: Set<String> = []
        for setting in settings {
            guard setting.name.hasPrefix("app."), setting.name.count > 4 else {
                throw BDKError(
                    domain: .config, code: "nameNotApp",
                    message: "the setting '\(setting.name)' must start with 'app.'")
            }
            guard seen.insert(setting.name).inserted else {
                throw BDKError(
                    domain: .config, code: "duplicateName",
                    message: "the setting '\(setting.name)' is registered twice")
            }
            if let problem = setting.encodingProblem {
                throw BDKError(
                    domain: .config, code: "settingDefaultInvalid",
                    message: "the setting '\(setting.name)': \(problem)")
            }
        }
    }

    /// Hands every setting to the runtime. A registration, so before the freeze.
    @MainActor
    func adopt(into runtime: BDKRuntime) throws {
        let handle = try runtime.liveHandle()
        for setting in settings {
            try withBDKString(setting.registrationJSON.jsonString) { lent in
                try abiCall(.runtime) { bdk_app_setting_register(handle, lent, $0) }
            }
        }
    }
}
