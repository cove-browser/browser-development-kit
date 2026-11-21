import AppKit
import CBDK

/// A request that targets a window rather than a page, from an extension (`windows.update`) or a
/// page. Frames are in screen coordinates.
public struct WindowRequest: Sendable, Equatable {
    public enum Action: Sendable, Equatable {
        case setFrame(CGRect)
        case activate
        case minimize
        case zoom
        case enterFullscreen
        case exitFullscreen
        case close
    }

    public let action: Action
    /// The extension that asked, or nil when a page or BDK did.
    public let extensionID: String?

    init?(json: String) {
        guard let fields = BDKJSValue(jsonString: json)?.objectValue,
            let kind = fields["kind"]?.stringValue
        else { return nil }
        switch kind {
        case "setFrame":
            guard let frame = fields["frame"]?.objectValue else { return nil }
            func number(_ key: String) -> Double { frame[key]?.doubleValue ?? 0 }
            action = .setFrame(
                CGRect(
                    x: number("x"), y: number("y"), width: number("width"),
                    height: number("height")))
        case "activate": action = .activate
        case "minimize": action = .minimize
        case "zoom": action = .zoom
        case "enterFullscreen": action = .enterFullscreen
        case "exitFullscreen": action = .exitFullscreen
        case "close": action = .close
        default: return nil
        }
        extensionID = fields["initiator"]?.objectValue?["extensionId"]?.stringValue
    }
}

/// The application's side of window requests. Return `.useDefault` to let BDK apply the request to
/// the `NSWindow`.
@MainActor
public protocol BDKWindowDelegate: AnyObject {
    func window(_ window: BDKWindow, didReceive request: WindowRequest) -> BDKUIResponse
}

/// An application window known to the runtime (swift-api.md section 8.1).
@MainActor
public final class BDKWindow {
    public enum Kind: UInt32, Sendable {
        /// What extensions see in `windows.get` as `normal`.
        case normal = 0
        case popup = 1
    }

    public struct Options: Sendable {
        public var kind: Kind

        public init(kind: Kind = .normal) {
            self.kind = kind
        }
    }

    /// Where a default presenter attaches (`BDK_ANCHOR_*`).
    public enum AnchorRole: UInt32, Sendable, Hashable {
        /// Permission prompts, password and translation offers.
        case addressBar = 1
        /// The default extensions menu.
        case extensions = 2
        /// The downloads popover.
        case downloads = 3
    }

    /// Nil once the window was deallocated.
    public private(set) weak var nsWindow: NSWindow?
    public let kind: Kind
    /// Each role's rectangle in the window's own coordinates (the ones `NSView.convert(_:to: nil)`
    /// gives, origin at the bottom left). A role with no entry, or an empty rectangle, has no anchor.
    public var anchors: [AnchorRole: @MainActor () -> CGRect] = [:]
    public weak var delegate: (any BDKWindowDelegate)?

    package private(set) var handle: bdk_window_t
    private unowned let runtime: BDKRuntime

    init(_ nsWindow: NSWindow, kind: Kind, handle: bdk_window_t, runtime: BDKRuntime) {
        self.nsWindow = nsWindow
        self.kind = kind
        self.handle = handle
        self.runtime = runtime
    }

    // MARK: Queries

    /// The pages attached or last attached to the window, in the order they arrived.
    public var pages: [BDKWebContents] {
        guard handle != 0 else { return [] }
        var buffer = bdk_buffer_t()
        guard (try? abiCall(.runtime) { bdk_window_contents(handle, &buffer, $0) }) != nil else {
            return []
        }
        let handles = BDKJSValue(jsonString: String(decoding: takeBuffer(buffer), as: UTF8.self))
        return (handles?.arrayValue ?? []).compactMap {
            BDKUIPayload.handle($0).flatMap { runtime.webContents(handle: $0) }
        }
    }

    /// The page extensions see as the window's active tab.
    public var activePage: BDKWebContents? {
        guard handle != 0 else { return nil }
        var contents: bdk_contents_t = 0
        guard (try? abiCall(.runtime) { bdk_window_active_contents(handle, &contents, $0) }) != nil
        else { return nil }
        return runtime.webContents(handle: contents)
    }

