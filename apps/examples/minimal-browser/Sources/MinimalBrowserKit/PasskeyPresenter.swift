import AppKit
import BDK

/// What draws a passkey ceremony: one sheet per ceremony, redrawn for each step.
@MainActor
public protocol PasskeyDisplay: AnyObject {
    /// Shows the ceremony's sheet for `request`'s step, or redraws it when it is open.
    func show(_ request: WebAuthnRequest, presenter: PasskeyPresenter)
    /// Takes the ceremony's sheet down.
    func close(ceremony: String)
}

/// The example's own passkey and security-key UI (ui-requests.md 5.18, T-UIR-09), in place of BDK's default
/// sheet. Each step of a ceremony arrives as its own `WebAuthnRequest` with the same `ceremonyID`: the
/// presenter keeps one sheet per ceremony and redraws it for the next step, and answers through the step's
/// `presentation`, so it shows what the default would. The sheet goes when the ceremony ends: the step was
/// withdrawn, or it was answered and no next step followed.
@MainActor
public final class PasskeyPresenter {
    /// How long an answered step's sheet waits for the next step before it closes.
    public static let nextStepWait: Duration = .milliseconds(500)

    private let display: PasskeyDisplay
    /// The step each open ceremony shows.
    public private(set) var steps: [String: WebAuthnRequest] = [:]
    /// Bumped by every step and answer, so a close scheduled for an earlier one does nothing.
    private var generations: [String: Int] = [:]

    public init(display: PasskeyDisplay) {
        self.display = display
    }

    /// Installs the presenter as the handler of the `webauthn` surface. Called before the runtime starts.
    public func install(in configuration: BDKConfiguration) throws {
        try configuration.ui.webAuthn.setHandler { [weak self] request in
            self?.handle(request) ?? .useDefault
        }
    }

    /// Takes a step: it replaces the ceremony's previous one in the same sheet.
    public func handle(_ request: WebAuthnRequest) -> BDKUIResponse {
        let ceremony = request.ceremonyID
        steps[ceremony] = request
        let generation = bump(ceremony)
        display.show(request, presenter: self)
        request.cancellation.onFire { [weak self] in
            // Withdrawn: the next step replaces it at once, or the ceremony is over.
            self?.closeLater(ceremony, generation: generation, after: .zero)
        }
        return .handled
    }

    /// The sheet's choices: a mechanism, or an account with the click that chose it.
    public func perform(
        _ choice: WebAuthnRequest.Presentation.Choice, in ceremony: String,
        confirmedBy event: NSEvent?
    ) throws {
        guard let request = steps[ceremony] else { return }
        try request.perform(choice, confirmedBy: event)
        answered(ceremony)
    }

    /// The sheet's buttons; the accept button of a PIN step gives the PIN typed.
    public func perform(
        _ action: WebAuthnRequest.Presentation.Action, in ceremony: String, pin: String? = nil
    ) throws {
        guard let request = steps[ceremony] else { return }
        try request.perform(action, pin: pin)
        if case .cancel = action {
            close(ceremony)
        } else {
            answered(ceremony)
        }
    }

    private func answered(_ ceremony: String) {
        closeLater(ceremony, generation: bump(ceremony), after: Self.nextStepWait)
    }

    private func bump(_ ceremony: String) -> Int {
        let next = (generations[ceremony] ?? 0) + 1
        generations[ceremony] = next
        return next
    }

    private func closeLater(_ ceremony: String, generation: Int, after delay: Duration) {
        Task { @MainActor [weak self] in
            if delay > .zero { try? await Task.sleep(for: delay) } else { await Task.yield() }
            guard let self, self.generations[ceremony] == generation else { return }
            self.close(ceremony)
        }
    }

    private func close(_ ceremony: String) {
        guard steps.removeValue(forKey: ceremony) != nil else { return }
        generations[ceremony] = nil
        display.close(ceremony: ceremony)
    }
}
