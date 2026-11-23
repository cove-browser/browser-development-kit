import AppKit
import CoreImage

// Passkeys and security keys (ui-requests.md 5.18, T-UIR-09).
//
// A WebAuthn ceremony (`navigator.credentials.create` or `.get`) is a sequence of steps, and each
// step the engine shows is one request: choosing a mechanism, touching a security key, entering
// its PIN, choosing an account, connecting a phone, a confirmation or an error. When Chromium
// moves to the next step it withdraws the request (`cancellation` fires) and delivers the next
// one, with the same `ceremonyID`, so a presenter can keep one sheet up for the whole ceremony.
// The ceremony ends when the last step is withdrawn and no other follows.
//
// Completions: "accept" with `mechanism`, `pin` or `action`, or with nothing for the step's own
// button; "grant" with `account` (it releases a credential to the site, so it needs the
// confirming event); "deny" cancels the ceremony, and the page's promise rejects with
// NotAllowedError. `.reject`, a dropped request and a disabled surface cancel it too.

public final class WebAuthnRequest: BDKUIRequest {
    public enum Operation: String, Sendable {
        /// `navigator.credentials.create`: a new passkey or security-key credential.
        case create
        /// `navigator.credentials.get`: signing in with one.
        case get
    }

    public enum Step: String, Sendable {
        /// Choose how to authenticate: `mechanisms`, then `choose(_:)`.
        case chooseMechanism
        /// Insert and touch a security key. Nothing to answer but `cancel()`; the step ends when
        /// the key responds.
        case touchSecurityKey
        /// The security key's PIN: `pin`, then `providePIN(_:)`.
        case enterPIN
        /// Which passkey to use: `accounts`, then `chooseAccount(_:confirmedBy:)`.
        case chooseAccount
        /// Scan a QR code with a phone (`qrCode`), or wait for Bluetooth or the phone.
        case connectPhone
        /// A question with the step's own button (`acceptLabel`), answered by `accept()`.
        case confirm
        /// The ceremony failed: `error`. `accept()` tries again where `acceptLabel` offers it.
        case error
        /// A step this version does not model. `text` and the labels say what it is.
        case other
    }

    public enum ErrorKind: String, Sendable {
        case noMechanisms
        case noPasskeys
        case timedOut
        case keyNotRegistered
        case keyAlreadyRegistered
        case missingCapability
        case storageFull
        /// The phone could not be reached or failed.
        case phone
        case pinSoftBlocked
        case pinBlocked
        case keyRemoved
        case unrecognized
        case other
    }

    public struct Mechanism: Sendable, Hashable, Identifiable {
        public enum Kind: String, Sendable {
            /// A USB, NFC or Bluetooth security key.
            case securityKey
            /// A passkey on a phone, over a QR code and Bluetooth (hybrid).
            case phone
            /// The Mac's own authenticator (Touch ID).
            case platform
            /// The system's passkey provider (iCloud Keychain or another credential provider).
            case iCloudKeychain
            /// One passkey the page can use, named by `name`.
            case passkey
            /// A saved password offered alongside passkeys.
            case password
            case other
        }

        public let id: String
        public let kind: Kind
        public let name: String
        public let description: String?

        init(_ payload: BDKUIPayload) {
            id = payload.string("id") ?? ""
            kind = payload.choice("kind", default: .other)
            name = payload.string("name") ?? ""
            description = payload.string("description")
        }
    }

    public struct Account: Sendable, Hashable, Identifiable {
        public let id: String
        /// The account name the site stored, often an email address.
        public let name: String
        public let displayName: String

        init(_ payload: BDKUIPayload) {
            id = payload.string("id") ?? ""
            name = payload.string("name") ?? ""
            displayName = payload.string("displayName") ?? ""
        }
    }

    public struct PINEntry: Sendable, Equatable {
        public enum Mode: String, Sendable {
            /// The key has a PIN: ask for it.
            case enter
            /// The key needs a new PIN: ask for it twice and give it once.
            case setup
            /// The key needs its PIN changed: as `setup`.
            case change
        }

        public let mode: Mode
        public let minimumLength: Int
        /// Tries left before the key locks, when the key has said.
        public let attemptsRemaining: Int?

        init(_ payload: BDKUIPayload) {
            mode = payload.choice("mode", default: .enter)
            minimumLength = payload.int("minLength") ?? 4
            attemptsRemaining = payload.int("attemptsRemaining")
        }
    }

