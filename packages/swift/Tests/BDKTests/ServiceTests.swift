import BDKTestSupport
import Foundation
import Testing

@testable import BDK

/// The generated wrappers (`Generated/BDKServices.swift`) against the stub's in-memory services.
extension BDKStubTests {
    @MainActor
    @Suite("Service commands")
    struct ServiceTests {
        private func url(_ text: String) -> URL {
            URL(string: text) ?? URL(fileURLWithPath: "/")
        }

        // MARK: Settings

        @Test func settingsGetSetAndReset() async throws {
            try await withProfile { runtime, profile in
                #expect(try await profile.settings.get("ui.density") == .null)
                try await profile.settings.set("ui.density", value: .string("compact"))
                #expect(try await profile.settings.get("ui.density") == .string("compact"))
                try await profile.settings.set("ui.zoom", 1.25)
                try await profile.settings.set("ui.tabs", ["a", "b"])
                let many = try await profile.settings.getMany(keys: ["ui.density", "ui.zoom", "x"])
                #expect(
                    many
                        == .object([
                            "ui.density": .string("compact"), "ui.zoom": .number(1.25),
                            "x": .null,
                        ]))
                #expect(try await profile.settings.get("ui.zoom", as: Double.self) == 1.25)
                #expect(try await profile.settings.get("ui.tabs", as: [String].self) == ["a", "b"])

                // Application scope is a different store from the profile's.
                #expect(try await runtime.settings.get("ui.density") == .null)

                try await profile.settings.reset("ui.density")
                #expect(try await profile.settings.get("ui.density") == .null)
                await expectError(.runtime, "malformedResult") {
                    _ = try await profile.settings.get("ui.zoom", as: String.self)
                }
            }
        }

        // MARK: History

        @Test func historyAddSearchAndDelete() async throws {
            try await withProfile { _, profile in
                let base = Date(timeIntervalSince1970: 1_700_000_000.5)
                for index in 0..<5 {
                    try await profile.history.add(
                        url: url("https://example.test/\(index)"), title: "Page \(index)",
                        time: base.addingTimeInterval(Double(index) * 60))
                }
                try await profile.history.add(url: url("https://other.test/"), title: "Other")

                let first = try await profile.history.search("example", limit: 2)
                #expect(first.items.count == 2)
                #expect(first.items.map(\.title) == ["Page 4", "Page 3"])
                #expect(first.items[0].lastVisit == base.addingTimeInterval(240))
                let cursor = try #require(first.nextCursor)

                let second = try await profile.history.search("example", limit: 2, cursor: cursor)
                #expect(second.items.map(\.title) == ["Page 2", "Page 1"])
                let third = try await profile.history.search(
                    "example", limit: 2, cursor: second.nextCursor)
                #expect(third.items.map(\.title) == ["Page 0"])
                #expect(third.nextCursor == nil)

                let window = try await profile.history.search(
                    in: BDKTimeRange(
                        start: base.addingTimeInterval(60), end: base.addingTimeInterval(120)))
                #expect(window.items.map(\.title) == ["Page 2", "Page 1"])

                let visits = try await profile.history.visits(url: url("https://example.test/1"))
                #expect(visits.count == 1)

                try await profile.history.delete(urls: [url("https://example.test/4")])
                try await profile.history.deleteRange(
                    range: BDKTimeRange(end: base.addingTimeInterval(60)))
                let rest = try await profile.history.search("example")
                #expect(rest.items.map(\.title) == ["Page 3", "Page 2"])

                try await profile.history.deleteAll()
                #expect(try await profile.history.search().items.isEmpty)
            }
        }

        @Test func aBadCursorIsInvalidArguments() async throws {
            try await withProfile { _, profile in
                await expectError(.broker, "invalidArguments") {
                    _ = try await profile.history.search(cursor: "nope")
                }
            }
        }

        // MARK: Bookmarks

