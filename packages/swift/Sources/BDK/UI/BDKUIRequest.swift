import AppKit
import CBDK
import Foundation

/// What a handler answers for a request (swift-api.md section 10).
public enum BDKUIResponse: Sendable, Equatable {
    /// The application owns the request and completes it through the request object.
    case handled
    /// BDK's default presenter shows it.
    case useDefault
    /// Deny or cancel, where that is safe.
    case reject
}

/// A browser-owned surface (ui-requests.md section 4). The raw value is `BDK_SURFACE_*`.
public enum BDKUISurface: UInt32, Sendable, CaseIterable {
    case permissions = 1
    case javascriptDialogs = 2
    case httpAuth = 3
    case clientCertificates = 4
    case filePickers = 5
    case contextMenus = 6
    case newWindows = 7
    case downloads = 8
    case passwords = 9
    case autofill = 10
    case autofillSuggestions = 11
    case printing = 12
    case translation = 13
    case find = 14
    case fullscreen = 15
    case exclusiveAccess = 16
    case notifications = 17
    case certificateErrors = 18
    case reputationWarnings = 19
    case devTools = 20
    case deviceChoosers = 21
    case screenCapture = 22
    case webAuthn = 23
    case externalProtocols = 24

    /// The key under `[ui.surfaces]` in `bdk.toml` and in `bdk.resolved.json`.
    public var configurationKey: String {
        switch self {
        case .permissions: "permissions"
        case .javascriptDialogs: "javascript_dialogs"
        case .httpAuth: "http_auth"
        case .clientCertificates: "client_certificates"
        case .filePickers: "file_pickers"
        case .contextMenus: "context_menus"
        case .newWindows: "new_windows"
        case .downloads: "downloads"
        case .passwords: "passwords"
        case .autofill: "autofill"
        case .autofillSuggestions: "autofill_suggestions"
        case .printing: "printing"
        case .translation: "translation"
        case .find: "find"
        case .fullscreen: "fullscreen"
        case .exclusiveAccess: "exclusive_access"
        case .notifications: "notifications"
        case .certificateErrors: "certificate_errors"
        case .reputationWarnings: "reputation_warnings"
        case .devTools: "devtools"
        case .deviceChoosers: "device_choosers"
        case .screenCapture: "screen_capture"
        case .webAuthn: "webauthn"
        case .externalProtocols: "external_protocols"
        }
    }
}

/// The mode of a surface, from `[ui.surfaces]` (configuration.md section 3.6).
public enum BDKUISurfaceMode: String, Sendable, Equatable {
    case `default`
    case custom
    case disabled
}

/// Fires when Chromium withdraws a request: the page navigated, the page closed, or the request
/// was superseded. The application then dismisses its own UI.
@MainActor
public final class BDKUICancellation {
    public private(set) var isFired = false
    private var actions: [@MainActor () -> Void] = []
    private var waiters: [CheckedContinuation<Void, Never>] = []

    /// Runs `action` when the request is withdrawn, or at once if it already was.
    public func onFire(_ action: @escaping @MainActor () -> Void) {
        if isFired {
            action()
        } else {
            actions.append(action)
        }
    }

    /// Returns when the request is withdrawn.
    public func wait() async {
        if isFired { return }
        await withCheckedContinuation { waiters.append($0) }
    }

    func fire() {
        guard !isFired else { return }
        isFired = true
        let pending = actions
        let blocked = waiters
        actions = []
        waiters = []
        for action in pending { action() }
        for waiter in blocked { waiter.resume() }
    }
}

/// Where a request came from.
public enum BDKUIInitiator: Sendable, Equatable {
    case page(origin: URL)
    case `extension`(id: String)
    case browser
}

/// The fields every delivered request shares, read once from the ABI struct. It holds no borrowed
/// memory, so it can cross into the main actor.
struct BDKUIDelivery: Sendable {
    var handle: bdk_request_t
    var surface: UInt32
    var contents: bdk_contents_t
    var window: bdk_window_t
    var profile: bdk_profile_t
    var payloadJSON: String
    var anchor: [Double]

    init(_ info: bdk_ui_request_info_t) {
        handle = info.request
        surface = info.surface
        contents = info.contents
        window = info.window
        profile = info.profile
        payloadJSON = swiftString(info.payload_json)
        anchor = [info.anchor.0, info.anchor.1, info.anchor.2, info.anchor.3]
    }
}

/// The base of every UI request (ui-requests.md section 2). A request is completed once, through
/// the methods of its subclass. One the application took and then released without completing is
/// cancelled, never granted.
@MainActor
public class BDKUIRequest {
    public struct ID: Hashable, Sendable {
        public let rawValue: UInt64
    }

    enum State {
        case open
        case completed
        case defaulted
        case rejected
        case withdrawn
    }

    public let id: ID
    public let surface: BDKUISurface
    /// The page it concerns, if any.
    public let contents: BDKWebContents?
    /// The window it should appear in, when the runtime knows an application window for it.
    public let window: NSWindow?
    /// The profile it concerns. Nil when the runtime names none.
    public let profile: BDKProfile?
    public let initiator: BDKUIInitiator
    /// In window coordinates, when the request has a natural position.
    public let anchor: CGRect?
    /// The payload as the runtime delivered it, for fields this version does not model.
    public let payload: [String: BDKJSValue]
    /// Fires if Chromium withdraws the request.
    public let cancellation = BDKUICancellation()