    /// The same for every step of one ceremony.
    public let ceremonyID: String
    /// The relying party ID, the domain the credential belongs to.
    public let relyingParty: String
    /// The top-level page's origin.
    public let origin: URL?
    public let operation: Operation
    public let step: Step
    /// Chromium's name for the step ("usbInsertAndActivate", "clientPinTapAgain", ...), to tell
    /// apart two steps of one kind.
    public let detail: String
    /// Set when `step` is `.error`.
    public let error: ErrorKind?
    /// Chromium's wording for the step. A custom presenter shows the same facts.
    public let text: BDKUIText
    /// Why the last attempt failed (a wrong PIN, two PINs that differ), in Chromium's wording.
    public let errorMessage: String?
    public let hint: String?
    /// The label of the step's own button, when it has one; `accept()` presses it.
    public let acceptLabel: String?
    public let cancelLabel: String?
    /// The label of "use another method", when the step offers it; `useOtherMechanism()`.
    public let otherMechanismsLabel: String?
    /// The step is waiting on the authenticator: show progress, keep only cancel enabled.
    public let isBusy: Bool
    public let mechanisms: [Mechanism]
    public let accounts: [Account]
    /// Set when `step` is `.enterPIN`.
    public let pin: PINEntry?
    /// The FIDO URL a phone scans, when `step` is `.connectPhone` and the code is shown.
    public let qrCode: String?

    override init(_ delivery: BDKUIDelivery, runtime: BDKRuntime) {
        let fields = BDKUIPayload(delivery)
        ceremonyID = fields.string("ceremony") ?? ""
        relyingParty = fields.string("relyingParty") ?? ""
        origin = fields.url("origin")
        operation = fields.choice("operation", default: .get)
        step = fields.choice("step", default: .other)
        detail = fields.string("detail") ?? ""
        error = fields.string("error").map { ErrorKind(rawValue: $0) ?? .other }
        let text = fields.object("text")
        self.text = BDKUIText(text)
        errorMessage = text?.string("error")
        hint = text?.string("hint")
        acceptLabel = fields.string("acceptLabel")
        cancelLabel = fields.string("cancelLabel")
        otherMechanismsLabel = fields.string("otherMechanismsLabel")
        isBusy = fields.bool("busy")
        mechanisms = fields.objects("mechanisms").map(Mechanism.init)
        accounts = fields.objects("accounts").map(Account.init)
        pin = fields.object("pin").map(PINEntry.init)
        qrCode = fields.string("qrCode")
        super.init(delivery, runtime: runtime)
    }

    // MARK: Completions

    /// Throws `ui.mechanismNotOffered` for a mechanism the step did not list.
    public func choose(_ mechanism: Mechanism) throws {
        guard mechanisms.contains(where: { $0.id == mechanism.id }) else {
            throw BDKError(
                domain: .ui, code: "mechanismNotOffered",
                message: "the mechanism is not one the step offered")
        }
        try submit("accept", ["mechanism": .string(mechanism.id)])
    }

    /// Gives the security key's PIN. For a new PIN (`.setup`, `.change`) the application has
    /// confirmed it already. Throws `ui.pinNotRequested` when the step asks for none and
    /// `ui.pinTooShort` for a PIN shorter than `pin.minimumLength` characters, leaving the step
    /// open; the key itself judges the rest, and a wrong PIN comes back as the next step's
    /// `errorMessage`.
    public func providePIN(_ value: String) throws {
        guard let pin else {
            throw BDKError(
                domain: .ui, code: "pinNotRequested", message: "the step asks for no PIN")
        }
        guard value.count >= pin.minimumLength else {
            throw BDKError(
                domain: .ui, code: "pinTooShort",
                message: "the PIN needs at least \(pin.minimumLength) characters")
        }
        try submit("accept", ["pin": .string(value)])
    }

    /// Signs in with the account, or creates the passkey for it. `event` is the click or key that
    /// chose it: without one it throws `ui.gestureRequired`. Throws `ui.accountNotOffered` for an
    /// account the step did not list.
    public func chooseAccount(_ account: Account, confirmedBy event: NSEvent) throws {
        guard accounts.contains(where: { $0.id == account.id }) else {
            throw BDKError(
                domain: .ui, code: "accountNotOffered",
                message: "the account is not one the step offered")
        }
        try submit("grant", ["account": .string(account.id)], gesture: event)
    }

    /// Presses the step's own button (`acceptLabel`): confirm, try again. Throws
    /// `ui.nothingToAccept` when the step has none.
    public func accept() throws {
        guard acceptLabel != nil else {
            throw BDKError(
                domain: .ui, code: "nothingToAccept", message: "the step has no button to press")
        }
        try submit("accept")
    }

