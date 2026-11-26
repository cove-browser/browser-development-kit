import CBDK
import Foundation

/// The handlers of the UI-request surfaces (swift-api.md section 10).
///
/// `BDKConfiguration.ui` collects handlers before start; `BDKRuntime.ui` is what the runtime
/// delivers to. A configuration's handlers are copied into the runtime at start and installed
/// before the registrations freeze (`bdk_ui_set_handler` is refused afterwards with
/// `config.registrationAfterStart`). After the freeze a handler can be replaced or cleared on a
/// surface that had one at start; installing one on another surface throws
/// `config.registrationAfterStart`. A cleared handler means "use the default presenter".
@MainActor
public final class BDKUIHandlers {
    /// One surface. `Request` is the type the handler receives.
    @MainActor
    public struct Surface<Request: BDKUIRequest> {
        fileprivate let owner: BDKUIHandlers
        fileprivate let kind: BDKUISurface

        /// From `bdk.toml`. `.default` for a configuration that is not attached to a runtime.
        public var mode: BDKUISurfaceMode {
            owner.modes[kind] ?? .default
        }

        public var hasHandler: Bool {
            owner.handlers[kind] != nil
        }

        /// Sets or clears the handler. Throws `config.registrationAfterStart` when the surface
        /// needs a registration with the runtime and the registrations are frozen.
        public func setHandler(_ handler: (@MainActor (Request) -> BDKUIResponse)?) throws {
            let erased = handler.map { BDKUIHandlers.erase($0) }
            try owner.setHandler(kind, erased)
        }
    }

    fileprivate static func erase<Request: BDKUIRequest>(
        _ typed: @escaping @MainActor (Request) -> BDKUIResponse
    ) -> Erased {
        { request in
            guard let request = request as? Request else { return .useDefault }
            return typed(request)
        }
    }

    typealias Erased = @MainActor (BDKUIRequest) -> BDKUIResponse

    private var handlers: [BDKUISurface: Erased] = [:]
    private var installed: Set<BDKUISurface> = []
    fileprivate var modes: [BDKUISurface: BDKUISurfaceMode] = [:]
    private weak var runtime: BDKRuntime?

    public nonisolated init() {}

    public var permissions: Surface<PermissionRequest> { surface(.permissions) }
    public var javascriptDialogs: Surface<JavaScriptDialogRequest> { surface(.javascriptDialogs) }
    public var httpAuth: Surface<HTTPAuthRequest> { surface(.httpAuth) }
    public var clientCertificates: Surface<ClientCertificateRequest> {
        surface(.clientCertificates)
    }
    public var filePickers: Surface<FilePickerRequest> { surface(.filePickers) }
    public var contextMenus: Surface<ContextMenuRequest> { surface(.contextMenus) }
    public var newWindows: Surface<NewWindowRequest> { surface(.newWindows) }
    /// Receives `DownloadRequest` and `DangerousDownloadRequest`.
    public var downloads: Surface<BDKUIRequest> { surface(.downloads) }
    /// Receives `PasswordSaveRequest` and `LeakedPasswordRequest`.
    public var passwords: Surface<BDKUIRequest> { surface(.passwords) }
    public var autofill: Surface<AutofillSaveRequest> { surface(.autofill) }
    public var autofillSuggestions: Surface<AutofillSuggestionsRequest> {
        surface(.autofillSuggestions)
    }
    public var printing: Surface<PrintRequest> { surface(.printing) }
    public var translation: Surface<TranslationRequest> { surface(.translation) }
    public var find: Surface<FindRequest> { surface(.find) }
    public var fullscreen: Surface<FullscreenRequest> { surface(.fullscreen) }
    public var exclusiveAccess: Surface<ExclusiveAccessRequest> { surface(.exclusiveAccess) }
    public var notifications: Surface<NotificationRequest> { surface(.notifications) }
    /// Receives `CertificateErrorRequest`.
    public var certificateErrors: Surface<CertificateErrorRequest> { surface(.certificateErrors) }
    public var reputationWarnings: Surface<ReputationWarningRequest> {
        surface(.reputationWarnings)
    }
    public var devTools: Surface<DevToolsRequest> { surface(.devTools) }
    /// Receives `DeviceChooserRequest`.
    public var deviceChoosers: Surface<DeviceChooserRequest> { surface(.deviceChoosers) }
    /// Receives `ScreenCaptureRequest`.
    public var screenCapture: Surface<ScreenCaptureRequest> { surface(.screenCapture) }
    /// One request per step of a passkey or security-key ceremony (`WebAuthnRequest`).
    public var webAuthn: Surface<WebAuthnRequest> { surface(.webAuthn) }
    public var externalProtocols: Surface<ExternalProtocolRequest> { surface(.externalProtocols) }
    public var extensionPopups: Surface<ExtensionPopupRequest> { surface(.extensionPopups) }
    public var extensionSidePanels: Surface<ExtensionSidePanelRequest> {
        surface(.extensionSidePanels)
    }
    public var extensionInstalls: Surface<ExtensionInstallRequest> { surface(.extensionInstalls) }
    public var extensionUninstalls: Surface<ExtensionUninstallRequest> {
        surface(.extensionUninstalls)
    }
    /// Receives `UnresponsivePageRequest` and `BlockedPopupRequest`.
    public var pageHealth: Surface<BDKUIRequest> { surface(.pageHealth) }
    public var webAppInstalls: Surface<WebAppInstallRequest> { surface(.webAppInstalls) }

