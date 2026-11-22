import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("UI window requests")
    struct UIWindowRequestTests {
        @Test func contextMenus() async throws {
            try await withUIFixture { fixture in
                let payload = """
                    {"location":{"x":10,"y":20},"context":{"linkURL":"https://l.test/",\
                    "selectionText":"hi","isEditable":true,"spellingSuggestions":["a"]},\
                    "items":[{"id":"copy","kind":"command","role":"copy","title":"Copy"},\
                    {"id":"more","kind":"submenu","title":"More","children":[\
                    {"id":"inspect","role":"inspect","title":"Inspect","source":"extension","extensionId":"ext1"}]}]}
                    """
                var request = try fixture.deliver(
                    ContextMenuRequest.self, BDK_SURFACE_CONTEXT_MENUS, payload)
                #expect(request.location == CGPoint(x: 10, y: 20))
                #expect(request.context.linkURL == URL(string: "https://l.test/"))
                #expect(request.context.selectionText == "hi" && request.context.isEditable)
                #expect(request.items.map(\.title) == ["Copy", "More"])
                let inspect = try #require(request.items[1].children.first)
                #expect(inspect.source == .extension(id: "ext1") && inspect.role == "inspect")

                try request.perform(inspect)
                try fixture.expectOutcome(
                    .completed, "accept", "{\"action\":\"perform\",\"item\":\"inspect\"}")

                request = try fixture.deliver(
                    ContextMenuRequest.self, BDK_SURFACE_CONTEXT_MENUS, payload)
                let own = BDKMenuItem(id: .init(rawValue: "mine"), title: "Mine")
                await expectError(.ui, "unknownItem") { try request.perform(own) }
                request.show([own], at: CGPoint(x: 1, y: 2))
                // The stub keeps only the first 127 bytes of the arguments.
                let shown = try fixture.outcome()
                #expect(shown.kind == .completed && shown.completion == "accept")
                #expect(
                    shown.argumentsJSON.hasPrefix(
                        "{\"action\":\"show\",\"at\":{\"x\":1,\"y\":2},\"items\":[{\"children\":[]")
                )

                request = try fixture.deliver(
                    ContextMenuRequest.self, BDK_SURFACE_CONTEXT_MENUS, payload)
                request.dismiss()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }

        @Test func newWindows() async throws {
            try await withUIFixture { fixture in
                let created = try fixture.profile.webContents.create()
                let payload = """
                    {"kind":"popup","disposition":"popup","targetURL":"https://p.test/",\
                    "opener":"\(fixture.page.id.rawValue)","newContents":"\(created.id.rawValue)",\
                    "features":{"width":300,"height":200},"isUserGesture":true}
                    """
                var request = try fixture.deliver(
                    NewWindowRequest.self, BDK_SURFACE_NEW_WINDOWS, payload)
                #expect(request.kind == .popup && request.disposition == .popup)
                #expect(request.targetURL == URL(string: "https://p.test/"))
                #expect(request.opener === fixture.page && request.isUserGesture)
                #expect(request.features.width == 300 && request.features.x == nil)
                #expect(request.newContents === created)
                request.accept()
                try fixture.expectOutcome(.completed, "accept", "{}")

                request = try fixture.deliver(NewWindowRequest.self, BDK_SURFACE_NEW_WINDOWS)
                #expect(request.kind == .newPage && request.newContents == nil)
                request.reject()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }

        @Test func printing() async throws {
            try await withUIFixture { fixture in
                let preview = try fixture.profile.webContents.create()
                var request = try fixture.deliver(
                    PrintRequest.self, BDK_SURFACE_PRINTING,
                    "{\"isUserInitiated\":false,\"previewContents\":\"\(preview.id.rawValue)\"}")
                #expect(!request.isUserInitiated && request.previewContents === preview)
                request.printWithSystemDialog()
                try fixture.expectOutcome(.completed, "accept", "{\"action\":\"systemDialog\"}")
                request = try fixture.deliver(PrintRequest.self, BDK_SURFACE_PRINTING)
                request.savePDF(to: URL(fileURLWithPath: "/tmp/p.pdf"))
                try fixture.expectOutcome(
                    .completed, "accept", "{\"action\":\"savePDF\",\"path\":\"/tmp/p.pdf\"}")
                request = try fixture.deliver(PrintRequest.self, BDK_SURFACE_PRINTING)
                #expect(request.isUserInitiated)
                request.cancel()
                try fixture.expectOutcome(.completed, "deny", "{}")
            }
        }

        @Test func findFullscreenAndExclusiveAccess() async throws {
            try await withUIFixture { fixture in
                let find = try fixture.deliver(FindRequest.self, BDK_SURFACE_FIND)
                #expect(find.initialText == nil)
                find.close()
                try fixture.expectOutcome(.completed, "accept", "{\"action\":\"close\"}")

                var fullscreen = try fixture.deliver(
                    FullscreenRequest.self, BDK_SURFACE_FULLSCREEN,
                    "{\"origin\":\"https://v.test\",\"isEntering\":false}")
                #expect(
                    !fullscreen.isEntering && fullscreen.origin == URL(string: "https://v.test"))
                fullscreen.allow()
                try fixture.expectOutcome(.completed, "accept", "{}")
                fullscreen = try fixture.deliver(FullscreenRequest.self, BDK_SURFACE_FULLSCREEN)
                #expect(fullscreen.isEntering)
                fullscreen.deny()
                try fixture.expectOutcome(.completed, "deny", "{}")

                let access = try fixture.deliver(
                    ExclusiveAccessRequest.self, BDK_SURFACE_EXCLUSIVE_ACCESS,
                    "{\"kind\":\"keyboardLock\",\"exitInstruction\":\"Hold Esc\"}")
                #expect(access.kind == .keyboardLock && access.exitInstruction == "Hold Esc")
                access.acknowledge()
                try fixture.expectOutcome(.completed, "accept", "{}")
            }
        }

        @Test func devToolsAndExternalProtocolsAndRawSurfaces() async throws {
            try await withUIFixture { fixture in
                let frontend = try fixture.profile.webContents.create()
                let devTools = try fixture.deliver(
                    DevToolsRequest.self, BDK_SURFACE_DEVTOOLS,
                    "{\"inspected\":\"\(fixture.page.id.rawValue)\",\"frontend\":\"\(frontend.id.rawValue)\","
                        + "\"preferredDock\":\"right\"}")
                #expect(devTools.inspected === fixture.page && devTools.frontend === frontend)
                #expect(devTools.preferredDock == .right)
                devTools.accept()
                try fixture.expectOutcome(.completed, "accept", "{}")

                let external = try fixture.deliver(
                    ExternalProtocolRequest.self, BDK_SURFACE_EXTERNAL_PROTOCOLS,
                    "{\"url\":\"mailto:a@b.test\",\"handlerName\":\"Mail\",\"isUserGesture\":true}")
                #expect(external.handlerName == "Mail" && external.isUserGesture)
                try external.open(remember: true, confirmedBy: try confirmingEvent())
                try fixture.expectOutcome(.completed, "grant", "{\"remember\":true}")

                let raw = try fixture.deliver(
                    BDKRawUIRequest.self, BDK_SURFACE_DEVICE_CHOOSERS, "{\"kind\":\"usb\"}")
                #expect(raw.payload["kind"] == .string("usb"))
                try raw.complete("deny", arguments: ["why": .string("no")])
                try fixture.expectOutcome(.completed, "deny", "{\"why\":\"no\"}")
            }
        }
    }
}
