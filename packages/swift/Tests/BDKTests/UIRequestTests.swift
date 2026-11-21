import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

extension BDKStubTests {
    @MainActor
    @Suite("UI requests")
    struct UIRequestTests {
        @Test func surfaceIdentifiersMatchTheHeaders() {
            let expected: [BDKUISurface: UInt32] = [
                .permissions: BDK_SURFACE_PERMISSIONS,
                .javascriptDialogs: BDK_SURFACE_JAVASCRIPT_DIALOGS,
                .httpAuth: BDK_SURFACE_HTTP_AUTH,
                .clientCertificates: BDK_SURFACE_CLIENT_CERTIFICATES,
                .filePickers: BDK_SURFACE_FILE_PICKERS, .contextMenus: BDK_SURFACE_CONTEXT_MENUS,
                .newWindows: BDK_SURFACE_NEW_WINDOWS, .downloads: BDK_SURFACE_DOWNLOADS,
                .passwords: BDK_SURFACE_PASSWORDS, .autofill: BDK_SURFACE_AUTOFILL,
                .autofillSuggestions: BDK_SURFACE_AUTOFILL_SUGGESTIONS,
                .printing: BDK_SURFACE_PRINTING, .translation: BDK_SURFACE_TRANSLATION,
                .find: BDK_SURFACE_FIND, .fullscreen: BDK_SURFACE_FULLSCREEN,
                .exclusiveAccess: BDK_SURFACE_EXCLUSIVE_ACCESS,
                .notifications: BDK_SURFACE_NOTIFICATIONS,
                .certificateErrors: BDK_SURFACE_CERTIFICATE_ERRORS,
                .reputationWarnings: BDK_SURFACE_REPUTATION_WARNINGS,
                .devTools: BDK_SURFACE_DEVTOOLS,
                .deviceChoosers: BDK_SURFACE_DEVICE_CHOOSERS,
                .screenCapture: BDK_SURFACE_SCREEN_CAPTURE, .webAuthn: BDK_SURFACE_WEBAUTHN,
                .externalProtocols: BDK_SURFACE_EXTERNAL_PROTOCOLS,
            ]
            #expect(expected.count == BDKUISurface.allCases.count)
            for (surface, value) in expected { #expect(surface.rawValue == value) }
        }

        @Test func useDefaultTakesTheDefaultOnEverySurface() async throws {
            try await withUIFixture { fixture in
                fixture.box.response = .useDefault
                for surface in BDKUISurface.allCases {
                    try fixture.raise(surface.rawValue)
                    let request = try #require(fixture.box.request)
                    #expect(request.surface == surface)
                    #expect(request.isCompleted)
                    try fixture.expectOutcome(.defaulted)
                }
            }
        }

        @Test func rejectRejectsOnEverySurface() async throws {
            try await withUIFixture { fixture in
                fixture.box.response = .reject
                for surface in BDKUISurface.allCases {
                    try fixture.raise(surface.rawValue)
                    #expect(try #require(fixture.box.request).isCompleted)
                    try expectNeverGranted(fixture, surface, otherwise: .rejected)
                }
            }
        }

        @Test func handledThenDroppedIsCancelledOnEverySurface() async throws {
            try await withUIFixture { fixture in
                for surface in BDKUISurface.allCases {
                    try fixture.raise(surface.rawValue)
                    #expect(try #require(fixture.box.request).isCompleted == false)
                    fixture.box.request = nil
                    try expectNeverGranted(fixture, surface, otherwise: .cancelled)
                }
            }
        }

        @Test func handledThenUseDefaultPresenterHandsItBack() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    FindRequest.self, BDK_SURFACE_FIND, "{\"initialText\":\"needle\"}")
                #expect(request.initialText == "needle")
                request.useDefaultPresenter()
                #expect(request.isCompleted)
                try fixture.expectOutcome(.defaulted)
            }
        }

        @Test func aRequestIsCompletedOnce() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(FullscreenRequest.self, BDK_SURFACE_FULLSCREEN)
                request.allow()
                #expect(request.isCompleted)
                #expect(throws: BDKError.self) { try request.submit("deny") }
                request.deny()  // ignored: the first completion stands
                try fixture.expectOutcome(.completed, "accept", "{}")
                await expectError(.ui, "requestCompleted") { try request.submit("accept") }
            }
        }

        @Test func aWithdrawnRequestFiresItsCancellationAndIgnoresCompletion() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(FullscreenRequest.self, BDK_SURFACE_FULLSCREEN)
                var fired = 0
                request.cancellation.onFire { fired += 1 }
                try fixture.runtime.stub.withdrawUIRequests()
                fixture.runtime.stub.pump()
                #expect(request.isWithdrawn && request.isCompleted && request.cancellation.isFired)
                #expect(fired == 1)
                request.allow()
                try request.submit("accept")
                try fixture.expectOutcome(.withdrawn)
                await request.cancellation.wait()
            }
        }

        @Test func aRequestCarriesItsPageAndProfile() async throws {
            try await withUIFixture { fixture in
                let request = try fixture.deliver(
                    PermissionRequest.self, BDK_SURFACE_PERMISSIONS,
                    "{\"kinds\":[\"camera\"],\"origin\":\"https://site.test\"}")
                #expect(request.contents === fixture.page)
                #expect(request.profile === fixture.profile)
                #expect(request.anchor == nil && request.window == nil)
                #expect(request.initiator == .page(origin: URL(string: "https://site.test")!))
            }
        }

        @Test func handlersSetOnTheConfigurationAreInstalledAtStart() async throws {
            let box = UIBox()
            var configuration = BDKConfiguration.stub
            try configuration.ui.permissions.setHandler { request in
                box.request = request
                return .handled
            }
            try await withStubRuntime(configuration) { runtime in
                let profile = try await runtime.profiles.open("Config")
                let page = try profile.webContents.create()
                try runtime.stub.raiseUIRequest(
                    surface: BDK_SURFACE_PERMISSIONS, on: page, payloadJSON: "{}")
                runtime.stub.pump()
                #expect(box.request is PermissionRequest)
                #expect(runtime.ui.permissions.hasHandler)
                #expect(!runtime.ui.find.hasHandler)
                box.request = nil
            }
        }

        @Test func afterTheFreezeAHandlerCanBeReplacedButNotAddedToAnotherSurface() async throws {
            var configuration = BDKConfiguration.stub
            try configuration.ui.find.setHandler { _ in .reject }
            try await withStubRuntime(configuration) { runtime in
                let profile = try await runtime.profiles.open("Frozen")
                let page = try profile.webContents.create()
                await expectError(.config, "registrationAfterStart") {
                    try runtime.ui.fullscreen.setHandler { _ in .handled }
                }
                #expect(!runtime.ui.fullscreen.hasHandler)

                try runtime.stub.raiseUIRequest(surface: BDK_SURFACE_FIND, on: page)
                runtime.stub.pump()
                #expect(try runtime.stub.lastUIOutcome.kind == .rejected)

                try runtime.ui.find.setHandler { _ in .useDefault }
                try runtime.stub.raiseUIRequest(surface: BDK_SURFACE_FIND, on: page)
                runtime.stub.pump()
                #expect(try runtime.stub.lastUIOutcome.kind == .defaulted)

                try runtime.ui.find.setHandler(nil)
                #expect(!runtime.ui.find.hasHandler)
                try runtime.stub.raiseUIRequest(surface: BDK_SURFACE_FIND, on: page)
                runtime.stub.pump()
                #expect(try runtime.stub.lastUIOutcome.kind == .defaulted)
            }
        }

        @Test func aSurfaceWithoutAHandlerTakesTheDefault() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                try runtime.stub.raiseUIRequest(surface: BDK_SURFACE_PRINTING, on: page)
                runtime.stub.pump()
                #expect(try runtime.stub.lastUIOutcome.kind == .defaulted)
            }
        }

        @Test func modesComeFromTheResolvedConfiguration() async throws {
            let json = """
                {"surfaces":{"context_menus":"custom","notifications":"disabled","downloads":"default"}}
                """
            let configuration = BDKConfiguration(resolvedJSON: json)
            #expect(configuration.ui.contextMenus.mode == .default)  // not attached yet
            try configuration.ui.contextMenus.setHandler { _ in .useDefault }
            try await withStubRuntime(configuration) { runtime in
                #expect(runtime.ui.contextMenus.mode == .custom)
                #expect(runtime.ui.notifications.mode == .disabled)
                #expect(runtime.ui.downloads.mode == .default)
                #expect(runtime.ui.permissions.mode == .default)
            }
        }

        @Test func aDisabledSurfaceNeverReachesItsHandler() async throws {
            let box = UIBox()
            let configuration = BDKConfiguration(
                resolvedJSON: "{\"surfaces\":{\"notifications\":\"disabled\"}}")
            try configuration.ui.notifications.setHandler { request in
                box.request = request
                return .handled
            }
            try await withStubRuntime(configuration) { runtime in
                let profile = try await runtime.profiles.open("Disabled")
                let page = try profile.webContents.create()
                try runtime.stub.raiseUIRequest(surface: BDK_SURFACE_NOTIFICATIONS, on: page)
                runtime.stub.pump()
                #expect(box.request == nil)
                #expect(try runtime.stub.lastUIOutcome.kind == .disabled)
            }
        }

        @Test func aCustomSurfaceWithoutAHandlerFailsTheFreeze() async throws {
            let configuration = BDKConfiguration(
                resolvedJSON: "{\"surfaces\":{\"find\":\"custom\"}}")
            try await withStubRuntime(configuration, freezingRegistrations: false) { runtime in
                await expectError(.config, "handlerMissing") {
                    try runtime.freezeRegistrations()
                }
                try runtime.ui.find.setHandler { _ in .useDefault }
                try runtime.freezeRegistrations()
            }
        }
    }
}

/// A rejected or dropped interstitial goes back, which the router reports as that completion; any
/// other surface reports `otherwise`.
@MainActor
private func expectNeverGranted(
    _ fixture: UIFixture, _ surface: BDKUISurface, otherwise kind: BDKStub.UIOutcomeKind,
    sourceLocation: SourceLocation = #_sourceLocation
) throws {
    switch surface {
    case .certificateErrors, .reputationWarnings:
        try fixture.expectOutcome(.completed, "go_back", "{}", sourceLocation: sourceLocation)
    default:
        try fixture.expectOutcome(kind, sourceLocation: sourceLocation)
    }
}
