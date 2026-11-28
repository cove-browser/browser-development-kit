import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("Page health, autofill suggestions and exclusive access (T-UIR-15)")
    struct UIPageHealthRequestTests {
        @Test func anUnresponsivePageWaitsOrTerminates() async throws {
            try await withUIFixture { fixture in
                var request = try fixture.deliver(
                    UnresponsivePageRequest.self, BDK_SURFACE_PAGE_HEALTH,
                    "{\"kind\":\"unresponsive\",\"origin\":\"https://slow.test\",\"pageTitle\":\"Slow\"}"
                )
                #expect(request.origin == URL(string: "https://slow.test"))
                #expect(request.pageTitle == "Slow")
                #expect(request.presentation.title == "Slow is unresponsive")
                #expect(request.presentation.actions.map(\.label) == ["Exit page", "Wait"])
                request.wait()
                try fixture.expectOutcome(.completed, "accept", "{\"action\":\"wait\"}")

                request = try fixture.deliver(
                    UnresponsivePageRequest.self, BDK_SURFACE_PAGE_HEALTH,
                    "{\"kind\":\"unresponsive\"}")
                #expect(request.presentation.title == "Page Unresponsive")
                request.perform(.terminate("Exit page"))
                try fixture.expectOutcome(.completed, "accept", "{\"action\":\"terminate\"}")
            }
        }

        @Test func blockedPopupsOpenOneOrAllowTheSiteWithAGesture() async throws {
            try await withUIFixture { fixture in
                let payload =
                    "{\"kind\":\"blockedPopups\",\"origin\":\"https://ads.test\",\"popups\":["
                    + "{\"id\":\"1\",\"url\":\"https://ads.test/a\"},{\"id\":\"2\",\"url\":\"https://ads.test/b\"}]}"
                var request = try fixture.deliver(
                    BlockedPopupRequest.self, BDK_SURFACE_PAGE_HEALTH, payload)
                #expect(
                    request.urls.map(\.absoluteString) == [
                        "https://ads.test/a", "https://ads.test/b",
                    ])
                #expect(request.presentation.items.count == 2)
                #expect(
                    request.presentation.actions.first?.label
                        == "Always allow pop-ups and redirects from ads.test")
                let unlisted = BlockedPopupRequest.BlockedPopup(
                    id: "9", url: try #require(URL(string: "https://x.test")))
                #expect(throws: BDKError.self) {
                    try request.allowOnce(unlisted, confirmedBy: try confirmingEvent())
                }
                #expect(throws: BDKError.self) {
                    try request.perform(.alwaysAllow("Always"), confirmedBy: nil)
                }
                try request.allowOnce(request.popups[1], confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{\"popup\":\"2\"}")

                request = try fixture.deliver(
                    BlockedPopupRequest.self, BDK_SURFACE_PAGE_HEALTH, payload)
                try request.alwaysAllow(confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{\"always\":true}")

                request = try fixture.deliver(
                    BlockedPopupRequest.self, BDK_SURFACE_PAGE_HEALTH, payload)
                try request.perform(.done("Done"), confirmedBy: nil)
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }

        @Test func theStubRaisesPageHealthForAHungPageAndBlockedPopups() async throws {
            try await withUIFixture { fixture in
                let stub = fixture.runtime.stub
                fixture.box.request = nil
                try stub.makeUnresponsive(fixture.page)
                stub.pump()
                let hung = try #require(fixture.box.request as? UnresponsivePageRequest)
                try stub.makeResponsive(fixture.page)
                stub.pump()
                #expect(hung.isWithdrawn)

                #expect(
                    try stub.blockPopup(
                        on: fixture.page, url: #require(URL(string: "https://p.test/1"))))
                stub.pump()
                let first = try #require(fixture.box.request as? BlockedPopupRequest)
                #expect(first.popups.count == 1)
                #expect(
                    try stub.blockPopup(
                        on: fixture.page, url: #require(URL(string: "https://p.test/2"))))
                stub.pump()
                let both = try #require(fixture.box.request as? BlockedPopupRequest)
                #expect(first.isWithdrawn && both.popups.count == 2)
                try both.allowOnce(both.popups[0], confirmedBy: try confirmingEvent())
                stub.pump()
                let opened = try #require(fixture.box.request as? NewWindowRequest)
                #expect(opened.kind == .popup && opened.opener === fixture.page)
                #expect(opened.targetURL == URL(string: "https://p.test/1"))
            }
        }

        @Test func suggestionsArePreviewedAndClearedThroughTheRuntime() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    AutofillSuggestionsRequest.self, BDK_SURFACE_AUTOFILL_SUGGESTIONS,
                    "{\"suggestions\":[{\"id\":\"0\",\"label\":\"Ada\",\"kind\":\"address\"}]}")
                let suggestion = try #require(request.suggestions.first)
                try request.preview(suggestion)
                fixture.runtime.stub.pump()
                #expect(try fixture.runtime.stub.lastUIUpdate == "{\"preview\":\"0\"}")
                try request.preview(nil)
                fixture.runtime.stub.pump()
                #expect(try fixture.runtime.stub.lastUIUpdate == "{\"preview\":null}")
                let other = AutofillSuggestionsRequest.Suggestion(
                    id: "7", label: "Grace", sublabel: nil, kind: .address)
                #expect(throws: BDKError.self) { try request.preview(other) }
                // A preview fills nothing.
                try fixture.expectOutcome(.none)
            }
        }

        @Test func pageHealthIsDeliveredOnItsSurfaceAndCannotBeDisabled() throws {
            #expect(BDKUISurface.pageHealth.configurationKey == "page_health")
            let configuration = BDKConfiguration(
                resolvedJSON: "{\"surfaces\":{\"page_health\":\"disabled\"}}")
            #expect(throws: BDKError.self) { try BDKRuntime.startStub(configuration) }
        }
    }
}
