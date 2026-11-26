import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

/// What the web app handlers received and how they answer.
@MainActor
final class WebAppBox {
    var installs: [WebAppInstallRequest] = []
    var windows: [NewWindowRequest] = []
    /// What to do with an install prompt; the default answers nothing and holds the request.
    var onInstall: (WebAppInstallRequest) -> Void = { _ in }

    func confirmInstalls() {
        onInstall = { request in
            if let event = try? confirmingEvent() { try? request.install(confirmedBy: event) }
        }
    }
}

private let document = URL(string: "https://app.example/index.html")!
private let start = URL(string: "https://app.example/app/")!
private let manifest = """
    {"name":"Example","short_name":"Ex","start_url":"/app/","scope":"/app/",\
    "display":"minimal-ui","theme_color":"#336699",\
    "icons":[{"src":"/icon.png","sizes":"192x192","purpose":"maskable"}],\
    "shortcuts":[{"name":"New","url":"/app/new"}]}
    """
private let webAppsOn = "{\"web_apps\":{\"enabled\":true}}"

@MainActor
private func withWebApps(
    resolvedJSON: String = webAppsOn, handlers: Bool = true,
    _ body: (BDKRuntime, BDKProfile, WebAppBox) async throws -> Void
) async throws {
    let box = WebAppBox()
    let configuration = BDKConfiguration(resolvedJSON: resolvedJSON)
    if handlers {
        try configuration.ui.webAppInstalls.setHandler { request in
            box.installs.append(request)
            box.onInstall(request)
            return .handled
        }
        try configuration.ui.newWindows.setHandler { request in
            box.windows.append(request)
            request.accept()
            return .handled
        }
    }
    try await withStubRuntime(configuration) { runtime in
        try runtime.stub.registerWebAppManifest(documentURL: document, manifestJSON: manifest)
        let profile = try await runtime.profiles.open("Apps")
        try await body(runtime, profile, box)
    }
}

extension BDKStubTests {
    @MainActor
    @Suite("Web apps")
    struct WebAppTests {
        @Test func everyCallFailsWhileWebAppsAreOff() async throws {
            try await withWebApps(resolvedJSON: "{}") { runtime, profile, box in
                await expectError(.webApps, "disabled") { _ = try await profile.webApps.list() }
                await expectError(.webApps, "disabled") {
                    _ = try await profile.webApps.install(
                        .url(document), confirmedBy: confirmingEvent())
                }
                let page = try profile.webContents.create()
                try await page.navigation.load(document)
                await expectError(.webApps, "disabled") {
                    try runtime.stub.promptWebAppInstall(from: page)
                }
                #expect(box.installs.isEmpty)
                let info = try await runtime.about.info()
                #expect(!info.webApps.enabled && info.push.provider == BDKPushProvider.none)
            }
        }

        @Test func runtimeInfoReportsWebAppsAndPush() async throws {
            try await withWebApps { runtime, _, _ in
                let info = try await runtime.about.info()
                #expect(info.webApps.enabled && info.push.provider == BDKPushProvider.none)
            }
        }