    /// Goes back to choosing a mechanism. Throws `ui.nothingToAccept` when the step does not
    /// offer it (`otherMechanismsLabel` is nil).
    public func useOtherMechanism() throws {
        guard otherMechanismsLabel != nil else {
            throw BDKError(
                domain: .ui, code: "nothingToAccept",
                message: "the step offers no other mechanism")
        }
        try submit("accept", ["action": .string("otherMechanisms")])
    }

    /// Ends the ceremony; the page's promise rejects with NotAllowedError.
    public func cancel() {
        try? submit("deny")
    }

    /// A handler that cancels every ceremony, for an application that offers no passkeys but
    /// leaves the surface enabled (`disabled` in `bdk.toml` does the same without a handler).
    public static func cancelAll(_ request: WebAuthnRequest) -> BDKUIResponse {
        .reject
    }
}

// MARK: What to show

extension WebAuthnRequest {
    /// What a presenter shows for the step, whatever it draws with: the text, the choices, a PIN
    /// field and the buttons, in Chromium's order. `perform(_:pin:confirmedBy:)` answers with one.
    public struct Presentation: Sendable, Equatable {
        public enum Choice: Sendable, Hashable {
            case mechanism(Mechanism)
            case account(Account)

            public var title: String {
                switch self {
                case .mechanism(let mechanism): mechanism.name
                case .account(let account):
                    account.name.isEmpty ? account.displayName : account.name
                }
            }
        }

        public enum Action: Sendable, Hashable {
            /// The step's own button, with its label.
            case accept(String)
            case otherMechanisms(String)
            case cancel(String)
        }

        public let title: String
        public let message: String
        public let errorMessage: String?
        public let hint: String?
        public let showsProgress: Bool
        public let choices: [Choice]
        /// Set when the presenter shows a PIN field (two for a new PIN).
        public let pin: PINEntry?
        /// The QR code to show a phone, already drawn.
        public let qrCode: String?
        public let actions: [Action]
    }

    public var presentation: Presentation {
        var actions: [Presentation.Action] = []
        if let acceptLabel { actions.append(.accept(acceptLabel)) }
        if let otherMechanismsLabel { actions.append(.otherMechanisms(otherMechanismsLabel)) }
        actions.append(.cancel(cancelLabel ?? "Cancel"))
        let choices: [Presentation.Choice] =
            step == .chooseAccount
            ? accounts.map(Presentation.Choice.account)
            : mechanisms.map(Presentation.Choice.mechanism)
        return Presentation(
            title: text.title, message: text.message, errorMessage: errorMessage, hint: hint,
            showsProgress: isBusy, choices: choices, pin: pin, qrCode: qrCode, actions: actions)
    }

    /// Answers with a choice of the presentation. A PIN step's accept action needs `pin`; an
    /// account needs `event`, the click that chose it.
    public func perform(_ choice: Presentation.Choice, confirmedBy event: NSEvent? = nil) throws {
        switch choice {
        case .mechanism(let mechanism):
            try choose(mechanism)
        case .account(let account):
            guard let event else {
                throw BDKError(
                    domain: .ui, code: "gestureRequired",
                    message: "choosing an account needs the event that chose it")
            }
            try chooseAccount(account, confirmedBy: event)
        }
    }

    /// Answers with an action of the presentation.
    public func perform(_ action: Presentation.Action, pin value: String? = nil) throws {
        switch action {
        case .accept:
            if pin != nil {
                try providePIN(value ?? "")
            } else {
                try accept()
            }
        case .otherMechanisms:
            try useOtherMechanism()
        case .cancel:
            cancel()
        }
    }

    /// The QR code a phone scans, drawn `scale` device pixels per module, or nil when the step
    /// shows none.
    public func qrCodeImage(scale: CGFloat = 8) -> NSImage? {
        guard let qrCode, let data = qrCode.data(using: .utf8),
            let filter = CIFilter(name: "CIQRCodeGenerator")
        else { return nil }
        filter.setValue(data, forKey: "inputMessage")
        filter.setValue("M", forKey: "inputCorrectionLevel")
        guard let output = filter.outputImage?.transformed(by: .init(scaleX: scale, y: scale))
        else { return nil }
        let representation = NSCIImageRep(ciImage: output)
        let image = NSImage(size: representation.size)
        image.addRepresentation(representation)
        return image
    }
}
