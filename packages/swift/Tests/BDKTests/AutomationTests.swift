import BDKTestSupport
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    /// Sessions, tasks and audit events (T-AUT-02, T-AUT-03), against the stub's scripted host. The
    /// stub runs no JavaScript: the test plays the script, in the wire protocol.
    @MainActor
    @Suite("Automation")
    struct AutomationTests {
        // MARK: Sessions

        @Test func sessionOptionsAreChecked() async throws {
            try await withStubRuntime(automationConfiguration()) { runtime in
                await expectError(.capability, "denied") {
                    _ = try runtime.automation.makeSession(
                        .init(name: "a", capabilities: ["passwords.reveal"]))
                }
                await expectError(.capability, "denied") {
                    _ = try runtime.automation.makeSession(
                        .init(name: "a", capabilities: ["history.*"]))
                }
                await expectError(.broker, "invalidArguments") {
                    _ = try runtime.automation.makeSession(.init(name: ""))
                }
                let session = try runtime.automation.makeSession(
                    .init(name: "agent", capabilities: ["history.read", "app.notes.*"]))
                #expect(session.name == "agent")
                #expect(session.capabilities == ["history.read", "app.notes.*"])
                #expect(!session.isRevoked)
            }
        }

        @Test func aSessionCanBeLimitedToProfiles() async throws {
            try await withProfile { runtime, profile in
                let session = try runtime.automation.makeSession(
                    .init(name: "scoped", scope: .init(profiles: [profile], contents: .created)))
                session.revoke()
                try profile.close()
                await expectError(.profile, "stale") {
                    _ = try runtime.automation.makeSession(
                        .init(name: "scoped", scope: .init(profiles: [profile])))
                }
            }
        }

        @Test func anExplicitScopeNamesItsPages() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                let session = try runtime.automation.makeSession(
                    .init(
                        name: "explicit", capabilities: ["contents.read"],
                        scope: .init(contents: .explicit([page]))))
                let other = try profile.webContents.create()
                try session.grant(contents: other)
                session.revoke()
                #expect(session.isRevoked)
                #expect(throws: BDKError.self) { try session.grant(contents: page) }
            }
        }

        // MARK: Running

        @Test func runNeedsAutomationEnabledAndAListedScript() async throws {
            try await withStubRuntime { runtime in
                let session = try runtime.automation.makeSession(.init(name: "agent"))
                await expectError(.automation, "disabled") {
                    _ = try session.run("echo")
                }
            }
            try await withStubRuntime(automationConfiguration()) { runtime in
                let session = try runtime.automation.makeSession(.init(name: "agent"))
                await expectError(.automation, "scriptNotFound") {
                    _ = try session.run("nope")
                }
            }
        }

        @Test func aTaskReportsItsEventsAndFinishesWithTheScriptsResult() async throws {
            try await withStubRuntime(automationConfiguration()) { runtime in
                let script = try runtime.stub.defineScript("echo")
                let session = try runtime.automation.makeSession(
                    .init(name: "agent", capabilities: ["runtime.read"]))
                let task = try session.run(
                    "echo", arguments: .object(["url": .string("https://example.com/")]))
                let events = EventReader(task.events)
                try await waitUntil("the start frame") { script.frames.count == 1 }
                let start = try #require(script.last)
                #expect(start["type"] == .string("start"))
                #expect(start["arguments", "url"] == .string("https://example.com/"))
                #expect(start["session", "name"] == .string("agent"))

                try script.send(
                    #"{"type":"call","id":1,"command":"runtime.caller","args":{}}"#)
                try await waitUntil("the result") { !script.frames(ofType: "result").isEmpty }
                #expect(script.frames(ofType: "result")[0]["value", "kind"] == .string("session"))
                // A call the session has no capability for is refused, and audited all the same.
                try script.send(#"{"type":"call","id":2,"command":"history.search","args":{}}"#)
                try await waitUntil("the refusal") { !script.frames(ofType: "error").isEmpty }
                #expect(
                    script.frames(ofType: "error")[0]["error", "code"] == .string("denied"))

                try script.send(#"{"type":"progress","fraction":0.5,"message":"half"}"#)
                try script.send(#"{"type":"log","level":"warn","message":"careful"}"#)
                try script.send(#"{"type":"done","result":{"title":"Example"}}"#)

                let value = try await task.value
                #expect(value == .object(["title": .string("Example")]))
                let seen = await events.drain { _ in false }
                #expect(seen.count == 4)
                #expect(seen[0] == .commandInvoked("runtime.caller", error: nil))
                guard case .commandInvoked("history.search", let failure?) = seen[1] else {
                    Issue.record("expected the refused call, got \(seen)")
                    return
                }
                #expect(failure.domain == .capability && failure.code == "denied")
                #expect(seen[2] == .progress(0.5, "half"))
                #expect(seen[3] == .log(.warn, "careful"))
                // A late reader of a finished task gets the value at once.
                #expect(try await task.value == value)
                #expect(try runtime.stub.automationCount(.ended) == 1)
            }
        }

        @Test func aScriptThatThrowsFailsTheTask() async throws {
            try await withStubRuntime(automationConfiguration()) { runtime in
                let script = try runtime.stub.defineScript("echo")
                let session = try runtime.automation.makeSession(.init(name: "agent"))
                let task = try session.run("echo")
                try await waitUntil("the start frame") { !script.frames.isEmpty }
                try script.send(#"{"type":"failed","error":{"message":"boom"}}"#)
                do {
                    _ = try await task.value
                    Issue.record("the task should fail")
                } catch let error as BDKError {
                    #expect(error.domain == .automation && error.code == "scriptFailed")
                    #expect(error.message == "boom")
                }
            }
        }

        @Test func cancelEndsTheTask() async throws {
            try await withStubRuntime(automationConfiguration()) { runtime in
                let script = try runtime.stub.defineScript("echo")
                let session = try runtime.automation.makeSession(.init(name: "agent"))
                let task = try session.run("echo")
                try await waitUntil("the start frame") { !script.frames.isEmpty }
                task.cancel()
                await expectError(.broker, "cancelled") { _ = try await task.value }
                #expect(script.frames(ofType: "abort").count == 1)
                task.cancel()  // finished: nothing more happens
            }
        }

        @Test func revokingTheSessionEndsItsTasks() async throws {
            try await withStubRuntime(automationConfiguration()) { runtime in
                let script = try runtime.stub.defineScript("echo")
                let session = try runtime.automation.makeSession(.init(name: "agent"))
                let task = try session.run("echo")
                try await waitUntil("the start frame") { !script.frames.isEmpty }
                session.revoke()
                #expect(session.isRevoked)
                await expectError(.automation, "sessionRevoked") { _ = try await task.value }
                await expectError(.automation, "sessionRevoked") { _ = try session.run("echo") }
                session.revoke()  // already revoked: nothing happens
            }
        }

        @Test func aHostCrashEndsEveryTask() async throws {
            try await withStubRuntime(automationConfiguration()) { runtime in
                let script = try runtime.stub.defineScript("echo")
                let session = try runtime.automation.makeSession(.init(name: "agent"))
                let task = try session.run("echo")
                try await waitUntil("the start frame") { !script.frames.isEmpty }
                try runtime.stub.crashAutomationHost()
                await expectError(.automation, "hostGone") { _ = try await task.value }
            }
        }

        // MARK: Audit events

        @Test func sessionsAndCommandsAreAudited() async throws {
            try await withStubRuntime(automationConfiguration()) { runtime in
                let script = try runtime.stub.defineScript("echo")
                let events = EventReader(runtime.automation.events)
                let session = try runtime.automation.makeSession(
                    .init(name: "agent", capabilities: ["runtime.read"]))
                let task = try session.run("echo")
                try await waitUntil("the start frame") { !script.frames.isEmpty }
                try script.send(#"{"type":"call","id":1,"command":"runtime.caller"}"#)
                try await waitUntil("the result") { !script.frames(ofType: "result").isEmpty }
                session.revoke()
                _ = try? await task.value

                let created = try await events.next {
                    if case .sessionCreated = $0 { true } else { false }
                }
                guard case .sessionCreated(let info) = created else { return }
                #expect(info == BDKSessionInfo(name: "agent", capabilities: ["runtime.read"]))
                let invoked = try await events.next {
                    if case .commandInvoked = $0 { true } else { false }
                }
                guard case .commandInvoked(let record) = invoked else { return }
                #expect(record.session == "agent" && record.command == "runtime.caller")
                #expect(record.task != nil && record.error == nil)
                let revoked = try await events.next {
                    if case .sessionRevoked = $0 { true } else { false }
                }
                guard case .sessionRevoked(let ended) = revoked else { return }
                #expect(ended.name == "agent")
            }
        }

        @Test func aPageASessionCreatesIsDeliveredAndKept() async throws {
            try await withStubRuntime(automationConfiguration()) { runtime in
                let profile = try await runtime.profiles.open("Test")
                let script = try runtime.stub.defineScript("echo")
                let session = try runtime.automation.makeSession(
                    .init(name: "agent", capabilities: ["contents.read", "contents.control"]))
                let events = EventReader(runtime.automation.events)
                _ = try session.run("echo")
                try await waitUntil("the start frame") { !script.frames.isEmpty }
                try script.send(
                    """
                    {"type":"call","id":1,"command":"contents.create","args":{},\
                    "profile":"\(profile.id.rawValue)"}
                    """)
                let created = try await events.next {
                    if case .contentsCreated = $0 { true } else { false }
                }
                guard case .contentsCreated(let page, let creator) = created else { return }
                #expect(creator == "agent")
                #expect(page.profile === profile && page.isAlive)
                #expect(runtime.webContents.contains { $0 === page })

                // The runtime holds the page until it closes, and then ends it.
                try script.send(
                    """
                    {"type":"call","id":2,"command":"contents.close","contents":\
                    "\(page.id.rawValue)"}
                    """)
                try await waitUntil("the page to end") { !page.isAlive }
                #expect(page.isClosed)
                // A page the application made is not announced.
                let own = try profile.webContents.create()
                #expect(own.isAlive)
            }
        }
    }
}
