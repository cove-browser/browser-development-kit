import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

/// What the handlers of an `ExtensionFixture` received and how they answer.
@MainActor
final class ExtensionBox {
    var installs: [ExtensionInstallRequest] = []
    var uninstalls: [ExtensionUninstallRequest] = []
    var popups: [ExtensionPopupRequest] = []
    var panels: [ExtensionSidePanelRequest] = []
    /// What to do with an install prompt; the default answers nothing and holds the request.
    var onInstall: (ExtensionInstallRequest) -> Void = { _ in }
    var onUninstall: (ExtensionUninstallRequest) -> Void = { _ in }

    /// Accepts every install prompt with a click.
    func confirmInstalls() {
        onInstall = { request in
            if let event = try? confirmingEvent() { try? request.accept(confirmedBy: event) }
        }
    }

    func confirmUninstalls() {
        onUninstall = { request in
            if let event = try? confirmingEvent() { try? request.confirm(confirmedBy: event) }
        }
    }
}

/// A running stub runtime with a profile, a page and handlers on the four extension surfaces.
@MainActor
struct ExtensionFixture {
    let runtime: BDKRuntime
    let profile: BDKProfile
    let page: BDKWebContents
    let box: ExtensionBox

    var extensions: BDKExtensions {
        profile.extensions
    }

    /// A manifest a package can be registered with. `extra` is more members, with a leading comma.
    static func manifest(
        name: String = "Test extension", version: String = "1.0", permissions: [String] = [],
        extra: String = ""
    ) -> String {
        let list = permissions.map { "\"\($0)\"" }.joined(separator: ",")
        return """
            {"manifest_version":3,"name":"\(name)","version":"\(version)",\
            "permissions":[\(list)]\(extra)}
            """
    }

    static let popupAction = ",\"action\":{\"default_title\":\"Open\",\"default_popup\":\"p.html\"}"
    static let plainAction = ",\"action\":{\"default_title\":\"Click\"}"

    /// Registers an unpacked package and installs it with the install prompt accepted.
    @discardableResult
    func install(
        directory: String = "/tmp/bdk-ext-a", manifest: String = manifest(extra: plainAction)
    ) async throws -> BDKExtensionInfo {
        try runtime.stub.registerExtensionPackage(
            kind: "unpacked", key: directory, manifestJSON: manifest)
        box.confirmInstalls()
        return try await extensions.install(.unpacked(URL(fileURLWithPath: directory)))
    }
}

@MainActor
func withExtensionFixture(
    resolvedJSON: String = "{}", _ body: (ExtensionFixture) async throws -> Void
) async throws {
    let box = ExtensionBox()
    let configuration = BDKConfiguration(resolvedJSON: resolvedJSON)
    try configuration.ui.extensionInstalls.setHandler { request in
        box.installs.append(request)
        box.onInstall(request)
        return .handled
    }
    try configuration.ui.extensionUninstalls.setHandler { request in
        box.uninstalls.append(request)
        box.onUninstall(request)
        return .handled
    }
    try configuration.ui.extensionPopups.setHandler { request in
        box.popups.append(request)
        return .handled
    }
    try configuration.ui.extensionSidePanels.setHandler { request in
        box.panels.append(request)
        return .handled
    }
    try await withStubRuntime(configuration) { runtime in
        let profile = try await runtime.profiles.open("Extensions")
        let page = try profile.webContents.create()
        try await body(ExtensionFixture(runtime: runtime, profile: profile, page: page, box: box))
    }
}
