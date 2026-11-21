import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("Extensions")
    struct ExtensionTests {
        @Test func installAcceptedWithAGesture() async throws {
            try await withExtensionFixture { fixture in
                let events = EventReader(fixture.extensions.events)
                let manifest = ExtensionFixture.manifest(
                    permissions: ["tabs"], extra: ExtensionFixture.plainAction)
                let info = try await fixture.install(manifest: manifest)

                let prompt = try #require(fixture.box.installs.first)
                #expect(prompt.kind == .install && prompt.source == .unpacked)
                #expect(prompt.extensionName == "Test extension")
                #expect(prompt.warnings == ["Uses tabs"] && prompt.hostWarnings.isEmpty)
                #expect(prompt.compatibility.verdict == .compatible)
                #expect(prompt.extensionID == info.id)
                #expect(info.name == "Test extension" && info.isEnabled)
                #expect(try await fixture.extensions.list().map(\.id) == [info.id])

                let installed = try await events.next {
                    if case .installed = $0 { true } else { false }
                }
                guard case .installed(let payload) = installed else { return }
                #expect(payload.extension.id == info.id)
                #expect(info.iconImage == nil)
            }
        }

        @Test func installDeniedIsCancelled() async throws {
            try await withExtensionFixture { fixture in
                try fixture.runtime.stub.registerExtensionPackage(
                    kind: "unpacked", key: "/tmp/bdk-ext-b",
                    manifestJSON: ExtensionFixture.manifest())
                fixture.box.onInstall = { $0.cancel() }
                await expectError(.extension, "cancelled") {
                    _ = try await fixture.extensions.install(
                        .unpacked(URL(fileURLWithPath: "/tmp/bdk-ext-b")))
                }
                #expect(try await fixture.extensions.list().isEmpty)
            }
        }

        @Test func aDroppedInstallPromptIsCancelled() async throws {
            try await withExtensionFixture { fixture in
                try fixture.runtime.stub.registerExtensionPackage(
                    kind: "unpacked", key: "/tmp/bdk-ext-b",
                    manifestJSON: ExtensionFixture.manifest())
                // The handler keeps the request until the test drops it.
                fixture.box.onInstall = { request in
                    Task { @MainActor in
                        try? await Task.sleep(for: .milliseconds(20))
                        request.cancel()
                    }
                }
                await expectError(.extension, "cancelled") {
                    _ = try await fixture.extensions.install(
                        .unpacked(URL(fileURLWithPath: "/tmp/bdk-ext-b")))
                }
            }
        }

        @Test func aSourceTheConfigurationRefusesNeverReachesThePrompt() async throws {
            try await withExtensionFixture(
                resolvedJSON: "{\"extensions\":{\"install\":{\"crx\":false}}}"
            ) { fixture in
                try fixture.runtime.stub.registerExtensionPackage(
                    kind: "crx", key: "/tmp/a.crx", manifestJSON: ExtensionFixture.manifest())
                await expectError(.extension, "sourceNotAllowed") {
                    _ = try await fixture.extensions.install(
                        .crx(URL(fileURLWithPath: "/tmp/a.crx")))
                }
                #expect(fixture.box.installs.isEmpty)
            }
        }

        @Test func anIncompatibleExtensionIsRefusedAndInspectSaysWhy() async throws {
            try await withExtensionFixture { fixture in
                try fixture.runtime.stub.registerExtensionPackage(
                    kind: "crx", key: "/tmp/b.crx",
                    manifestJSON: ExtensionFixture.manifest(permissions: ["platformKeys"]))
                let source = BDKExtensionSource.crx(URL(fileURLWithPath: "/tmp/b.crx"))
                let report = try await fixture.extensions.inspect(source)
                #expect(report.verdict == .incompatible)
                #expect(report.findings.first?.severity == .blocking)
                await expectError(.extension, "incompatible") {
                    _ = try await fixture.extensions.install(source)
                }
                #expect(fixture.box.installs.isEmpty)
            }
        }

        @Test func checkForUpdatesFindsANewVersion() async throws {
            let id = String(repeating: "a", count: 32)
            try await withExtensionFixture(
                resolvedJSON: "{\"extensions\":{\"install\":{\"update_urls\":\"any\"}}}"
            ) { fixture in
                try fixture.runtime.stub.registerExtensionPackage(
                    kind: "updateUrl", key: id, manifestJSON: ExtensionFixture.manifest())
                fixture.box.confirmInstalls()
                let source = BDKExtensionSource.updateURL(
                    id: id, url: URL(string: "https://updates.test/crx")!)
                _ = try await fixture.extensions.install(source)

                let events = EventReader(fixture.extensions.events)
                #expect(try await fixture.extensions.checkForUpdates().updated.isEmpty)
                try fixture.runtime.stub.registerExtensionPackage(
                    kind: "updateUrl", key: id,
                    manifestJSON: ExtensionFixture.manifest(version: "2.0"))
                let result = try await fixture.extensions.checkForUpdates(id: id)
                #expect(result.updated == [id] && result.refused.isEmpty)
                let updated = try await events.next {
                    if case .updated = $0 { true } else { false }
                }
                guard case .updated(let payload) = updated else { return }
                #expect(payload.previousVersion == "1.0" && payload.extension.version == "2.0")
            }
        }

        @Test func uninstallAsksTheUser() async throws {
            try await withExtensionFixture { fixture in
                let info = try await fixture.install()
                let events = EventReader(fixture.extensions.events)
                fixture.box.confirmUninstalls()
                try await fixture.extensions.uninstall(info.id)
                let prompt = try #require(fixture.box.uninstalls.first)
                #expect(prompt.extensionName == "Test extension" && prompt.requestedBy == .user)
                #expect(prompt.extensionID == info.id)
                #expect(try await fixture.extensions.list().isEmpty)
                _ = try await events.next {
                    if case .uninstalled(let gone) = $0 { gone.id == info.id } else { false }
                }
            }
        }

        @Test func uninstallCanBeCancelled() async throws {
            try await withExtensionFixture { fixture in
                let info = try await fixture.install()
                fixture.box.onUninstall = { $0.cancel() }
                await expectError(.extension, "cancelled") {
                    try await fixture.extensions.uninstall(info.id)
                }
                #expect(try await fixture.extensions.list().count == 1)
            }
        }

        @Test func settingsAndEventsFollowTheExtension() async throws {
            try await withExtensionFixture { fixture in
                let info = try await fixture.install()
                let events = EventReader(fixture.extensions.events)
                let changes = EventReader(fixture.extensions.actionChanges)

                try await fixture.extensions.setEnabled(info.id, enabled: false)
                let changed = try await events.next {
                    if case .enabledChanged = $0 { true } else { false }
                }
                guard case .enabledChanged(let payload) = changed else { return }
                #expect(payload.id == info.id && !payload.isEnabled)

                try await fixture.extensions.setPinned(info.id, pinned: true)
                #expect(try await changes.next()?.id == info.id)
                try await fixture.extensions.setSiteAccess(
                    info.id, access: .onSpecificSites, patterns: ["https://a.test/*"])
                try await fixture.extensions.setAllowedInPrivate(info.id, allowed: true)
                try await fixture.extensions.setAllowedFileAccess(info.id, allowed: true)
                let after = try await fixture.extensions.get(info.id)
                #expect(after.isPinned && !after.isEnabled)
                #expect(after.siteAccess == .onSpecificSites)
                #expect(after.siteAccessPatterns == ["https://a.test/*"])
                #expect(after.allowedInPrivate && after.allowedFileAccess)
            }
        }

        @Test func actionsListedForAPage() async throws {
            try await withExtensionFixture { fixture in
                let info = try await fixture.install(
                    manifest: ExtensionFixture.manifest(extra: ExtensionFixture.popupAction))
                let actions = try await fixture.extensions.actions(for: fixture.page)
                #expect(actions.map(\.id) == [info.id])
                #expect(actions.first?.title == "Open" && actions.first?.hasPopup == true)
                #expect(actions.first?.iconImage == nil && actions.first?.badgeNSColor == nil)
            }
        }

        @Test func activationOutcomes() async throws {
            try await withExtensionFixture { fixture in
                let plain = try await fixture.install(directory: "/tmp/bdk-ext-plain")
                let event = try confirmingEvent()
                #expect(
                    try fixture.extensions.activateAction(
                        plain.id, on: fixture.page, confirmedBy: event)
                        == .dispatchedClick)

                let bare = try await fixture.install(
                    directory: "/tmp/bdk-ext-bare",
                    manifest: ExtensionFixture.manifest(name: "Bare"))
                guard
                    case .blocked(let reason) = try fixture.extensions.activateAction(
                        bare.id, on: fixture.page, confirmedBy: event)
                else {
                    Issue.record("expected a blocked outcome")
                    return
                }
                #expect(!reason.isEmpty)

                await expectError(.extension, "notFound") {
                    _ = try fixture.extensions.activateAction(
                        "nope", on: fixture.page, confirmedBy: event)
                }
            }
        }

        @Test func commandsAreListedAndPerformed() async throws {
            try await withExtensionFixture { fixture in
                let commands =
                    ",\"commands\":{\"run\":{\"description\":\"Run\",\"suggested_key\":{\"default\":\"Ctrl+Shift+Y\"}}}"
                let info = try await fixture.install(
                    manifest: ExtensionFixture.manifest(extra: commands))
                let declared = try await fixture.extensions.commands()
                let command = try #require(declared.first { $0.name == "run" })
                #expect(command.extensionId == info.id && command.description == "Run")
                #expect(
                    try fixture.extensions.performCommand(
                        command, on: fixture.page, confirmedBy: try confirmingEvent())
                        == .dispatchedClick)
                try await fixture.extensions.setCommandShortcut(
                    extensionId: info.id, name: "run", shortcut: "Ctrl+Shift+U")
                let bound = try await fixture.extensions.commands().first { $0.name == "run" }
                #expect(bound?.shortcut == "Ctrl+Shift+U")
            }
        }

        @Test func presentingTheDefaultMenuReachesTheRuntime() async throws {
            try await withExtensionFixture { fixture in
                let window = NSWindow(
                    contentRect: NSRect(x: 0, y: 0, width: 400, height: 300), styleMask: [.titled],
                    backing: .buffered, defer: true)
                window.isReleasedWhenClosed = false
                let registered = try fixture.runtime.windows.register(window)
                #expect(try fixture.runtime.stub.menusPresented == 0)
                try fixture.extensions.presentDefaultMenu(
                    in: registered, confirmedBy: try confirmingEvent())
                #expect(try fixture.runtime.stub.menusPresented == 1)
            }
        }

        @Test func anOptionsPageNamesTheExtensionThatOpenedIt() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    NewWindowRequest.self, BDK_SURFACE_NEW_WINDOWS,
                    "{\"kind\":\"newPage\",\"initiator\":{\"extensionId\":\"abc\"}}")
                #expect(request.initiator == .extension(id: "abc"))
            }
        }

        @Test func imagesAndColoursDecode() throws {
            // A 1x1 PNG.
            let png =
                "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAfbLI3wAAAABJRU5ErkJggg=="
            let image = BDKImageDecoding.image(fromDataURL: "data:image/png;base64,\(png)")
            #expect(image != nil)
            #expect(BDKImageDecoding.image(fromDataURL: nil) == nil)
            #expect(BDKImageDecoding.image(fromDataURL: "https://x.test/i.png") == nil)
            #expect(BDKImageDecoding.image(fromDataURL: "data:image/png;base64,!!") == nil)
            let red = BDKImageDecoding.color(fromHex: "#FF0000")
            #expect(red?.redComponent == 1 && red?.alphaComponent == 1)
            #expect(BDKImageDecoding.color(fromHex: "#00000080")?.alphaComponent ?? 0 > 0.49)
            #expect(BDKImageDecoding.color(fromHex: "red") == nil)
        }
    }
}
