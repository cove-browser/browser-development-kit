import CBDK
import Foundation

/// What a menu item or toolbar button invokes on the focused page (swift-api.md section 9).
public enum PageCommand: Sendable, CaseIterable {
    case back, forward, reload, reloadBypassingCache, stop
    case cut, copy, paste, pasteAndMatchStyle, undo, redo, selectAll, delete
    case find, findNext, findPrevious
    case zoomIn, zoomOut, zoomReset
    case print, savePageAs, viewSource
    case toggleMute, toggleDevTools, toggleFullscreen

    /// The ABI's `BDK_PAGE_COMMAND_*`.
    var abiValue: UInt32 {
        switch self {
        case .back: UInt32(BDK_PAGE_COMMAND_BACK)
        case .forward: UInt32(BDK_PAGE_COMMAND_FORWARD)
        case .reload: UInt32(BDK_PAGE_COMMAND_RELOAD)
        case .reloadBypassingCache: UInt32(BDK_PAGE_COMMAND_RELOAD_BYPASSING_CACHE)
        case .stop: UInt32(BDK_PAGE_COMMAND_STOP)
        case .cut: UInt32(BDK_PAGE_COMMAND_CUT)
        case .copy: UInt32(BDK_PAGE_COMMAND_COPY)
        case .paste: UInt32(BDK_PAGE_COMMAND_PASTE)
        case .pasteAndMatchStyle: UInt32(BDK_PAGE_COMMAND_PASTE_AND_MATCH_STYLE)
        case .undo: UInt32(BDK_PAGE_COMMAND_UNDO)
        case .redo: UInt32(BDK_PAGE_COMMAND_REDO)
        case .selectAll: UInt32(BDK_PAGE_COMMAND_SELECT_ALL)
        case .delete: UInt32(BDK_PAGE_COMMAND_DELETE)
        case .find: UInt32(BDK_PAGE_COMMAND_FIND)
        case .findNext: UInt32(BDK_PAGE_COMMAND_FIND_NEXT)
        case .findPrevious: UInt32(BDK_PAGE_COMMAND_FIND_PREVIOUS)
        case .zoomIn: UInt32(BDK_PAGE_COMMAND_ZOOM_IN)
        case .zoomOut: UInt32(BDK_PAGE_COMMAND_ZOOM_OUT)
        case .zoomReset: UInt32(BDK_PAGE_COMMAND_ZOOM_RESET)
        case .print: UInt32(BDK_PAGE_COMMAND_PRINT)
        case .savePageAs: UInt32(BDK_PAGE_COMMAND_SAVE_PAGE_AS)
        case .viewSource: UInt32(BDK_PAGE_COMMAND_VIEW_SOURCE)
        case .toggleMute: UInt32(BDK_PAGE_COMMAND_TOGGLE_MUTE)
        case .toggleDevTools: UInt32(BDK_PAGE_COMMAND_TOGGLE_DEV_TOOLS)
        case .toggleFullscreen: UInt32(BDK_PAGE_COMMAND_TOGGLE_FULLSCREEN)
        }
    }
}

/// Where a zoom level applies.
public enum ZoomScope: Sendable, CaseIterable {
    /// This page only.
    case page
    /// Every page of the same site: Chromium's per-site level, kept by the profile.
    case site

    var abiValue: UInt32 {
        switch self {
        case .page: UInt32(BDK_ZOOM_SCOPE_PAGE)
        case .site: UInt32(BDK_ZOOM_SCOPE_SITE)
        }
    }
}

/// A page's zoom (swift-api.md section 6.7): `page.zoom`.
@MainActor
public struct ZoomControl {
    let contents: BDKWebContents

    /// The zoom factor now; 1.0 is 100 percent.
    public var factor: Double {
        contents.current.zoom
    }

    /// Sets the zoom for this page or for its site. `factor` is a factor, not a level, and must be
    /// positive (`broker.invalidArguments` otherwise).
    public func set(_ factor: Double, scope: ZoomScope) throws {
        let handle = try contents.liveHandle()
        try abiCall(.contents) {
            bdk_contents_set_zoom_scoped(handle, factor, scope.abiValue, $0)
        }
    }
}

/// A page's media (swift-api.md section 6.7): `page.media`.
@MainActor
public struct MediaControl {
    let contents: BDKWebContents

    /// Whether the page's audio is muted. The setter ignores a failure (a closed page); use
    /// `BDKWebContents.setMuted(_:)` to see it.
    public var isMuted: Bool {
        get { contents.isMuted }
        nonmutating set { try? contents.setMuted(newValue) }
    }

    /// Closes the page's picture-in-picture window, if it has one.
    public func exitPictureInPicture() throws {
        let handle = try contents.liveHandle()
        try abiCall(.contents) { bdk_contents_exit_picture_in_picture(handle, $0) }
    }
}

extension BDKWebContents {
    /// True when `command` would do something now, as Chromium would enable it. A closed page
    /// can do nothing.
    public func canPerform(_ command: PageCommand) -> Bool {
        guard let handle = try? liveHandle() else { return false }
        var enabled: UInt8 = 0
        var error: bdk_error_t = 0
        let status = bdk_contents_can_perform_command(handle, command.abiValue, &enabled, &error)
        EventLoop.kick()
        if error != 0 { bdk_error_release(error) }
        return status == BDK_OK && enabled != 0
    }

    /// Runs `command`. A command that cannot be performed now throws `contents.commandDisabled`;
    /// `find` and `print` raise the same UI requests as `find.present()` and `print()`.
    public func perform(_ command: PageCommand) throws {
        let handle = try liveHandle()
        try abiCall(.contents) { bdk_contents_perform_command(handle, command.abiValue, $0) }
    }

    public var zoom: ZoomControl {
        ZoomControl(contents: self)
    }

    public var media: MediaControl {
        MediaControl(contents: self)
    }
}