        @Test func googlePushNeedsNoKey() async throws {
            // Chromium's push client registers with Google and reads no key (T-PWA-05).
            var keyed = BDKConfiguration(
                resolvedJSON: "{\"services\":{\"push\":{\"provider\":\"google\"}}}")
            keyed.services.google.apiKey = "AIzaTestKey"
            await expectError(.config, "keyUnused") { try BDKRuntime.startStub(keyed) }
            // A failed start shuts the native runtime down in the background; wait for that.
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

        @Test func anInstallWithAHandlerShowsTheOrigin() async throws {
            try await withWebApps { _, profile, box in
                box.confirmInstalls()
                let events = EventReader(profile.webApps.events)
                let info = try await profile.webApps.install(url: document)
                let prompt = try #require(box.installs.first)
                #expect(prompt.source == .command)
                #expect(prompt.app.name == "Example" && prompt.app.origin == "https://app.example")
                #expect(info.startUrl == start && info.displayMode == .minimalUi)
                #expect(info.icons.first?.purpose == .maskable && info.icons.first?.width == 192)
                #expect(info.shortcuts.map(\.name) == ["New"])
                #expect(try await profile.webApps.list().map(\.id) == [info.id])
                let installed = try await events.next {
                    if case .installed = $0 { true } else { false }
                }
                guard case .installed(let payload) = installed else { return }
                #expect(payload.app.id == info.id)
            }
        }

        @Test func anInstallWithNoHandlerInstallsNothing() async throws {
            try await withWebApps(handlers: false) { _, profile, _ in
                await expectError(.webApps, "cancelled") {
                    _ = try await profile.webApps.install(url: document)
                }
                let apps = try await profile.webApps.list()
                #expect(apps.isEmpty)
            }
        }

        @Test func aCancelledInstallInstallsNothing() async throws {
            try await withWebApps { _, profile, box in
                box.onInstall = { $0.cancel() }
                await expectError(.webApps, "cancelled") {
                    _ = try await profile.webApps.install(url: document)
                }
                let apps = try await profile.webApps.list()
                #expect(apps.isEmpty)
            }
        }

        @Test func installWithoutAGestureIsRefused() async throws {
            try await withWebApps { _, profile, box in
                var refusal: BDKError?
                box.onInstall = { request in
                    // A granting completion with no event: the core refuses it.
                    do { try request.submit("grant") } catch let error as BDKError {
                        refusal = error
                    } catch {}
                    request.cancel()
                }
                await expectError(.webApps, "cancelled") {
                    _ = try await profile.webApps.install(url: document)
                }
                #expect(refusal?.domain == .ui && refusal?.code == "gestureRequired")
            }
        }

        @Test func theApplicationInstallsAPageWithoutAPrompt() async throws {
            try await withWebApps { _, profile, box in
                let page = try profile.webContents.create()
                try await page.navigation.load(document)
                let check = try await profile.webApps.installability(of: page)
                #expect(check.isInstallable && check.installedId == nil)
                #expect(check.manifest?.name == "Example")
                let info = try await profile.webApps.install(
                    .page(page), confirmedBy: confirmingEvent())
                #expect(box.installs.isEmpty)
                #expect(try await profile.webApps.installability(of: page).installedId == info.id)
                try await profile.webApps.uninstall(info.id, confirmedBy: confirmingEvent())
                #expect(try await profile.webApps.list().isEmpty)
            }
        }

        @Test func aPagesPromptRaisesTheRequest() async throws {
            try await withWebApps { runtime, profile, box in
                box.confirmInstalls()
                let page = try profile.webContents.create()
                try await page.navigation.load(document)
                await expectError(.webApps, "gestureRequired") {
                    try runtime.stub.promptWebAppInstall(from: page, userGesture: false)
                }
                let events = EventReader(profile.webApps.events)
                try runtime.stub.promptWebAppInstall(from: page)
                _ = try await events.next {
                    if case .installed = $0 { true } else { false }
                }
                #expect(box.installs.first?.source == .page)
                #expect(box.installs.first?.contents === page)
            }
        }

        @Test func aPrivateProfileRefuses() async throws {
            try await withWebApps { runtime, _, box in
                box.confirmInstalls()
                let incognito = try runtime.profiles.openPrivate()
                #expect(try await incognito.webApps.list().isEmpty)
                await expectError(.webApps, "privateProfile") {
                    _ = try await incognito.webApps.install(url: document)
                }
                #expect(box.installs.isEmpty)
            }
        }

        @Test func theSameAppInTwoProfilesIsTwoInstalls() async throws {
            try await withWebApps { runtime, work, box in
                box.confirmInstalls()
                let home = try await runtime.profiles.open("Home")
                let first = try await work.webApps.install(url: document)
                let second = try await home.webApps.install(url: document)
                #expect(first.id == second.id && first.profile != second.profile)
                try await work.webApps.uninstall(first.id, confirmedBy: confirmingEvent())
                #expect(try await work.webApps.list().isEmpty)
                #expect(try await home.webApps.list().map(\.id) == [second.id])
            }
        }

        @Test func launchRaisesAWebAppWindowRequestAndTracksTheScope() async throws {
            try await withWebApps { runtime, profile, box in
                box.confirmInstalls()
                let info = try await profile.webApps.install(url: document)
                let page = try await profile.webApps.launch(info.id, shortcut: info.shortcuts.first)
                #expect(page.profile === profile)
                #expect(page.webApp?.app.id == info.id && page.webApp?.isInScope == true)
                try await waitUntil { !box.windows.isEmpty }
                let request = try #require(box.windows.first)
                #expect(request.kind == .webApp && request.webApp?.id == info.id)
                #expect(request.targetURL == URL(string: "https://app.example/app/new"))
                #expect(request.newContents === page)

                // Leaving the scope is reported, and coming back.
                try await page.navigation.load(URL(string: "https://elsewhere.example/")!)
                try await waitUntil { page.webApp?.isInScope == false }
                try await page.navigation.load(start)
                try await waitUntil { page.webApp?.isInScope == true }

                // An ordinary page at the start URL is not an app page.
                let tab = try profile.webContents.create()
                try await tab.navigation.load(start)
                #expect(tab.webApp == nil)
                _ = runtime
            }
        }

        @Test func badgesAndUninstallsAreEvents() async throws {
            try await withWebApps { runtime, profile, box in
                box.confirmInstalls()
                let info = try await profile.webApps.install(url: document)
                let page = try profile.webContents.create()
                try await page.navigation.load(start)
                let events = EventReader(profile.webApps.events)
                try runtime.stub.setWebAppBadge(
                    from: page, json: "{\"kind\":\"count\",\"count\":3}")
                let changed = try await events.next {
                    if case .badgeChanged = $0 { true } else { false }
                }
                guard case .badgeChanged(let payload) = changed else { return }
                #expect(payload.id == info.id && payload.badge?.count == 3)
                #expect(try await profile.webApps.get(info.id).badge?.kind == .count)
                try await profile.webApps.uninstall(info.id, confirmedBy: confirmingEvent())
                let gone = try await events.next {
                    if case .uninstalled = $0 { true } else { false }
                }
                guard case .uninstalled(let removed) = gone else { return }
                #expect(removed.id == info.id)
            }
        }
    }
}

/// Polls the main actor until the condition holds, for state that follows an event.
@MainActor
private func waitUntil(_ condition: () -> Bool) async throws {
    for _ in 0..<200 where !condition() {
        try await Task.sleep(for: .milliseconds(5))
    }
    #expect(condition())
}
