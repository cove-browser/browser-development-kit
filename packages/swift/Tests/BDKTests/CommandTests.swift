import BDKTestSupport
import Foundation
import Testing

@testable import BDK

struct OpenTab: BDKCommand {
    static let name = "app.tabs.open"
    struct Input: Codable, Sendable, Equatable {
        var url: URL
        var inBackground: Bool?
    }
    struct Output: Codable, Sendable, Equatable { var id: String }
    static let input = BDKShape.fields(["url": .url, "inBackground": BDKType.bool.optional])
    static let output = BDKShape.fields(["id": .string])
    static let requiresUserGesture = true
}

struct Crunch: BDKCommand {
    static let name = "app.crunch"
    typealias Input = BDKVoid
    typealias Output = BDKVoid
    static let input = BDKShape.void
    static let output = BDKShape.void
    static let reportsProgress = true
}

struct WhoAmI: BDKCommand {
    static let name = "app.whoami"
    typealias Input = BDKVoid
    struct Output: Codable, Sendable { var isApplication: Bool }
    static let output = BDKShape.type("Caller")
    static let types: [String: BDKTypeDefinition] = [
        "Caller": .enumeration(["application", "page"])
    ]
}

struct Wait: BDKCommand {
    static let name = "app.wait"
    typealias Input = BDKVoid
    typealias Output = BDKVoid
}

struct BadName: BDKCommand {
    static let name = "tabs.open"
    typealias Input = BDKVoid
    typealias Output = BDKVoid
}

struct PlainFailure: Error {}

struct UndeclaredEvent: BDKEvent {
    static let name = "app.other"
    var value = 1
}

struct TabsChanged: BDKEvent {
    static let name = "app.tabs.changed"
    static let payload = BDKShape.fields(["count": .int])
    var count: Int
}

/// What a handler observed, for the test to read.
@MainActor
final class CommandProbe {
    var sawCancellation = false
    var started = false
}

extension BDKStubTests {
    @MainActor
    @Suite("Application commands")
    struct CommandTests {
        private func configuration(
            _ probe: CommandProbe = CommandProbe()
        ) -> BDKConfiguration {
            var configuration = BDKConfiguration.stub
            configuration.commands.register(OpenTab.self) { input, _ in
                .init(id: "tab-\(input.url.host() ?? "?")-\(input.inBackground ?? false)")
            }
            configuration.commands.register(Crunch.self) { _, context in
                context.report(progress: 0.25, message: "a quarter")
                context.report(progress: 1, message: "done")
                return BDKVoid()
            }
            configuration.commands.register(WhoAmI.self) { _, context in
                .init(isApplication: context.caller == .application)
            }
            configuration.commands.register(Wait.self) { _, context in
                probe.started = true
                while true {
                    do {
                        try context.checkCancelled()
                        try await Task.sleep(for: .milliseconds(5))
                    } catch {
                        probe.sawCancellation = true
                        throw error
                    }
                }
            }
            configuration.commands.declare(TabsChanged.self)
            return configuration
        }

        @Test func aRegisteredCommandIsCallableFromSwift() async throws {
            try await withStubRuntime(configuration()) { runtime in
                let output = try await runtime.commands.invoke(
                    OpenTab.self, .init(url: URL(string: "https://a.test/")!, inBackground: true))
                #expect(output == .init(id: "tab-a.test-true"))
                let raw = try await runtime.commands.invoke(
                    "app.tabs.open", arguments: "{\"url\":\"https://b.test/\"}")
                #expect(raw == "{\"id\":\"tab-b.test-false\"}")
            }
        }

        @Test func aCommandReportsProgress() async throws {
            try await withStubRuntime(configuration()) { runtime in
                var reports: [String] = []
                _ = try await runtime.commands.invoke(Crunch.self, BDKVoid()) { fraction, message in
                    reports.append("\(fraction) \(message)")
                }
                #expect(reports == ["0.25 a quarter", "1.0 done"])
            }
        }

        @Test func aHandlerKnowsItsCaller() async throws {
            try await withStubRuntime(configuration()) { runtime in
                let output = try await runtime.commands.invoke(WhoAmI.self, BDKVoid())
                #expect(output.isApplication)
            }
        }

