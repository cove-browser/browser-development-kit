import BDK
import CBDKStub
import Foundation

// MARK: - Page health

extension BDKStub {
    /// Acts as Chromium finding the page's renderer hung: an `UnresponsivePageRequest`, once per
    /// hang. `terminate()` then ends the renderer.
    public func makeUnresponsive(_ page: BDKWebContents) throws {
        try check(bdk_stub_page_unresponsive(page.handle))
    }

    /// Acts as the renderer answering again: the question is withdrawn.
    public func makeResponsive(_ page: BDKWebContents) throws {
        try check(bdk_stub_page_responsive(page.handle))
    }

    /// Acts as the page opening a popup without a gesture. Returns whether the popup blocker
    /// stopped it (a `BlockedPopupRequest` listing every popup blocked since the last answer);
    /// false when the site's popups are allowed and it opened as a new-window request.
    @discardableResult
    public func blockPopup(on page: BDKWebContents, url: URL) throws -> Bool {
        var blocked: UInt8 = 0
        try check(
            withBDKStrings([url.absoluteString]) {
                bdk_stub_block_popup(page.handle, $0[0], &blocked)
            })
        return blocked != 0
    }
}