        @Test func bookmarksFoldersCreateMoveAndRemove() async throws {
            try await withProfile { _, profile in
                let roots = try await profile.bookmarks.roots()
                #expect(roots.map(\.id) == ["bar", "other"])
                let bar = roots[0]
                let other = roots[1]

                let folder = try await profile.bookmarks.createFolder(
                    "Reading", in: bar.id, at: 0)
                #expect(folder.isFolder && folder.url == nil && folder.parentId == bar.id)
                let node = try await profile.bookmarks.create(
                    url: url("https://swift.test/"), title: "Swift", in: folder.id)
                #expect(node.title == "Swift" && node.url == url("https://swift.test/"))
                #expect(try await profile.bookmarks.children(folder.id).map(\.id) == [node.id])
                #expect(
                    try await profile.bookmarks.forUrl(url: url("https://swift.test/")).map(\.id)
                        == [node.id])
                #expect(try await profile.bookmarks.search(query: "swi").map(\.id) == [node.id])

                let renamed = try await profile.bookmarks.update(node.id, title: "Swift.org")
                #expect(renamed.title == "Swift.org")

                let moved = try await profile.bookmarks.move(node.id, to: other.id, at: 0)
                #expect(moved.parentId == other.id)
                #expect(try await profile.bookmarks.children(folder.id).isEmpty)

                await expectError(.broker, "invalidArguments") {
                    _ = try await profile.bookmarks.move(folder.id, to: folder.id)
                }

                try await profile.bookmarks.remove(folder.id)
                await expectError(.broker, "invalidArguments") {
                    _ = try await profile.bookmarks.get(folder.id)
                }
                await expectError(.broker, "invalidArguments") {
                    try await profile.bookmarks.remove("bar")
                }
            }
        }

        // MARK: Permissions

        @Test func permissionsGetAndSet() async throws {
            try await withProfile { _, profile in
                let site = url("https://site.test/")
                let unset = try await profile.permissions.get(origin: site, kind: "camera")
                #expect(unset.value == .ask && unset.source == .default)

                try await profile.permissions.set(origin: site, kind: "camera", value: .block)
                let blocked = try await profile.permissions.get(origin: site, kind: "camera")
                #expect(blocked.value == .block && blocked.source == .user)

                try await profile.permissions.setDefault(kind: "camera", value: .allow)
                #expect(try await profile.permissions.getDefault(kind: "camera") == .allow)
                #expect(
                    try await profile.permissions.get(origin: site, kind: "camera").value == .block)

                try await profile.permissions.reset(origin: site, kind: "camera")
                #expect(
                    try await profile.permissions.get(origin: site, kind: "camera").value == .allow)
            }
        }

        // MARK: Passwords

        @Test func passwordsAddListAndReveal() async throws {
            try await withProfile { runtime, profile in
                let saved = try await profile.passwords.add(
                    origin: url("https://login.test/"), username: "ada", password: "hunter2")
                let listed = try await profile.passwords.list()
                #expect(listed == [saved])
                #expect(saved.username == "ada" && saved.lastUsed == nil)

                // Reading a password in clear asks the operating system, whoever calls.
                await expectError(.capability, "reauthenticationFailed") {
                    _ = try await profile.passwords.reveal(saved.id)
                }
                try runtime.stub.setAuthentication(.accept)
                #expect(try await profile.passwords.reveal(saved.id) == "hunter2")
                #expect(try runtime.stub.serviceCount(.authentications) == 2)

                let updated = try await profile.passwords.update(saved.id, username: "grace")
                #expect(updated.username == "grace")
                try await profile.passwords.remove(saved.id)
                #expect(try await profile.passwords.list().isEmpty)
            }
        }

        // MARK: Privacy

        @Test func privacyClearReportsProgress() async throws {
            try await withProfile { _, profile in
                try await profile.history.add(url: url("https://a.test/"))
                var reports: [String] = []
                try await profile.privacy.clear(types: [.history, .cookies]) { fraction, message in
                    reports.append("\(fraction) \(message)")
                }
                #expect(reports.last == "0.5 halfway")
                #expect(try await profile.history.search().items.isEmpty)
            }
        }

        @Test func cancellingTheTaskCancelsTheCall() async throws {
            try await withProfile { runtime, profile in
                try runtime.stub.holdServices(true)
                let task = Task { @MainActor in
                    try await profile.privacy.clear(types: [.history])
                }
                while try runtime.stub.serviceCount(.held) == 0 {
                    try await Task.sleep(for: .milliseconds(5))
                }
                task.cancel()
                await expectError(.broker, "cancelled") { try await task.value }
                #expect(try runtime.stub.serviceCount(.stopped) == 1)
                try runtime.stub.holdServices(false)
            }
        }

        // MARK: Errors, groups and the rest

        @Test func aCommandTheStubLacksIsNotImplemented() async throws {
            try await withProfile { _, profile in
                await expectError(.broker, "notImplemented") {
                    try await profile.downloads.pause("d1")
                }
            }
        }

        @Test func aClosedProfileIsStale() async throws {
            try await withProfile { _, profile in
                let history = profile.history
                try profile.close()
                await expectError(.profile, "stale") { _ = try await history.search() }
            }
        }

        @Test func groupsAreNested() async throws {
            try await withProfile { _, profile in
                let address = try await profile.autofill.addresses.add(
                    fields: BDKAddressFields(fullName: "Ada Lovelace", city: "London"))
                #expect(address.fields.city == "London")
                #expect(try await profile.autofill.addresses.list() == [address])
                try await profile.autofill.addresses.remove(address.id)
                #expect(try await profile.autofill.addresses.list().isEmpty)
            }
        }

        @Test func applicationScopedServices() async throws {
            try await withStubRuntime { runtime in
                let info = try await runtime.about.info()
                #expect(info.browserName == "BDKStub" && info.internalScheme == "bdk")
                let status = try await runtime.system.defaultBrowser.status()
                #expect(status.canBeDefault && !status.isDefault)
            }
        }

        // MARK: Events

        @Test func eventsDecodeByName() throws {
            let visited = try BDKHistory.Event.decode(
                name: "history.visited",
                payload: """
                    {"entry":{"url":"https://a.test/","title":"A","lastVisit":1700000000000,\
                    "visitCount":2,"typedCount":0},"source":"page"}
                    """)
            guard case .visited(let payload) = visited else {
                Issue.record("expected .visited, got \(String(describing: visited))")
                return
            }
            #expect(payload.entry.visitCount == 2)
            #expect(payload.entry.lastVisit == Date(timeIntervalSince1970: 1_700_000_000))
            #expect(payload.source == .page)

            let changed = try BDKSettings.Event.decode(
                name: "settings.changed",
                payload:
                    #"{"key":"k","oldValue":null,"newValue":{"a":[1]},"source":"user"}"#)
            #expect(
                changed
                    == .changed(
                        .init(
                            key: "k", oldValue: .null,
                            newValue: .object(["a": .array([.number(1)])]), source: .user)))

            #expect(
                try BDKPasswords.Event.decode(name: "passwords.issuesChanged", payload: "{}")
                    == .issuesChanged)
            #expect(try BDKHistory.Event.decode(name: "history.future", payload: "{}") == nil)
        }

        @Test func anEventStreamEndsWithTheRuntime() async throws {
            let runtime = try BDKRuntime.startStub()
            let profile = try await runtime.profiles.open("Events")
            // The runtime accepts a subscription to each event the wrapper subscribes to.
            for name in BDKHistory.Event.names {
                _ = try runtime.commands.events(named: name, profile: profile)
            }
            let events = profile.history.events
            let counted = Task { @MainActor in
                var count = 0
                for await _ in events { count += 1 }
                return count
            }
            await runtime.shutdown()
            #expect(await counted.value == 0)
        }
    }
}