        @Test func aHandlerSeesTheCallerJSON() {
            #expect(BDKCommandCaller(json: "{\"caller\":\"host\"}") == .application)
            #expect(
                BDKCommandCaller(
                    json: "{\"caller\":\"page\",\"route\":\"settings\",\"contents\":\"7\"}")
                    == .page(route: "settings", contents: .init(rawValue: 7)))
            #expect(
                BDKCommandCaller(json: "{\"caller\":\"session\",\"session\":\"s1\"}")
                    == .automation(session: "s1"))
            #expect(
                BDKCommandCaller(json: "{\"caller\":\"robot\"}")
                    == .unknown(json: "{\"caller\":\"robot\"}"))
        }

        @Test func cancellingTheCallCancelsTheHandler() async throws {
            let probe = CommandProbe()
            try await withStubRuntime(configuration(probe)) { runtime in
                let commands = runtime.commands
                let call = Task { try await commands.invoke(Wait.self, BDKVoid()) }
                while !probe.started { try await Task.sleep(for: .milliseconds(5)) }
                call.cancel()
                await expectError(.broker, "cancelled") { _ = try await call.value }
                while !probe.sawCancellation { try await Task.sleep(for: .milliseconds(5)) }
            }
        }

        @Test func aHandlerErrorReachesTheCaller() async throws {
            var configuration = BDKConfiguration.stub
            configuration.commands.register(OpenTab.self) { _, _ in
                throw BDKError(domain: .capability, code: "denied", message: "not for you")
            }
            configuration.commands.register(Wait.self) { _, _ in throw PlainFailure() }
            try await withStubRuntime(configuration) { runtime in
                await expectError(.capability, "denied") {
                    _ = try await runtime.commands.invoke(
                        OpenTab.self, .init(url: URL(string: "https://a.test/")!))
                }
                await expectError(.broker, "handlerFailed") {
                    _ = try await runtime.commands.invoke(Wait.self, BDKVoid())
                }
            }
        }

        @Test func inputThatDoesNotMatchIsRefused() async throws {
            try await withStubRuntime(configuration()) { runtime in
                await expectError(.broker, "invalidArguments") {
                    _ = try await runtime.commands.invoke(
                        "app.tabs.open", arguments: "{\"nope\":1}")
                }
                await expectError(.broker, "unknownCommand") {
                    _ = try await runtime.commands.invoke("app.missing")
                }
            }
        }

        @Test func namesNeedTheAppPrefixAndMustBeUnique() async throws {
            var configuration = BDKConfiguration.stub
            configuration.commands.register(BadName.self) { _, _ in BDKVoid() }
            do {
                _ = try BDKRuntime.startStub(configuration)
                Issue.record("expected config.nameNotApp")
            } catch let error as BDKError {
                #expect(error.domain == .config && error.code == "nameNotApp")
            }
            var twice = BDKConfiguration.stub
            twice.commands.register(Wait.self) { _, _ in BDKVoid() }
            twice.commands.register(Wait.self) { _, _ in BDKVoid() }
            do {
                _ = try BDKRuntime.startStub(twice)
                Issue.record("expected config.duplicateName")
            } catch let error as BDKError {
                #expect(error.domain == .config && error.code == "duplicateName")
            }
            #expect(!BDKRuntime.isRunning)
        }

        @Test func registrationsAreRefusedAfterTheFreeze() async throws {
            try await withStubRuntime(freezingRegistrations: false) { runtime in
                try runtime.commands.register(Wait.self) { _, _ in BDKVoid() }
                try runtime.commands.declare(TabsChanged.self)
                #expect(runtime.commands.declarations.commands.map(\.name) == ["app.wait"])
                try runtime.freezeRegistrations()
                await expectError(.config, "registrationAfterStart") {
                    try runtime.commands.register(Crunch.self) { _, _ in BDKVoid() }
                }
                await expectError(.config, "registrationAfterStart") {
                    try runtime.commands.declare(TabsChanged.self)
                }
                _ = try await runtime.commands.invoke(Wait.self, BDKVoid())
            }
        }

        @Test func registrationRefusedByTheRuntimeNamesTheProblem() async throws {
            try await withStubRuntime(freezingRegistrations: false) { runtime in
                await expectError(.config, "nameNotApp") {
                    try runtime.commands.register(BadName.self) { _, _ in BDKVoid() }
                }
                try runtime.commands.register(Wait.self) { _, _ in BDKVoid() }
                await expectError(.config, "duplicateName") {
                    try runtime.commands.register(Wait.self) { _, _ in BDKVoid() }
                }
            }
        }

        @Test func eventsAreDeclaredEmittedAndObserved() async throws {
            try await withStubRuntime(configuration()) { runtime in
                let events = try runtime.commands.events(TabsChanged.self)
                let raw = try runtime.commands.events(named: "app.tabs.changed")
                try runtime.commands.emit(TabsChanged(count: 12), to: .all)
                try runtime.commands.emit(TabsChanged(count: 13), to: .page(.init(rawValue: 5)))
                runtime.stub.pump()
                var iterator = events.makeAsyncIterator()
                #expect(await iterator.next()?.count == 12)
                #expect(await iterator.next()?.count == 13)
                var rawIterator = raw.makeAsyncIterator()
                #expect(
                    await rawIterator.next()
                        == BDKApplicationEvent(
                            name: "app.tabs.changed", payloadJSON: "{\"count\":12}"))
            }
        }

        @Test func emittingAnUndeclaredEventFails() async throws {
            try await withStubRuntime(configuration()) { runtime in
                await expectError(.broker, "invalidArguments") {
                    try runtime.commands.emit(UndeclaredEvent())
                }
            }
        }

        @Test func eventStreamsEndWithTheRuntime() async throws {
            let runtime = try BDKRuntime.startStub(configuration())
            let events = try runtime.commands.events(TabsChanged.self)
            await runtime.shutdown()
            var iterator = events.makeAsyncIterator()
            #expect(await iterator.next() == nil)
        }

        // MARK: Declarations for code generation

        @Test func declarationsAreListedForCodeGeneration() throws {
            let declarations = configuration().commands.declarations
            #expect(declarations.commands.count == 4)
            #expect(declarations.events.map(\.name) == ["app.tabs.changed"])
            let json = try #require(BDKJSValue(jsonString: declarations.jsonString)?.objectValue)
            #expect(json["version"] == .number(1))
            let commands = try #require(json["commands"]?.arrayValue)
            let open = try #require(
                commands.compactMap(\.objectValue).first { $0["name"] == .string("app.tabs.open") })
            #expect(open["capability"] == .string("app.tabs.open"))
            #expect(open["profile"] == .string("optional"))
            #expect(open["requires"] == .array([.string("gesture")]))
            #expect(
                open["args"] == .object(["url": .string("url"), "inBackground": .string("bool?")]))
            #expect(open["result"] == .object(["id": .string("string")]))
            let whoami = try #require(
                commands.compactMap(\.objectValue).first { $0["name"] == .string("app.whoami") })
            #expect(whoami["result"] == .string("Caller"))
            #expect(
                whoami["types"]
                    == .object([
                        "Caller": .object([
                            "enum": .array([.string("application"), .string("page")])
                        ])
                    ]))
            let crunch = try #require(
                commands.compactMap(\.objectValue).first { $0["name"] == .string("app.crunch") })
            #expect(crunch["progress"] == .bool(true) && crunch["args"] == .string("void"))
        }

        @Test func declarationsAreByteStableAndWriteToAFile() throws {
            let first = configuration().commands.declarations
            let second = configuration().commands.declarations
            #expect(first.jsonString == second.jsonString)
            let url = FileManager.default.temporaryDirectory.appendingPathComponent(
                "bdk-\(UUID().uuidString)-\(BDKDeclarations.fileName)")
            defer { try? FileManager.default.removeItem(at: url) }
            try first.write(to: url)
            #expect(try String(contentsOf: url, encoding: .utf8) == first.jsonString)
        }
    }
}
