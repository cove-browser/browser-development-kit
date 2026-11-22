import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    /// The provider bridge and `page.translation` (T-PRV-01, T-PRV-03, T-PRV-09 on the Swift side), against the
    /// stub's providers and page text. The engine runs the same behaviour on real pages.
    @MainActor
    @Suite("Providers")
    struct ProviderTests {
        // MARK: Starting

        /// A failed start shuts the native runtime down in the background; waits for that.
        private func awaitStoppedRuntime() async throws {
            for _ in 0..<200 {
                do {
                    try await BDKRuntime.startStub().shutdown()
                    return
                } catch let error as BDKError where error.code == "alreadyStarted" {
                    try await Task.sleep(for: .milliseconds(10))
                }
            }
            Issue.record("the runtime of the failed start never shut down")
        }

        @Test func anAppServiceWithNoProviderFailsTheStart() async throws {
            let missing = try providerConfiguration(services: appTranslation)
            await expectError(.config, "providerMissing") { try BDKRuntime.startStub(missing) }
            #expect(!BDKRuntime.isRunning)
            try await awaitStoppedRuntime()
        }

        @Test func aProviderForAServiceThatIsNotAppIsRefused() async throws {
            let wrong = try providerConfiguration {
                $0.services.translation.register(FakeTranslator())
            }
            await expectError(.config, "providerNotApp") { try BDKRuntime.startStub(wrong) }
            try await awaitStoppedRuntime()
        }

        @Test func aRemoteProviderNeedsAPolicyURL() async throws {
            let cloud = BDKProviderDescriptor(
                id: "app.cloud", name: "Cloud", privacy: .remote, host: "mt.example.com")
            let invalid = try providerConfiguration(services: appTranslation) {
                $0.services.translation.register(FakeTranslator(descriptor: cloud))
            }
            await expectError(.config, "descriptorInvalid") { try BDKRuntime.startStub(invalid) }
            try await awaitStoppedRuntime()
        }

        @Test func aGoogleProviderNeedsTheKeyAndTheKeyNeedsAGoogleProvider() async throws {
            let google = "\"geolocation\":{\"provider\":\"google\"}"
            let keyless = try providerConfiguration(services: google)
            await expectError(.config, "keyMissing") { try BDKRuntime.startStub(keyless) }
            try await awaitStoppedRuntime()

            let unused = try providerConfiguration { $0.services.google.apiKey = "AIzaTestKey" }
            await expectError(.config, "keyUnused") { try BDKRuntime.startStub(unused) }
            try await awaitStoppedRuntime()

            let keyed = try providerConfiguration(services: google) {
                $0.services.google.apiKey = "AIzaTestKey"
            }
            // Chromium takes the key once per process, so only the stub starts with one here.
            try await withStubRuntime(keyed) { runtime in
                #expect(runtime.areRegistrationsFrozen)
            }
        }

        // MARK: Translation

        @Test func aSwiftProviderTranslatesAPage() async throws {
            let services = try providerConfiguration(services: appTranslation) {
                $0.services.translation.register(FakeTranslator())
            }
            try await withStubRuntime(services) { runtime in
                let profile = try await runtime.profiles.open("Translation")
                let provider = try await profile.translation.provider()
                #expect(provider.id == "app.fake" && provider.privacy == .onDevice)
                let page = try profile.webContents.create()
                let events = EventReader(page.events)
                try runtime.stub.setPageText(
                    ["bonjour", "  ", "le monde"], language: "fr", on: page)
                runtime.stub.pump()
                let available = try await page.translation.state()
                #expect(available == BDKTranslationState(phase: .available, detected: "fr"))

                let state = try await page.translation.translate()
                #expect(state.phase == .translated && state.from == "fr" && state.to == "en")
                #expect(try runtime.stub.pageText(of: page) == ["BONJOUR", "  ", "LE MONDE"])
                let translated = try await events.next {
                    if case .translation(let state) = $0 { return state.phase == .translated }
                    return false
                }
                #expect(translated == .translation(state))

                try await page.translation.revert()
                #expect(try runtime.stub.pageText(of: page) == ["bonjour", "  ", "le monde"])
                #expect(try await page.translation.state().phase == .available)
            }
        }

        @Test func theOfferReachesTheHandlerAndItsAnswerTranslates() async throws {
            let box = UIBox()
            let services = try providerConfiguration(services: appTranslation) {
                $0.services.translation.register(FakeTranslator())
                try $0.ui.translation.setHandler { request in
                    box.request = request
                    return .handled
                }
            }
            try await withStubRuntime(services) { runtime in
                let profile = try await runtime.profiles.open("Translation")
                let page = try profile.webContents.create()
                try runtime.stub.setPageText(["bonjour"], language: "fr", on: page)
                try await settle(runtime) { box.request != nil }
                let offer = try #require(box.request as? TranslationRequest)
                #expect(offer.phase == .offer && offer.detected == "fr" && offer.target == "en")
                #expect(offer.provider?.id == "app.fake")
                #expect(offer.languages.contains(.init(from: "fr", to: "en", installed: true)))
                #expect(offer.contents === page)

                offer.translate()
                try await settle(runtime) { try runtime.stub.pageText(of: page) == ["BONJOUR"] }

                box.request = nil
                try await page.translation.present()
                try await settle(runtime) { box.request != nil }
                let shown = try #require(box.request as? TranslationRequest)
                #expect(shown.phase == .translated)
                shown.revert()
                try await settle(runtime) { try runtime.stub.pageText(of: page) == ["bonjour"] }
            }
        }

        @Test func neverForAnOfferIsRememberedByThePreferences() async throws {
            let box = UIBox()
            let services = try providerConfiguration(services: appTranslation) {
                $0.services.translation.register(FakeTranslator())
                try $0.ui.translation.setHandler { request in
                    box.request = request
                    return .handled
                }
            }
            try await withStubRuntime(services) { runtime in
                let profile = try await runtime.profiles.open("Translation")
                let page = try profile.webContents.create()
                try runtime.stub.setPageText(["bonjour"], language: "fr", on: page)
                try await settle(runtime) { box.request != nil }
                let offer = try #require(box.request as? TranslationRequest)
                offer.never(for: "fr")
                runtime.stub.pump()
                let preferences = try await profile.translation.preferences()
                #expect(preferences.neverTranslate == ["fr"])
            }
        }

        @Test func aProviderErrorKeepsItsCodeAndAnyOtherErrorIsRemote() async throws {
            struct Quota: Error {}
            let failing = BDKError(
                domain: .provider, code: "quotaExceeded", message: "the quota is used up")
            for (failure, code) in [(failing, "quotaExceeded"), (Quota(), "remoteError")]
                as [(any Error, String)]
            {
                let services = try providerConfiguration(services: appTranslation) {
                    $0.services.translation.register(FakeTranslator(failure: failure))
                }
                try await withStubRuntime(services) { runtime in
                    let profile = try await runtime.profiles.open("Translation")
                    let page = try profile.webContents.create()
                    try runtime.stub.setPageText(["bonjour"], language: "fr", on: page)
                    runtime.stub.pump()
                    await expectError(.provider, code) {
                        _ = try await page.translation.translate()
                    }
                    #expect(try await page.translation.state().phase == .failed)
                }
            }
        }

        @Test func optionalOperationsAreOfferedOnlyWhenTheProviderHasThem() async throws {
            let removed = ProviderFlag()
            let services = try providerConfiguration(services: appTranslation) {
                $0.services.translation.register(
                    FakeFullTranslator(cancelled: ProviderFlag(), removed: removed))
            }
            try await withStubRuntime(services) { runtime in
                let profile = try await runtime.profiles.open("Translation")
                var fractions: [Double] = []
                try await profile.translation.downloadModel(from: "fr", to: "en") { fraction, _ in
                    fractions.append(fraction)
                }
                #expect(fractions == [0.25, 1])
                try await profile.translation.removeModel(from: "fr", to: "en")
                #expect(removed.value)

                // The page declares no language: the provider detects it.
                let page = try profile.webContents.create()
                try runtime.stub.setPageText(["bonjour"], on: page)
                var state = try await page.translation.state()
                for _ in 0..<100 where state.detected == nil {
                    try await Task.sleep(for: .milliseconds(10))
                    runtime.stub.pump()
                    state = try await page.translation.state()
                }
                #expect(state.phase == .available && state.detected == "fr")
            }

            // A provider without the optional protocols has no model to remove.
            let plain = try providerConfiguration(services: appTranslation) {
                $0.services.translation.register(FakeTranslator())
            }
            try await withStubRuntime(plain) { runtime in
                let profile = try await runtime.profiles.open("Translation")
                await expectError(.provider, "unavailable") {
                    try await profile.translation.removeModel(from: "fr", to: "en")
                }
            }
        }

        @Test func cancellingATranslationCancelsTheProvidersTask() async throws {
            let cancelled = ProviderFlag()
            let services = try providerConfiguration(services: appTranslation) {
                $0.services.translation.register(
                    FakeFullTranslator(cancelled: cancelled, removed: ProviderFlag()))
            }
            try await withStubRuntime(services) { runtime in
                let profile = try await runtime.profiles.open("Translation")
                let page = try profile.webContents.create()
                try runtime.stub.setPageText(["stuck"], language: "fr", on: page)
                runtime.stub.pump()
                // The pair is not installed, so the provider prepares it first and then translates.
                let translation = Task { try await page.translation.translate() }
                try await Task.sleep(for: .milliseconds(100))
                translation.cancel()
                await expectError(.broker, "cancelled") { _ = try await translation.value }
                try await settle(runtime) { cancelled.value }
            }
        }

        @Test func preferencesRoundTripThroughTheProfile() async throws {
            let services = try providerConfiguration(services: appTranslation) {
                $0.services.translation.register(FakeTranslator())
            }
            try await withStubRuntime(services) { runtime in
                let profile = try await runtime.profiles.open("Translation")
                let set = try await profile.translation.setPreferences(
                    target: "de", alwaysTranslate: ["es"], neverTranslate: ["fr"],
                    neverSites: ["news.example"], offers: false)
                #expect(
                    set
                        == BDKTranslationPreferences(
                            target: "de", alwaysTranslate: ["es"], neverTranslate: ["fr"],
                            neverSites: ["news.example"], offers: false))
                #expect(try await profile.translation.preferences() == set)
            }
        }

        @Test func aClosedPageThrowsStale() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                try await page.close()
                await expectError(.contents, "stale") { _ = try await page.translation.state() }
                await expectError(.contents, "stale") {
                    _ = try await page.translation.translate(to: "en")
                }
                await expectError(.contents, "stale") { try await page.translation.revert() }
                await expectError(.contents, "stale") { try await page.translation.present() }
            }
        }
    }
}
