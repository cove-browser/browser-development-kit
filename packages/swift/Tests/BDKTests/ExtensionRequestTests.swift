import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("Extension requests")
    struct ExtensionRequestTests {
        /// Raises a popup request directly: the stub records the outcome and the application's
        /// updates only for requests it raised itself, not for the one an activation raises.
        private func raisePopup(_ fixture: ExtensionFixture) throws -> ExtensionPopupRequest {
            try fixture.runtime.stub.raiseUIRequest(
                surface: BDK_SURFACE_EXTENSION_POPUPS, on: fixture.page,
                payloadJSON: "{\"extensionId\":\"abc\",\"trigger\":\"command\"}")
            fixture.runtime.stub.pump()
            return try #require(fixture.box.popups.last)
        }

        @Test func aPopupFollowsActivation() async throws {
            try await withExtensionFixture { fixture in
                let info = try await fixture.install(
                    manifest: ExtensionFixture.manifest(extra: ExtensionFixture.popupAction))
                let outcome = try fixture.extensions.activateAction(
                    info.id, on: fixture.page, confirmedBy: try confirmingEvent())
                #expect(outcome == .popupRequested)
                fixture.runtime.stub.pump()
                let popup = try #require(fixture.box.popups.first)
                #expect(popup.extensionID == info.id && popup.trigger == .userClick)
                #expect(popup.extensionName == "Test extension")
                #expect(popup.url?.absoluteString == "chrome-extension://\(info.id)/p.html")
                #expect(popup.preferredSize == .zero)

            }
        }

        @Test func aPopupTakesSizeUpdatesAndCloses() async throws {
            try await withExtensionFixture { fixture in
                let popup = try raisePopup(fixture)
                #expect(popup.trigger == .command)
                let sizes = EventReader(popup.sizeChanges)
                try fixture.runtime.stub.sendUIUpdate(
                    "{\"preferredSize\":{\"width\":320,\"height\":240}}")
                fixture.runtime.stub.pump()
                #expect(await sizes.next() == CGSize(width: 320, height: 240))
                #expect(popup.preferredSize == CGSize(width: 320, height: 240))

                popup.accept()
                fixture.runtime.stub.pump()
                #expect(try fixture.runtime.stub.lastUIUpdate == "{\"placed\":true}")
                #expect(!popup.isCompleted)

                popup.close()
                fixture.runtime.stub.pump()
                let outcome2 = try fixture.runtime.stub.lastUIOutcome
                #expect(outcome2.kind == .completed && outcome2.completion == "accept")
                #expect(outcome2.argumentsJSON == "{\"action\":\"close\"}")
                #expect(await sizes.next() == nil)
            }
        }

        @Test func aPopupRejectedIsDenied() async throws {
            try await withExtensionFixture { fixture in
                try raisePopup(fixture).reject()
                fixture.runtime.stub.pump()
                let outcome = try fixture.runtime.stub.lastUIOutcome
                #expect(outcome.kind == .completed && outcome.completion == "deny")
            }
        }

        @Test func aPopupWithdrawnFiresClosed() async throws {
            try await withExtensionFixture { fixture in
                let popup = try raisePopup(fixture)
                let sizes = EventReader(popup.sizeChanges)
                #expect(!popup.closed.isFired)
                try fixture.runtime.stub.withdrawUIRequests()
                fixture.runtime.stub.pump()
                #expect(popup.closed.isFired && popup.isWithdrawn)
                #expect(await sizes.next() == nil)
                popup.accept()
                fixture.runtime.stub.pump()
                #expect(try fixture.runtime.stub.lastUIUpdate == "")
            }
        }

        @Test func aSidePanelAcceptsAndFollowsUpdatesBothWays() async throws {
            try await withExtensionFixture { fixture in
                let payload = """
                    {"extensionId":"abc","title":"Notes","url":"chrome-extension://abc/p.html",\
                    "scope":"page","contents":"\(fixture.page.id.rawValue)",\
                    "icon":"data:image/png;base64,AAAA"}
                    """
                try fixture.runtime.stub.raiseUIRequest(
                    surface: BDK_SURFACE_EXTENSION_SIDE_PANELS, on: fixture.page,
                    payloadJSON: payload)
                fixture.runtime.stub.pump()
                let panel = try #require(fixture.box.panels.first)
                #expect(panel.extensionID == "abc" && panel.title == "Notes")
                guard case .page(let scoped) = panel.scope else {
                    Issue.record("expected a page scope")
                    return
                }
                #expect(scoped === fixture.page)
                #expect(panel.icon == nil)  // not an image

                let visibility = EventReader(panel.visibilityChanges)
                try fixture.runtime.stub.sendUIUpdate("{\"visible\":false}")
                fixture.runtime.stub.pump()
                #expect(await visibility.next() == false)

                panel.accept(side: .leading)
                fixture.runtime.stub.pump()
                #expect(
                    try fixture.runtime.stub.lastUIUpdate
                        == "{\"placed\":true,\"side\":\"leading\"}")
                panel.side = .trailing
                fixture.runtime.stub.pump()
                #expect(try fixture.runtime.stub.lastUIUpdate == "{\"side\":\"trailing\"}")
                #expect(panel.side == .trailing)

                panel.close()
                fixture.runtime.stub.pump()
                #expect(try fixture.runtime.stub.lastUIOutcome.completion == "accept")
                #expect(await visibility.next() == nil)
            }
        }

        @Test func aWindowScopedSidePanelIsRejected() async throws {
            try await withExtensionFixture { fixture in
                try fixture.runtime.stub.raiseUIRequest(
                    surface: BDK_SURFACE_EXTENSION_SIDE_PANELS, on: fixture.page,
                    payloadJSON: "{\"extensionId\":\"abc\",\"scope\":\"window\"}")
                fixture.runtime.stub.pump()
                let panel = try #require(fixture.box.panels.first)
                guard case .window = panel.scope else {
                    Issue.record("expected a window scope")
                    return
                }
                panel.reject()
                fixture.runtime.stub.pump()
                #expect(try fixture.runtime.stub.lastUIOutcome.completion == "deny")
                try fixture.runtime.stub.raiseUIRequest(
                    surface: BDK_SURFACE_EXTENSION_SIDE_PANELS, on: fixture.page,
                    payloadJSON: "{\"extensionId\":\"abc\"}")
                fixture.runtime.stub.pump()
                try #require(fixture.box.panels.last).close()
                fixture.runtime.stub.pump()
                #expect(
                    try fixture.runtime.stub.lastUIOutcome.argumentsJSON == "{\"action\":\"close\"}"
                )
            }
        }
    }
}