    /// The identifier extensions use for the window, or nil before it shows a page of a profile
    /// with extensions.
    public var extensionWindowID: Int? {
        guard handle != 0 else { return nil }
        var identifier: Int32 = -1
        guard (try? abiCall(.runtime) { bdk_window_extension_id(handle, &identifier, $0) }) != nil
        else { return nil }
        return identifier < 0 ? nil : Int(identifier)
    }

    // MARK: Callbacks

    /// Installs the anchor provider and the request handler, once, at registration.
    func installCallbacks() throws {
        let anchorBox = Unmanaged.passRetained(WindowBox(self))
        do {
            try abiCall(.runtime) {
                bdk_window_set_anchor_provider(
                    handle, anchorCallback, anchorBox.toOpaque(), windowBoxRelease, $0)
            }
        } catch {
            anchorBox.release()
            throw error
        }
        let requestBox = Unmanaged.passRetained(WindowBox(self))
        do {
            try abiCall(.runtime) {
                bdk_window_set_request_handler(
                    handle, requestCallback, requestBox.toOpaque(), windowBoxRelease, $0)
            }
        } catch {
            requestBox.release()
            throw error
        }
    }

    /// The anchor in the ABI's window coordinates (origin at the top left), nil for none.
    fileprivate func anchor(for role: UInt32) -> CGRect? {
        guard let role = AnchorRole(rawValue: role), let provide = anchors[role],
            let frame = nsWindow?.frame
        else { return nil }
        let rect = provide()
        guard !rect.isEmpty, !rect.isNull, !rect.isInfinite else { return nil }
        return CGRect(
            x: rect.minX, y: frame.height - rect.maxY, width: rect.width, height: rect.height)
    }

    fileprivate func answer(_ json: String) -> bdk_ui_response_t {
        guard let request = WindowRequest(json: json), let delegate else {
            return BDK_UI_USE_DEFAULT
        }
        switch delegate.window(self, didReceive: request) {
        case .handled: return BDK_UI_HANDLED
        case .useDefault: return BDK_UI_USE_DEFAULT
        case .reject: return BDK_UI_REJECT
        }
    }

    // MARK: Lifetime

    /// The native window closed.
    func unregistered() {
        guard handle != 0 else { return }
        // A stale handle means the runtime shut down first and already released it.
        try? abiCall(.runtime) { bdk_window_unregister(handle, $0) }
        handle = 0
    }

    /// The runtime released the handle.
    func invalidate() {
        handle = 0
    }
}

extension BDKWebContents {
    /// The registered window the page is attached to, or was last attached to.
    public var window: BDKWindow? {
        guard let handle = try? liveHandle() else { return nil }
        var window: bdk_window_t = 0
        guard (try? abiCall(.contents) { bdk_window_for_contents(handle, &window, $0) }) != nil
        else { return nil }
        return profile.runtime.windows.bdkWindow(handle: window)
    }
}

/// What a window callback's context holds: the window, weakly, so the runtime keeps nothing alive.
private final class WindowBox: @unchecked Sendable {
    weak var window: BDKWindow?

    init(_ window: BDKWindow) {
        self.window = window
    }
}

private let windowBoxRelease: bdk_release_cb = { context in
    guard let context else { return }
    Unmanaged<WindowBox>.fromOpaque(context).release()
}

private let anchorCallback: bdk_anchor_cb = { context, _, role, out in
    guard let context, let out else { return 0 }
    let box = Unmanaged<WindowBox>.fromOpaque(context).takeUnretainedValue()
    let rect = MainActor.assumeIsolated { box.window?.anchor(for: role) }
    guard let rect else { return 0 }
    out[0] = rect.minX
    out[1] = rect.minY
    out[2] = rect.width
    out[3] = rect.height
    return 1
}

private let requestCallback: bdk_window_request_cb = { context, _, json in
    guard let context else { return BDK_UI_USE_DEFAULT }
    let box = Unmanaged<WindowBox>.fromOpaque(context).takeUnretainedValue()
    let text = swiftString(json)
    return MainActor.assumeIsolated { box.window?.answer(text) ?? BDK_UI_USE_DEFAULT }
}
