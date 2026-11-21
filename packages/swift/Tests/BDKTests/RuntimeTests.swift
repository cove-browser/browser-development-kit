import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("Runtime")
    struct RuntimeTests {
        @Test func abiConstantsComeFromTheHeaders() {
            #expect(HostABI.current.version == UInt32(BDK_ABI_VERSION))
            #expect(HostABI.current.hash == BDK_ABI_HASH)
            #expect(HostABI.current.hash.count == 64)
        }

        @Test func startsRunsAndShutsDown() async throws {
            #expect(!BDKRuntime.isRunning)
            let runtime = try BDKRuntime.startStub()
            #expect(BDKRuntime.isRunning)
            #expect(BDKRuntime.shared === runtime)
            #expect(runtime.areRegistrationsFrozen)
            #expect(runtime.info.abiVersion == UInt32(BDK_ABI_VERSION))
            #expect(runtime.info.abiHash == BDK_ABI_HASH)
            await runtime.shutdown()
            #expect(!BDKRuntime.isRunning)
            await runtime.shutdown()  // a second shutdown does nothing
        }

        @Test func aStartThatFailsAtTheFreezeLeavesNothingRunning() async throws {
            let configuration = BDKConfiguration(
                resolvedJSON: "{\"surfaces\":{\"find\":\"custom\"}}")
            await expectError(.config, "handlerMissing") { try BDKRuntime.startStub(configuration) }
            #expect(!BDKRuntime.isRunning)
            // The native runtime shuts down too: a correct start succeeds once it has.
            for _ in 0..<200 {
                do {
                    let runtime = try BDKRuntime.startStub()
                    await runtime.shutdown()
                    return
                } catch let error as BDKError where error.code == "alreadyStarted" {
                    try await Task.sleep(for: .milliseconds(10))
                }
            }
            Issue.record("the runtime of the failed start never shut down")
        }

        @Test func secondStartFailsWithAlreadyStarted() async throws {
            try await withStubRuntime { _ in
                await expectError(.runtime, "alreadyStarted") { try BDKRuntime.startStub() }
            }
        }

        @Test func versionMismatchNamesBothVersions() async throws {
            let host = HostABI(version: 99, hash: HostABI.current.hash)
            do {
                try BDKRuntime.startRuntime(.stub, host: host)
                Issue.record("start should fail")
            } catch let error as BDKError {
                #expect(error.domain == .abi && error.code == "versionMismatch")
                #expect(error.message.contains("version 99"))
                #expect(error.message.contains("version \(BDK_ABI_VERSION)"))
                #expect(error.details["hostVersion"] == .number(99))
                #expect(error.details["runtimeVersion"] == .number(Double(BDK_ABI_VERSION)))
            }
            #expect(!BDKRuntime.isRunning)
        }

        @Test func hashMismatchNamesBothHashes() async throws {
            let wrong = String(repeating: "ab", count: 32)
            do {
                try BDKRuntime.startRuntime(
                    .stub, host: HostABI(version: HostABI.current.version, hash: wrong))
                Issue.record("start should fail")
            } catch let error as BDKError {
                #expect(error.domain == .abi && error.code == "hashMismatch")
                #expect(error.message.contains(wrong))
                #expect(error.message.contains(BDK_ABI_HASH))
            }
            #expect(!BDKRuntime.isRunning)
            // Nothing is left running: a correct start still works.
            let runtime = try BDKRuntime.startStub()
            await runtime.shutdown()
        }

        @Test func invalidResolvedConfigurationIsRejected() async throws {
            await expectError(.config, "resolvedInvalid") {
                try BDKRuntime.startStub(BDKConfiguration(resolvedJSON: "[1]"))
            }
            await expectError(.config, "resolvedMissing") {
                try BDKRuntime.startStub(BDKConfiguration(resolvedJSON: ""))
            }
        }

        @Test func freezingTwiceFailsWithRegistrationAfterStart() async throws {
            try await withStubRuntime(freezingRegistrations: false) { runtime in
                #expect(!runtime.areRegistrationsFrozen)
                try runtime.freezeRegistrations()
                #expect(runtime.areRegistrationsFrozen)
                await expectError(.config, "registrationAfterStart") {
                    try runtime.freezeRegistrations()
                }
            }
        }

        @Test func registrationAfterFreezeIsRefused() async throws {
            try await withStubRuntime { runtime in
                var error: bdk_error_t = 0
                let status = bdk_ui_set_handler(
                    runtime.handle, BDK_SURFACE_PERMISSIONS, { _, _ in BDK_UI_USE_DEFAULT }, nil,
                    nil,
                    &error)
                #expect(status == BDK_ERR_FAILED)
                #expect(swiftString(bdk_error_code(error)) == "registrationAfterStart")
                bdk_error_release(error)
            }
        }

        @Test func contentsNeedFrozenRegistrations() async throws {
            try await withStubRuntime(freezingRegistrations: false) { runtime in
                let profile = try await runtime.profiles.open("Test")
                await expectError(.runtime, "notStarted") { _ = try profile.webContents.create() }
                try runtime.freezeRegistrations()
                _ = try profile.webContents.create()
            }
        }

        @Test func runtimeEventsReachEveryConsumer() async throws {
            try await withStubRuntime(freezingRegistrations: false) { runtime in
                let first = EventReader(runtime.events)
                let second = EventReader(runtime.events)
                try runtime.freezeRegistrations()
                let profile = try await runtime.profiles.open("Events")
                let id = profile.id
                try profile.close()
                runtime.stub.pump()
                let expected: [BDKRuntime.Event] = [
                    .registrationsFrozen, .profileOpened(id), .profileClosed(id),
                ]
                var seenFirst: [BDKRuntime.Event] = []
                var seenSecond: [BDKRuntime.Event] = []
                while seenFirst.count < expected.count, let event = await first.next() {
                    seenFirst.append(event)
                }
                while seenSecond.count < expected.count, let event = await second.next() {
                    seenSecond.append(event)
                }
                #expect(seenFirst == expected)
                #expect(seenSecond == expected)
            }
        }

        @Test func shutdownEndsEventSequencesAndStalesEverything() async throws {
            let runtime = try BDKRuntime.startStub()
            let profile = try await runtime.profiles.open("Doomed")
            let page = try profile.webContents.create()
            let runtimeEvents = EventReader(runtime.events)
            let pageEvents = EventReader(page.events)
            await runtime.shutdown()
            let seen = await runtimeEvents.drain { _ in false }
            #expect(seen.contains(.shutdownStarted))
            #expect(await pageEvents.next() == nil)
            #expect(!profile.isOpen)
            #expect(!page.isAlive)
            await expectError(.contents, "stale") { try page.navigation.reload() }
            await expectError(.profile, "stale") { try profile.rename(to: "Other") }
            await expectError(.runtime, "stale") { _ = try await runtime.profiles.list() }
            // Reading a stale page returns the last values and does not trap.
            #expect(page.title == "")
        }

        @Test func shutdownCancelsPendingCalls() async throws {
            let runtime = try BDKRuntime.startStub()
            let pending = Task { @MainActor in try await runtime.profiles.list() }
            await Task.yield()
            await runtime.shutdown()
            do {
                _ = try await pending.value
            } catch let error as BDKError {
                #expect(error.code == "cancelled" || error.code == "stale")
            }
        }

        @Test func terminateRunsShutdownThenCompletes() async throws {
            let runtime = try BDKRuntime.startStub()
            await withCheckedContinuation { continuation in
                runtime.terminate { continuation.resume() }
            }
            #expect(!BDKRuntime.isRunning)
        }

        /// Starts a shutdown and returns once it is in progress. The stub answers the shutdown only
        /// when it is pumped, so with pumping paused the runtime stays shutting down for `body`
        /// however the tasks are scheduled.
        private func duringShutdown(
            of runtime: BDKRuntime, _ body: () async -> Void
        ) async -> Task<Void, Never> {
            await runtime.stub.withAutomaticPumpingPaused {
                let first = Task { @MainActor in await runtime.shutdown() }
                while runtime.state == .running { await Task.yield() }
                await body()
                return first
            }
        }

        @Test func shutdownDuringShutdownWaitsForIt() async throws {
            let runtime = try BDKRuntime.startStub()
            var second: Task<Void, Never>?
            let first = await duringShutdown(of: runtime) {
                #expect(runtime.state == .shuttingDown)
                second = Task { @MainActor in await runtime.shutdown() }
            }
            await second?.value
            #expect(runtime.state == .stopped)
            #expect(!BDKRuntime.isRunning)
            await first.value
        }

        @Test func quitDuringShutdownWaitsForIt() async throws {
            let runtime = try BDKRuntime.startStub()
            let (askedAgain, ask) = AsyncStream<Bool>.makeStream()
            let first = await duringShutdown(of: runtime) {
                let reply = runtime.terminateReply {
                    ask.yield(runtime.state == .stopped)
                    ask.finish()
                }
                #expect(reply == .terminateCancel)
                // Any other reply never asks again; end the stream rather than wait for ever.
                if reply != .terminateCancel { ask.finish() }
            }
            var answers = askedAgain.makeAsyncIterator()
            #expect(await answers.next() == true)
            #expect(runtime.terminateReply {} == .terminateNow)
            await first.value
        }

        @Test func loadsTheResolvedConfigurationFromABundle() throws {
            let directory = FileManager.default.temporaryDirectory
                .appendingPathComponent("bdk-\(UUID().uuidString).bundle")
            let resources = directory.appendingPathComponent("Contents/Resources")
            try FileManager.default.createDirectory(
                at: resources, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: directory) }
            try Data("{\"browser\":{}}".utf8).write(
                to: resources.appendingPathComponent(BDKConfiguration.resolvedFileName))
            try Data(
                "<plist version=\"1.0\"><dict><key>CFBundleIdentifier</key><string>test.bdk</string></dict></plist>"
                    .utf8
            ).write(to: directory.appendingPathComponent("Contents/Info.plist"))
            let bundle = try #require(Bundle(url: directory))
            let configuration = try BDKConfiguration.load(from: bundle)
            #expect(configuration.resolvedJSON == "{\"browser\":{}}")
            #expect(configuration.bundleURL == bundle.bundleURL)
        }

        @Test func aBundleWithoutTheResolvedFileFails() async {
            await expectError(.config, "resolvedMissing") {
                _ = try BDKConfiguration.load(from: Bundle(for: BundleToken.self))
            }
        }

        @Test func applicationClassIsOpenAndSubclassable() {
            #expect(CustomApplication.superclass() === BDKApplication.self)
            // The runtime's base class sits in between; with the stub it is a plain NSApplication subclass.
            #expect(BDKApplication.superclass() === BDKApplicationBase.self)
            #expect(BDKApplicationBase.superclass() === NSApplication.self)
        }
    }

    private final class BundleToken {}

    private final class CustomApplication: BDKApplication {}
}