    private func surface<Request: BDKUIRequest>(_ kind: BDKUISurface) -> Surface<Request> {
        Surface(owner: self, kind: kind)
    }

    // MARK: Registration

    /// Sets the handler of a surface without naming its request type. For tests and code that
    /// handles every surface alike.
    package func setHandler(
        for kind: BDKUISurface, _ handler: (@MainActor (BDKUIRequest) -> BDKUIResponse)?
    ) throws {
        try setHandler(kind, handler)
    }

    private func setHandler(_ kind: BDKUISurface, _ handler: Erased?) throws {
        if handler != nil, let runtime, !installed.contains(kind) {
            try install(kind, on: runtime)
        }
        handlers[kind] = handler
    }

    /// Adopts the configuration's handlers and installs them with the runtime. Called between
    /// `bdk_runtime_start` and the freeze.
    func attach(to runtime: BDKRuntime, from configuration: BDKConfiguration) throws {
        self.runtime = runtime
        handlers = configuration.ui.handlers
        modes = Self.modes(inResolvedJSON: configuration.resolvedJSON)
        for kind in BDKUISurface.allCases where handlers[kind] != nil {
            try install(kind, on: runtime)
        }
    }

    private func install(_ kind: BDKUISurface, on runtime: BDKRuntime) throws {
        let handle = try runtime.liveHandle()
        let box = Unmanaged.passRetained(UIDispatchBox(owner: self, kind: kind))
        do {
            try abiCall(.runtime) {
                bdk_ui_set_handler(
                    handle, kind.rawValue, uiDispatchCallback, box.toOpaque(), uiReleaseCallback, $0
                )
            }
        } catch {
            // A refused registration leaves the context with the caller.
            box.release()
            throw error
        }
        installed.insert(kind)
    }

    // MARK: Delivery

    fileprivate func deliver(_ delivery: BDKUIDelivery, kind: BDKUISurface) -> bdk_ui_response_t {
        guard let runtime else { return BDK_UI_USE_DEFAULT }
        // The request object always exists: it owns the handle, which the runtime frees only for
        // a request it never delivered.
        let request = BDKUIRequest.make(delivery, runtime: runtime)
        request.observeWithdrawal()
        guard let handler = handlers[kind] else {
            request.finishedByResponse(.useDefault)
            return BDK_UI_USE_DEFAULT
        }
        let response = handler(request)
        request.finishedByResponse(response)
        switch response {
        case .handled: return BDK_UI_HANDLED
        case .useDefault: return BDK_UI_USE_DEFAULT
        case .reject: return BDK_UI_REJECT
        }
    }

    private static func modes(inResolvedJSON json: String) -> [BDKUISurface: BDKUISurfaceMode] {
        let root = BDKJSValue(jsonString: json)?.objectValue
        let surfaces = root?["surfaces"]?.objectValue ?? [:]
        let extensionSurfaces = root?["extensions"]?.objectValue?["ui"]?.objectValue ?? [:]
        var modes: [BDKUISurface: BDKUISurfaceMode] = [:]
        for kind in BDKUISurface.allCases {
            let table = kind.isConfiguredWithExtensions ? extensionSurfaces : surfaces
            if let name = table[kind.configurationKey]?.stringValue,
                let mode = BDKUISurfaceMode(rawValue: name)
            {
                modes[kind] = mode
            }
        }
        return modes
    }
}