    private(set) var state = State.open
    private var handle: bdk_request_t
    let runtime: BDKRuntime

    /// True once the request was completed, handed to the default presenter, rejected or withdrawn.
    public var isCompleted: Bool {
        state != .open
    }

    /// True when Chromium withdrew the request. Completing a withdrawn request does nothing.
    public var isWithdrawn: Bool {
        state == .withdrawn
    }

    init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKJSValue(jsonString: delivery.payloadJSON)?.objectValue ?? [:]
        id = ID(rawValue: delivery.handle)
        surface = BDKUISurface(rawValue: delivery.surface) ?? .permissions
        handle = delivery.handle
        payload = fields
        self.runtime = runtime
        contents = runtime.webContents(handle: delivery.contents)
        window = runtime.windows.window(handle: delivery.window)
        profile = runtime.profiles.loaded.first { $0.handle == delivery.profile }
        initiator = BDKUIInitiator(payload: fields)
        let rect = delivery.anchor
        anchor =
            rect.contains { $0 != 0 }
            ? CGRect(x: rect[0], y: rect[1], width: rect[2], height: rect[3]) : nil
    }

    isolated deinit {
        releaseHandle()
    }

    // MARK: Completion

    /// Hands the request back to BDK's default presenter, after the handler took it.
    public func useDefaultPresenter() {
        guard state == .open, handle != 0 else { return }
        try? abiCall(.request) { bdk_ui_use_default(handle, $0) }
        state = .defaulted
        releaseHandle()
    }

    /// Completes the request once. `gesture` is the event that confirmed a granting completion.
    func submit(
        _ completion: String, _ arguments: [String: BDKJSValue] = [:], gesture: NSEvent? = nil
    ) throws {
        switch state {
        case .withdrawn: return
        case .open: break
        default:
            throw BDKError(
                domain: .ui, code: "requestCompleted", message: "the request is already completed")
        }
        guard handle != 0 else { throw BDKError.stale(.request) }
        let native = gesture.map {
            bdk_native_handle_t(
                kind: BDK_NATIVE_NSEVENT.rawValue,
                pointer: Unmanaged.passUnretained($0).toOpaque(), id: UInt64(max($0.timestamp, 0)))
        }
        let json = BDKJSValue.object(arguments).jsonString
        do {
            try withBDKStrings([completion, json]) { lent in
                try abiCall(.request) {
                    bdk_ui_complete(handle, lent[0], lent[1], native ?? bdk_native_handle_t(), $0)
                }
            }
        } catch let error as BDKError where error.domain == .ui && error.code == "requestCompleted"
        {
            // Only Chromium can have finished a request this object has not: it was withdrawn and
            // the notification has not arrived yet.
            markWithdrawn()
            return
        }
        state = .completed
        releaseHandle()
    }

    // MARK: Delivery

    /// Registers for withdrawal. Called from the delivery callback, before the handler runs.
    func observeWithdrawal() {
        let box = Unmanaged.passRetained(WithdrawalBox(self))
        bdk_ui_set_withdrawn_handler(
            handle,
            { context in
                guard let context else { return }
                let box = Unmanaged<WithdrawalBox>.fromOpaque(context).takeUnretainedValue()
                MainActor.assumeIsolated { box.request?.markWithdrawn() }
            }, box.toOpaque(),
            { context in
                guard let context else { return }
                Unmanaged<WithdrawalBox>.fromOpaque(context).release()
            })
    }

    /// The handler answered `.useDefault` or `.reject`: the runtime finishes the request when the
    /// callback returns, so the handle is released after that.
    func finishedByResponse(_ response: BDKUIResponse) {
        guard state == .open else { return }
        switch response {
        case .useDefault: state = .defaulted
        case .reject: state = .rejected
        case .handled: return
        }
        DispatchQueue.main.async {
            MainActor.assumeIsolated { self.releaseHandle() }
        }
    }

    func markWithdrawn() {
        guard state == .open else { return }
        state = .withdrawn
        releaseHandle()
        cancellation.fire()
    }

    /// Releases the handle. An unfinished request is cancelled by the release.
    private func releaseHandle() {
        guard handle != 0 else { return }
        bdk_ui_request_release(handle)
        handle = 0
        EventLoop.kick()
    }
}

/// What the withdrawal callback holds: nothing that keeps the request alive.
private final class WithdrawalBox: @unchecked Sendable {
    weak var request: BDKUIRequest?

    init(_ request: BDKUIRequest) {
        self.request = request
    }
}

extension BDKUIInitiator {
    init(payload: [String: BDKJSValue]) {
        if let initiator = payload["initiator"]?.objectValue {
            if let id = initiator["extension"]?.stringValue ?? initiator["extensionId"]?.stringValue
            {
                self = .extension(id: id)
            } else if let origin = initiator["origin"]?.stringValue.flatMap(URL.init(string:)) {
                self = .page(origin: origin)
            } else {
                self = .browser
            }
        } else if let origin = payload["origin"]?.stringValue.flatMap(URL.init(string:)) {
            self = .page(origin: origin)
        } else {
            self = .browser
        }
    }
}
