import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    /// Swift reputation and geolocation providers (T-PRV-04, T-PRV-05 on the Swift side), against
    /// the stub.
    @MainActor
    @Suite("Reputation and geolocation providers")
    struct ProviderServiceTests {
        // MARK: Reputation

        @Test func aSwiftVerdictRaisesTheWarningWithTheSwiftDescriptor() async throws {
            let box = UIBox()
            let services = try providerConfiguration(
                services: "\"reputation\":{\"provider\":\"app\"}"
            ) {
                $0.services.reputation.register(FakeReputation())
                try $0.ui.reputationWarnings.setHandler { request in
                    box.request = request
                    return .reject
                }
            }
            try await withStubRuntime(services) { runtime in
                let profile = try await runtime.profiles.open("Reputation")
                let page = try profile.webContents.create()
                try await page.navigation.load(URL(string: "https://fine.test/")!)
                #expect(box.request == nil)
                await expectError(.navigation, "netError") {
                    try await page.navigation.load(URL(string: "https://malware.test/")!)
                }
                let warning = try #require(box.request as? ReputationWarningRequest)
                #expect(warning.verdict == .malware)
                let provider = try #require(warning.provider)
                #expect(provider.id == "app.rep" && provider.privacy == .remote)
                #expect(provider.host == "rep.example.com" && provider.protection == "full")
            }
        }

        @Test func theDefaultListsStillBlockWithoutASwiftProvider() async throws {
            let box = UIBox()
            var services = BDKConfiguration.stub
            try services.ui.reputationWarnings.setHandler { request in
                box.request = request
                return .reject
            }
            try await withStubRuntime(services) { runtime in
                let list = """
                    {"format":"bdk-reputation-list","version":1,"sequence":1,\
                    "generated":1700000000000,"expires":4102444800000,"source":"test",\
                    "entries":[{"expression":"evil.test/","verdict":"phishing"}]}
                    """
                try runtime.stub.loadReputationList(list)
                let page = try await runtime.profiles.open("Reputation").webContents.create()
                await expectError(.navigation, "netError") {
                    try await page.navigation.load(URL(string: "https://evil.test/login")!)
                }
                let warning = try #require(box.request as? ReputationWarningRequest)
                #expect(warning.verdict == .phishing)
                await expectError(.provider, "invalidList") {
                    try runtime.stub.loadReputationList("{\"format\":\"other\"}")
                }
            }
        }

        // MARK: Geolocation

        @Test func positionsReachThePageAndStoppingTheWatchCancelsTheStream() async throws {
            let terminated = ProviderFlag()
            let services = try providerConfiguration(
                services: "\"geolocation\":{\"provider\":\"app\"}"
            ) {
                $0.services.geolocation.register(FakeGeolocation(terminated: terminated))
            }
            try await withStubRuntime(services) { runtime in
                let page = try await runtime.profiles.open("Geolocation").webContents.create()
                try runtime.stub.startGeolocationWatch(on: page, highAccuracy: true)
                try await settle(runtime) {
                    try runtime.stub.lastGeolocation(of: page)?.contains("\"latitude\":51.5")
                        == true
                }
                let last = try #require(try runtime.stub.lastGeolocation(of: page))
                #expect(last.contains("\"accuracy\":5") && last.contains("\"timestamp\":"))
                #expect(!terminated.value)
                try runtime.stub.stopGeolocationWatch(on: page)
                try await settle(runtime) { terminated.value }
            }
        }

        @Test func aPositionErrorIsReportedToThePage() async throws {
            let denied = BDKError(
                domain: .provider, code: "permissionDenied", message: "location is off")
            let services = try providerConfiguration(
                services: "\"geolocation\":{\"provider\":\"app\"}"
            ) {
                $0.services.geolocation.register(
                    FakeGeolocation(terminated: ProviderFlag(), failure: denied))
            }
            try await withStubRuntime(services) { runtime in
                let page = try await runtime.profiles.open("Geolocation").webContents.create()
                try runtime.stub.startGeolocationWatch(on: page)
                try await settle(runtime) {
                    try runtime.stub.lastGeolocation(of: page)?.contains("permissionDenied") == true
                }
            }
        }
    }
}
