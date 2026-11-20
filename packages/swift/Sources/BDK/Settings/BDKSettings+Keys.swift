import Foundation

/// Typed access to application settings (swift-api.md section 12.3), beside the string keys of the
/// generated `BDKSettings`.
extension BDKSettings {
    /// The value of an application setting: what was set, or its default.
    public func get<Value: Codable & Sendable>(_ key: BDKSettingKey<Value>) async throws -> Value {
        try await get(key.name, as: Value.self)
    }

    /// Sets an application setting. A page that holds `settings.write` can set it too, when the
    /// setting is exposed to pages.
    public func set<Value: Codable & Sendable>(_ key: BDKSettingKey<Value>, _ value: Value)
        async throws
    {
        try await set(key.name, value)
    }

    /// Puts an application setting back to its default.
    public func reset<Value: Codable & Sendable>(_ key: BDKSettingKey<Value>) async throws {
        try await reset(key.name)
    }

    /// Type, default, current value, scope and whether the user changed it.
    public func describe<Value: Codable & Sendable>(_ key: BDKSettingKey<Value>) async throws
        -> BDKSettingInfo
    {
        try await describe(key.name)
    }
}
