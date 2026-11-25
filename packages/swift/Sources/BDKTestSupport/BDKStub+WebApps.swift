import BDK
import CBDK
import CBDKStub
import Foundation

// MARK: - Web apps

extension BDKStub {
    /// Registers the manifest a page at `documentURL` links (web-apps.md): what installability,
    /// `install` and the page's prompt read. Relative URLs in it resolve against `documentURL`.
    public func registerWebAppManifest(documentURL: URL, manifestJSON: String) throws {
        let handle = try runtime.liveHandle()
        try check(
            withBDKStrings([documentURL.absoluteString, manifestJSON]) {
                bdk_stub_web_app_manifest(handle, $0[0], $0[1])
            })
    }

    /// Acts as the page calling `prompt()` on its `beforeinstallprompt` event: raises a
    /// `web_app_installs` request with source `.page`. Throws `webApps.disabled` while web apps are
    /// off, `webApps.privateProfile`, `webApps.gestureRequired` without `userGesture` and
    /// `webApps.notInstallable`.
    public func promptWebAppInstall(from page: BDKWebContents, userGesture: Bool = true) throws {
        let handle = page.handle
        var error: bdk_error_t = 0
        try checkError(bdk_stub_web_app_prompt(handle, userGesture ? 1 : 0, &error), error)
    }

    /// Acts as the page calling `navigator.setAppBadge` (`{"kind":"count","count":3}`,
    /// `{"kind":"flag"}`) or `clearAppBadge` (`"null"`).
    public func setWebAppBadge(from page: BDKWebContents, json: String) throws {
        let handle = page.handle
        var error: bdk_error_t = 0
        let status = withBDKString(json) { bdk_stub_web_app_set_badge(handle, $0, &error) }
        try checkError(status, error)
    }

    /// Throws the error the stub filled in, or `check(status)`'s when it filled in none.
    func checkError(_ status: bdk_status_t, _ error: bdk_error_t) throws {
        defer {
            if error != 0 { bdk_error_release(error) }
        }
        guard status != BDK_OK, error != 0 else { return try check(status) }
        func string(_ value: bdk_string_t) -> String {
            guard let data = value.data else { return "" }
            return String(
                decoding: UnsafeRawBufferPointer(start: data, count: value.length), as: UTF8.self)
        }
        throw BDKError(
            domain: BDKError.Domain(rawValue: string(bdk_error_domain(error))),
            code: string(bdk_error_code(error)), message: string(bdk_error_message(error)))
    }
}