/// The context the runtime keeps for one surface's handler.
@MainActor
private final class UIDispatchBox {
    weak var owner: BDKUIHandlers?
    let kind: BDKUISurface

    init(owner: BDKUIHandlers, kind: BDKUISurface) {
        self.owner = owner
        self.kind = kind
    }

    func dispatch(_ delivery: BDKUIDelivery) -> bdk_ui_response_t {
        owner?.deliver(delivery, kind: kind) ?? BDK_UI_USE_DEFAULT
    }
}

private let uiDispatchCallback: bdk_ui_handler_cb = { context, info in
    guard let context, let info else { return BDK_UI_USE_DEFAULT }
    let box = Unmanaged<UIDispatchBox>.fromOpaque(context).takeUnretainedValue()
    let delivery = BDKUIDelivery(info.pointee)
    return MainActor.assumeIsolated { box.dispatch(delivery) }
}

private let uiReleaseCallback: bdk_release_cb = { context in
    guard let context else { return }
    Unmanaged<UIDispatchBox>.fromOpaque(context).release()
}

extension BDKUIRequest {
    private typealias Factory = @MainActor (BDKUIDelivery, BDKRuntime) -> BDKUIRequest

    /// The request type each surface delivers. Surfaces with two kinds choose from the payload.
    private static let factories: [BDKUISurface: Factory] = [
        .permissions: { PermissionRequest($0, runtime: $1) },
        .javascriptDialogs: { JavaScriptDialogRequest($0, runtime: $1) },
        .httpAuth: { HTTPAuthRequest($0, runtime: $1) },
        .clientCertificates: { ClientCertificateRequest($0, runtime: $1) },
        .filePickers: { FilePickerRequest($0, runtime: $1) },
        .contextMenus: { ContextMenuRequest($0, runtime: $1) },
        .newWindows: { NewWindowRequest($0, runtime: $1) },
        .downloads: {
            BDKUIPayload($0).fields["danger"] == nil
                ? DownloadRequest($0, runtime: $1) : DangerousDownloadRequest($0, runtime: $1)
        },
        .passwords: {
            BDKUIPayload($0).string("kind") == "leaked"
                ? LeakedPasswordRequest($0, runtime: $1) : PasswordSaveRequest($0, runtime: $1)
        },
        .autofill: { AutofillSaveRequest($0, runtime: $1) },
        .autofillSuggestions: { AutofillSuggestionsRequest($0, runtime: $1) },
        .printing: { PrintRequest($0, runtime: $1) },
        .translation: { TranslationRequest($0, runtime: $1) },
        .find: { FindRequest($0, runtime: $1) },
        .fullscreen: { FullscreenRequest($0, runtime: $1) },
        .exclusiveAccess: { ExclusiveAccessRequest($0, runtime: $1) },
        .notifications: { NotificationRequest($0, runtime: $1) },
        .certificateErrors: { CertificateErrorRequest($0, runtime: $1) },
        .reputationWarnings: { ReputationWarningRequest($0, runtime: $1) },
        .devTools: { DevToolsRequest($0, runtime: $1) },
        .deviceChoosers: { DeviceChooserRequest($0, runtime: $1) },
        .screenCapture: { ScreenCaptureRequest($0, runtime: $1) },
        .webAuthn: { WebAuthnRequest($0, runtime: $1) },
        .externalProtocols: { ExternalProtocolRequest($0, runtime: $1) },
        .extensionPopups: { ExtensionPopupRequest($0, runtime: $1) },
        .extensionSidePanels: { ExtensionSidePanelRequest($0, runtime: $1) },
        .extensionInstalls: { ExtensionInstallRequest($0, runtime: $1) },
        .extensionUninstalls: { ExtensionUninstallRequest($0, runtime: $1) },
        .pageHealth: {
            BDKUIPayload($0).string("kind") == "blockedPopups"
                ? BlockedPopupRequest($0, runtime: $1) : UnresponsivePageRequest($0, runtime: $1)
        },
        .webAppInstalls: { WebAppInstallRequest($0, runtime: $1) },
    ]

    /// The typed request for a delivery. A surface without a typed request delivers a
    /// `BDKRawUIRequest`.
    static func make(_ delivery: BDKUIDelivery, runtime: BDKRuntime) -> BDKUIRequest {
        let factory = BDKUISurface(rawValue: delivery.surface).flatMap { factories[$0] }
        return factory?(delivery, runtime) ?? BDKRawUIRequest(delivery, runtime: runtime)
    }
}
