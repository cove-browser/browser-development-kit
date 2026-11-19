import BDKTestSupport
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("Profiles")
    struct ProfileTests {
        @Test func createListAndOpen() async throws {
            try await withStubRuntime { runtime in
                #expect(try await runtime.profiles.list().isEmpty)
                let created = try await runtime.profiles.create(name: "Work")
                #expect(created.name == "Work")
                #expect(try await runtime.profiles.list() == [created])
                let profile = try await runtime.profiles.open(id: created.id)
                #expect(profile.id == created.id)
                #expect(profile.name == "Work")
                #expect(profile.kind == .persistent)
                #expect(!profile.isPrivate)
                #expect(profile.isOpen)
                #expect(runtime.profiles.loaded.map(\.id) == [created.id])
                #expect(
                    try profile.info()
                        == BDKProfile.Info(
                            id: created.id, name: "Work", kind: .persistent, parent: nil))
            }
        }

        @Test func openByNameCreatesWhenMissing() async throws {
            try await withStubRuntime { runtime in
                let first = try await runtime.profiles.open("Home")
                try first.close()
                let second = try await runtime.profiles.open("Home")
                #expect(second.id == first.id)
                await expectError(.profile, "notFound") {
                    _ = try await runtime.profiles.open("Nope", createIfMissing: false)
                }
            }
        }

        @Test func savedFactoryKeepsItsProfile() async throws {
            try await withStubRuntime { runtime in
                // Nothing but the factory holds the profile.
                let factory = try await runtime.profiles.open("Work").webContents
                let page = try factory.create()
                #expect(factory.all.map(\.id) == [page.id])
                try factory.profile.close()
                await expectError(.profile, "stale") { _ = try factory.create() }
            }
        }

        @Test func duplicateNameIsRefused() async throws {
            try await withStubRuntime { runtime in
                _ = try await runtime.profiles.create(name: "Same")
                await expectError(.profile, "nameTaken") {
                    _ = try await runtime.profiles.create(name: "Same")
                }
            }
        }

        @Test func unknownProfileIsNotFound() async throws {
            try await withStubRuntime { runtime in
                await expectError(.profile, "notFound") {
                    _ = try await runtime.profiles.open(id: BDKProfile.ID(rawValue: "missing"))
                }
                await expectError(.profile, "notFound") {
                    try await runtime.profiles.delete(BDKProfile.ID(rawValue: "missing"))
                }
            }
        }

        @Test func openingAnOpenProfileIsInUse() async throws {
            try await withProfile { runtime, profile in
                await expectError(.profile, "inUse") {
                    _ = try await runtime.profiles.open(id: profile.id)
                }
                #expect(runtime.profiles.loaded.count == 1)
                await expectError(.profile, "inUse") {
                    try await runtime.profiles.delete(profile.id)
                }
            }
        }

        @Test func closedProfileIsStaleAndCanBeDeleted() async throws {
            try await withProfile { runtime, profile in
                let identifier = profile.id
                try profile.close()
                #expect(!profile.isOpen)
                #expect(runtime.profiles.loaded.isEmpty)
                await expectError(.profile, "stale") { try profile.close() }
                await expectError(.profile, "stale") { _ = try profile.info() }
                await expectError(.profile, "stale") { _ = try profile.webContents.create() }
                try await runtime.profiles.delete(identifier)
                #expect(try await runtime.profiles.list().isEmpty)
            }
        }

        @Test func renameChangesTheNameAndChecksForClashes() async throws {
            try await withProfile { runtime, profile in
                _ = try await runtime.profiles.create(name: "Taken")
                try profile.rename(to: "Renamed")
                #expect(profile.name == "Renamed")
                #expect(try profile.info().name == "Renamed")
                await expectError(.profile, "nameTaken") { try profile.rename(to: "Taken") }
                #expect(profile.name == "Renamed")
            }
        }

        @Test func privateProfilesAreEphemeral() async throws {
            try await withStubRuntime { runtime in
                let priv = try runtime.profiles.openPrivate()
                #expect(priv.isPrivate && priv.kind == .ephemeral)
                #expect(priv.id.rawValue.hasPrefix("ephemeral:"))
                #expect(try await runtime.profiles.list().isEmpty)
                await expectError(.profile, "isPrivate") { try priv.rename(to: "Named") }
                let second = try runtime.profiles.openPrivate()
                #expect(second.id != priv.id)
                try priv.close()
                #expect(!priv.isOpen)
                #expect(runtime.profiles.loaded.map(\.id) == [second.id])
            }
        }

        @Test func closingAProfileStalesItsPages() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                let events = EventReader(page.events)
                try profile.close()
                #expect(!page.isAlive)
                #expect(await events.next() == nil)
                await expectError(.contents, "stale") { try page.navigation.stop() }
            }
        }

        @Test func cancellingTheTaskCancelsTheCall() async throws {
            try await withStubRuntime { runtime in
                let task = Task { @MainActor in try await runtime.profiles.list() }
                task.cancel()
                await expectError(.broker, "cancelled") { _ = try await task.value }
                // The runtime is still healthy.
                let remaining = try await runtime.profiles.list()
                #expect(remaining.isEmpty)
            }
        }

        @Test func eachCallResumesExactlyOnceUnderLoad() async throws {
            try await withStubRuntime { runtime in
                let names = (0..<20).map { "Profile \($0)" }
                let tasks = names.map { name in
                    Task { @MainActor in try await runtime.profiles.create(name: name).name }
                }
                var created: [String] = []
                for task in tasks { created.append(try await task.value) }
                #expect(Set(created) == Set(names))
                #expect(try await runtime.profiles.list().count == 20)
            }
        }
    }
}
