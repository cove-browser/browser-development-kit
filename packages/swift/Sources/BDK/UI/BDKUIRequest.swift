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
    /// The native view of a popup, side panel or hosted front end: its kind and address.
    var secureViewKind: UInt32
    var secureViewAddress: UInt

    init(_ info: bdk_ui_request_info_t) {
        handle = info.request
        surface = info.surface
        contents = info.contents
        window = info.window
        profile = info.profile
        payloadJSON = swiftString(info.payload_json)
        anchor = [info.anchor.0, info.anchor.1, info.anchor.2, info.anchor.3]
        secureViewKind = info.secure_view.kind
        secureViewAddress = info.secure_view.pointer.map { UInt(bitPattern: $0) } ?? 0
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
    /// The pages the runtime created for the request that the application was handed. A default
    /// presenter shows them, and the application may keep no reference to one, so once the request
    /// goes to it the runtime holds them while they are open.
    private var adoptedPages: [BDKWebContents] = []

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
        handedToDefaultPresenter()
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
        let native = gesture.map(gestureHandle)
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

    // MARK: Open requests

    /// Sends the application's change to a request that stays open (`bdk_ui_update`). A withdrawn
    /// request ignores it.
    func sendUpdate(_ fields: [String: BDKJSValue]) throws {
        switch state {
        case .withdrawn: return
        case .open: break
        default:
            throw BDKError(
                domain: .ui, code: "requestCompleted", message: "the request is already completed")
        }
        guard handle != 0 else { throw BDKError.stale(.request) }
        let json = BDKJSValue.object(fields).jsonString
        do {
            try withBDKString(json) { lent in
                try abiCall(.request) { bdk_ui_update(handle, lent, $0) }
            }
        } catch let error as BDKError where error.domain == .ui && error.code == "requestCompleted"
        {
            markWithdrawn()
        }
    }

    /// Receives the runtime's changes to an open request, in order, on the main actor. The runtime
    /// releases the callback when the request finishes.
    func observeUpdates(_ receive: @escaping @MainActor ([String: BDKJSValue]) -> Void) {
        let box = Unmanaged.passRetained(UpdateBox(receive))
        bdk_ui_set_update_handler(
            handle,
            { context, _, json in
                guard let context else { return }
                let box = Unmanaged<UpdateBox>.fromOpaque(context).takeUnretainedValue()
                let fields = BDKJSValue(jsonString: swiftString(json))?.objectValue ?? [:]
                MainActor.assumeIsolated { box.receive(fields) }
            }, box.toOpaque(),
            { context in
                guard let context else { return }
                Unmanaged<UpdateBox>.fromOpaque(context).release()
            })
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
        case .useDefault: handedToDefaultPresenter()
        case .reject: state = .rejected
        case .handled: return
        }
        DispatchQueue.main.async {
            MainActor.assumeIsolated { self.releaseHandle() }
        }
    }

    /// Wraps a page the runtime created for the request (a new window, a DevTools front end, a print
    /// preview).
    func adoptPage(handle: UInt64, in profile: BDKProfile) -> BDKWebContents {
        let page = BDKWebContents.adopt(handle: handle, in: profile)
        adoptedPages.append(page)
        if state == .defaulted { runtime.keepWhileOpen(page) }
        return page
    }

    private func handedToDefaultPresenter() {
        state = .defaulted
        for page in adoptedPages { runtime.keepWhileOpen(page) }
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

/// What the update callback holds. The runtime drops the box when the request finishes.
private final class UpdateBox: @unchecked Sendable {
    let receive: @MainActor ([String: BDKJSValue]) -> Void

    init(_ receive: @escaping @MainActor ([String: BDKJSValue]) -> Void) {
        self.receive = receive
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
