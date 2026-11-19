import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("Stub controls")
    struct StubTests {
        @Test func aRequestWithoutAHandlerTakesTheDefault() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                try runtime.stub.raiseUIRequest(
                    surface: BDK_SURFACE_PERMISSIONS, on: page,
                    payloadJSON: "{\"permission\":\"camera\"}")
                runtime.stub.pump()
                let outcome = try runtime.stub.lastUIOutcome
                #expect(outcome.kind == .defaulted)
                #expect(outcome.surface == BDK_SURFACE_PERMISSIONS)
            }
        }

        @Test func withdrawingARequestIsRecorded() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                try runtime.stub.raiseUIRequest(surface: BDK_SURFACE_FILE_PICKERS, on: page)
                try runtime.stub.withdrawUIRequests()
                runtime.stub.pump()
                #expect(try runtime.stub.lastUIOutcome.kind == .withdrawn)
            }
        }

        @Test func scriptedNavigationAppliesToLaterLoads() async throws {
            try await withProfile { runtime, profile in
                let url = URL(string: "https://scripted.test/")!
                try runtime.stub.scriptNavigation(to: url, title: "Scripted")
                let page = try profile.webContents.create()
                try await page.navigate(to: url)
                #expect(page.title == "Scripted")
            }
        }

        @Test func hooksFailOnceTheRuntimeIsGone() async throws {
            let runtime = try BDKRuntime.startStub()
            await runtime.shutdown()
            #expect(throws: BDKError.self) { try runtime.stub.withdrawUIRequests() }
        }
    }
}
