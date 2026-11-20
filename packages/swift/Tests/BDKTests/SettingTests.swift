import BDKTestSupport
import Foundation
import Testing

@testable import BDK

struct Sidebar: Codable, Sendable, Equatable {
    var width: Double
    var pinned: [String]
}

extension BDKSettingKey where Value == Double {
    static let sidebarWidth = BDKSettingKey<Double>("app.sidebar.width")
}

extension BDKStubTests {
    @MainActor
    @Suite("Application settings")
    struct SettingTests {
        static let sidebar = BDKSettingKey<Sidebar>("app.sidebar")
        static let lastWorkspace = BDKSettingKey<String>("app.lastWorkspace")
        static let hidden = BDKSettingKey<Bool>("app.hidden")

        private func configuration() -> BDKConfiguration {
            var configuration = BDKConfiguration.stub
            configuration.settings.register(
                .sidebarWidth, default: 240, scope: .profile, exposedToPages: true)
            configuration.settings.register(
                Self.sidebar, default: Sidebar(width: 1, pinned: []), scope: .profile,
                exposedToPages: true)
            configuration.settings.register(
                Self.lastWorkspace, default: "main", scope: .application, exposedToPages: true)
            configuration.settings.register(
                Self.hidden, default: false, scope: .profile, exposedToPages: false)
            return configuration
        }

        @Test func aRegisteredKeyReadsItsDefaultAndTakesTypedValues() async throws {
            try await withStubRuntime(configuration()) { runtime in
                let profile = try await runtime.profiles.open("Typed")
                #expect(try await profile.settings.get(.sidebarWidth) == 240)
                try await profile.settings.set(.sidebarWidth, 300)
                #expect(try await profile.settings.get(.sidebarWidth) == 300)
                // The same value through the string key.
                #expect(try await profile.settings.get("app.sidebar.width") == .number(300))

                let value = Sidebar(width: 12.5, pinned: ["a", "b"])
                try await profile.settings.set(Self.sidebar, value)
                #expect(try await profile.settings.get(Self.sidebar) == value)

                let info = try await profile.settings.describe(.sidebarWidth)
                #expect(info.source == .user && info.scope == .profile)
                #expect(info.default == .number(240) && info.value == .number(300))

                try await profile.settings.reset(.sidebarWidth)
                #expect(try await profile.settings.get(.sidebarWidth) == 240)
                #expect(try await profile.settings.describe(.sidebarWidth).source == .product)
            }
        }

        @Test func aValueOfTheWrongTypeIsRefused() async throws {
            try await withStubRuntime(configuration()) { runtime in
                let profile = try await runtime.profiles.open("Wrong")
                await expectError(.broker, "invalidArguments") {
                    try await profile.settings.set("app.sidebar.width", "wide")
                }
                #expect(try await profile.settings.get(.sidebarWidth) == 240)
            }
        }

        @Test func applicationScopeIsOneValueForEveryProfile() async throws {
            try await withStubRuntime(configuration()) { runtime in
                let first = try await runtime.profiles.open("One")
                let second = try await runtime.profiles.open("Two")
                try await runtime.settings.set(Self.lastWorkspace, "work")
                #expect(try await first.settings.get(Self.lastWorkspace) == "work")
                #expect(try await second.settings.get(Self.lastWorkspace) == "work")
                try await first.settings.set(.sidebarWidth, 50)
                #expect(try await second.settings.get(.sidebarWidth) == 240)
            }
        }

        @Test func aProfileScopedSettingSurvivesClosingAndReopeningTheProfile() async throws {
            try await withStubRuntime(configuration()) { runtime in
                let profile = try await runtime.profiles.open("Keeper")
                try await profile.settings.set(.sidebarWidth, 410)
                let id = profile.id
                try profile.close()
                let reopened = try await runtime.profiles.open(id: id)
                #expect(try await reopened.settings.get(.sidebarWidth) == 410)
            }
        }

        @Test func aChangeIsAnnounced() async throws {
            try await withStubRuntime(configuration()) { runtime in
                let profile = try await runtime.profiles.open("Events")
                let reader = EventReader(profile.settings.events)
                try await profile.settings.set(.sidebarWidth, 260)
                let event = try await reader.next { _ in true }
                guard case .changed(let change) = event else {
                    Issue.record("expected settings.changed")
                    return
                }
                #expect(
                    change.key == "app.sidebar.width" && change.oldValue == .number(240)
                        && change.newValue == .number(260))
            }
        }

        @Test func namesNeedTheAppPrefixAndMustBeUnique() async throws {
            var wrong = BDKConfiguration.stub
            wrong.settings.register(BDKSettingKey<Int>("ui.density"), default: 1)
            do {
                _ = try BDKRuntime.startStub(wrong)
                Issue.record("expected config.nameNotApp")
            } catch let error as BDKError {
                #expect(error.domain == .config && error.code == "nameNotApp")
            }
            var twice = BDKConfiguration.stub
            twice.settings.register(BDKSettingKey<Int>("app.n"), default: 1)
            twice.settings.register(BDKSettingKey<Int>("app.n"), default: 2)
            do {
                _ = try BDKRuntime.startStub(twice)
                Issue.record("expected config.duplicateName")
            } catch let error as BDKError {
                #expect(error.domain == .config && error.code == "duplicateName")
            }
            var unencodable = BDKConfiguration.stub
            unencodable.settings.register(BDKSettingKey<Double>("app.nan"), default: .nan)
            do {
                _ = try BDKRuntime.startStub(unencodable)
                Issue.record("expected config.settingDefaultInvalid")
            } catch let error as BDKError {
                #expect(error.domain == .config && error.code == "settingDefaultInvalid")
            }
            #expect(!BDKRuntime.isRunning)
        }

        @Test func registeredSettingsAreInTheDeclarations() throws {
            let declarations = configuration().declarations
            #expect(
                declarations.settings.map(\.name) == [
                    "app.sidebar.width", "app.sidebar", "app.lastWorkspace", "app.hidden",
                ])
            let json = try #require(BDKJSValue(jsonString: declarations.jsonString)?.objectValue)
            let settings = (json["settings"]?.arrayValue ?? []).compactMap(\.objectValue)
            #expect(
                settings.map { $0["key"]?.stringValue } == [
                    "app.hidden", "app.lastWorkspace", "app.sidebar", "app.sidebar.width",
                ])
            let width = try #require(settings.first { $0["key"] == .string("app.sidebar.width") })
            #expect(
                width == [
                    "key": .string("app.sidebar.width"), "type": .string("number"),
                    "default": .number(240), "scope": .string("profile"),
                    "exposedToPages": .bool(true),
                ])
            let workspace = try #require(
                settings.first { $0["key"] == .string("app.lastWorkspace") })
            #expect(
                workspace["scope"] == .string("application")
                    && workspace["type"] == .string("string"))
            let hidden = try #require(settings.first { $0["key"] == .string("app.hidden") })
            #expect(
                hidden["exposedToPages"] == .bool(false) && hidden["type"] == .string("boolean"))
            #expect(declarations.jsonString == configuration().declarations.jsonString)
        }
    }
}
