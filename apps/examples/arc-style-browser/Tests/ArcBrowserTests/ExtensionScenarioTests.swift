import AppKit
import ArcBrowserCore
import BDK
import BDKTestSupport
import Foundation
import Testing

@testable import ArcBrowserKit

/// Extensions in the sidebar: an install the application did not make shows its action, and the
/// action's button opens the popup. Part of the serialized M8 suite: the stub runs one runtime at a
/// time.
extension ScenarioTests {
    @Test("an extension installed without the application's asking appears in its space's sidebar")
    func extensionsAreaFollowsInstalls() async throws {
        // Grants the install prompt that BDK shows the person in the application.
        let grant: (BDKConfiguration) throws -> Void = { configuration in
            try configuration.ui.extensionInstalls.setHandler { request in
                if let click = NSEvent.mouseEvent(
                    with: .leftMouseUp, location: .zero, modifierFlags: [], timestamp: 1,
                    windowNumber: 0, context: nil, eventNumber: 0, clickCount: 1, pressure: 0)
                {
                    try? request.accept(confirmedBy: click)
                }
                return .handled
            }
        }
        try await withBrowser(configure: grant) { browser in
            let controller = browser.controller
            let directory = "/tmp/arc-browser-tests-extension"
            try browser.runtime.stub.registerExtensionPackage(
                kind: "unpacked", key: directory,
                manifestJSON: #"""
                    {"manifest_version":3,"name":"Blocker","version":"1.0",
                    "action":{"default_title":"Blocker"}}
                    """#)
            let area = try #require(controller.windows[controller.focusedWindow]).sidebar
                .extensionsArea
            await controller.extensionRefresh?.value  // the one that start() asked for
            #expect(area.arrangedSubviews.isEmpty)

            // As a store page's install button does: in the profile, not through the controller.
            let page = try browser.page(try controller.focusedPage())
            let info = try await page.profile.extensions.install(
                .unpacked(URL(fileURLWithPath: directory)))
            try await waitUntil("the action in the sidebar") { area.arrangedSubviews.count == 1 }
            #expect(area.arrangedSubviews.first?.identifier?.rawValue == info.id)

            // Work's profile has no extensions; Personal's come back with its space.
            try await controller.perform(.switchSpace("Work"))
            try await waitUntil("Work's empty area") { area.arrangedSubviews.isEmpty }
            try await controller.perform(.switchSpace("Personal"))
            try await waitUntil("Personal's action") { area.arrangedSubviews.count == 1 }

            // Disabled (here without a prompt, unlike an uninstall), it leaves the area.
            try await page.profile.extensions.setEnabled(info.id, enabled: false)
            try await waitUntil("the action to go") { area.arrangedSubviews.isEmpty }
        }
    }

    @Test("a click on an extension's sidebar button opens its popup, handled after the click")
    func extensionButtonOpensItsPopup() async throws {
        let click = try #require(
            NSEvent.mouseEvent(
                with: .leftMouseUp, location: .zero, modifierFlags: [], timestamp: 1,
                windowNumber: 0, context: nil, eventNumber: 0, clickCount: 1, pressure: 0))
        let grant: (BDKConfiguration) throws -> Void = { configuration in
            try configuration.ui.extensionInstalls.setHandler { request in
                try? request.accept(confirmedBy: click)
                return .handled
            }
        }
        try await withBrowser(configure: grant) { browser in
            let controller = browser.controller
            let directory = "/tmp/arc-browser-tests-popup-extension"
            try browser.runtime.stub.registerExtensionPackage(
                kind: "unpacked", key: directory,
                manifestJSON: #"""
                    {"manifest_version":3,"name":"Blocker","version":"1.0",
                    "action":{"default_title":"Blocker","default_popup":"popup.html"}}
                    """#)
            let page = try browser.page(try controller.focusedPage())
            let info = try await page.profile.extensions.install(
                .unpacked(URL(fileURLWithPath: directory)))
            let area = try #require(controller.windows[controller.focusedWindow]).sidebar
                .extensionsArea
            try await waitUntil("the action in the sidebar") { area.arrangedSubviews.count == 1 }
            let button = try #require(area.arrangedSubviews.first as? NSButton)

            // The request carries the click: it is handled on a later turn of the run loop, when
            // NSApp.currentEvent is something else (here, nothing).
            #expect(NSApp.currentEvent == nil)
            controller.handle(
                .extensionAction(id: info.id, button: button, click: click),
                from: controller.focusedWindow)
            try await waitUntil("the popup") { !browser.placement.popups.isEmpty }
            #expect(browser.placement.popups.first?.extensionID == info.id)
        }
    }

    @Test("a popup whose clicked button the extensions area replaced still opens in the window")
    func popupAfterTheAreaWasRebuilt() async throws {
        try await withBrowser { browser in
            let controller = browser.controller
            let page = try browser.page(try controller.focusedPage())
            // The button clicked, then replaced by a refresh before the popup arrived: no longer
            // in any window, where NSPopover refuses to point.
            browser.placement.noteAnchor(NSButton(), in: controller.focusedWindow)
            try browser.runtime.stub.raiseUIRequest(
                surface: Self.extensionPopups, on: page,
                payloadJSON: #"{"extensionId":"abc","trigger":"userClick"}"#)
            try await waitUntil("the popup") { !browser.placement.popups.isEmpty }
            #expect(browser.placement.popups.first?.isShown == true)
        }
    }
}
