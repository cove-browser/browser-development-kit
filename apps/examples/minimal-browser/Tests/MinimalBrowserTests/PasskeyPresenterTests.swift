import AppKit
import BDK
import BDKTestSupport
import Foundation
import MinimalBrowserKit
import Testing

/// Records what the presenter asked to draw.
@MainActor
private final class RecordingDisplay: PasskeyDisplay {
    var shown: [String] = []
    var closed: [String] = []

    func show(_ request: WebAuthnRequest, presenter: PasskeyPresenter) {
        shown.append(request.presentation.title)
    }

    func close(ceremony: String) {
        closed.append(ceremony)
    }
}

extension SessionTests {
    /// The example's own passkey UI (T-UIR-09), against steps raised as the engine raises them.
    @Suite("Passkeys")
    @MainActor
    struct PasskeyPresenterTests {
        static let mechanisms = """
            {"ceremony":"3","relyingParty":"example.com","origin":"https://example.com",\
            "operation":"create","step":"chooseMechanism","detail":"mechanismSelection",\
            "text":{"title":"Choose where to save your passkey","message":"for example.com"},\
            "cancelLabel":"Cancel","mechanisms":[{"id":"0","kind":"securityKey","name":"USB security key"}]}
            """
        static let pin = """
            {"ceremony":"3","relyingParty":"example.com","operation":"create","step":"enterPIN",\
            "detail":"clientPinEntry","text":{"title":"Enter your PIN"},"acceptLabel":"Next",\
            "cancelLabel":"Cancel","pin":{"mode":"enter","minLength":4}}
            """

        private func withPresenter(
            _ body: (BDKRuntime, BDKWebContents, PasskeyPresenter, RecordingDisplay) async throws ->
                Void
        ) async throws {
            let configuration = BDKConfiguration.stub
            let display = RecordingDisplay()
            let presenter = PasskeyPresenter(display: display)
            try presenter.install(in: configuration)
            let runtime = try BDKRuntime.startStub(configuration)
            do {
                let profile = try await runtime.profiles.open("Passkeys")
                let page = try profile.webContents.create()
                try await body(runtime, page, presenter, display)
            } catch {
                await runtime.shutdown()
                throw error
            }
            await runtime.shutdown()
        }

        private func raise(_ runtime: BDKRuntime, _ page: BDKWebContents, _ payload: String) throws
        {
            try runtime.stub.raiseUIRequest(
                surface: BDKUISurface.webAuthn.rawValue, on: page, payloadJSON: payload)
            runtime.stub.pump()
        }

        @Test("each step of a ceremony is drawn in the same sheet, and Cancel ends it")
        func stepsShareOneSheet() async throws {
            try await withPresenter { runtime, page, presenter, display in
                try raise(runtime, page, Self.mechanisms)
                // The engine withdraws a step and raises the next one together.
                try runtime.stub.withdrawUIRequests()
                try raise(runtime, page, Self.pin)
                for _ in 0..<5 { await Task.yield() }
                #expect(display.shown == ["Choose where to save your passkey", "Enter your PIN"])
                #expect(display.closed.isEmpty)
                #expect(presenter.steps.keys.sorted() == ["3"])
                #expect(presenter.steps["3"]?.step == .enterPIN)

                try presenter.perform(.cancel("Cancel"), in: "3")
                runtime.stub.pump()
                #expect(display.closed == ["3"])
                #expect(presenter.steps.isEmpty)
                let outcome = try runtime.stub.lastUIOutcome
                #expect(outcome.kind == .completed && outcome.completion == "deny")
            }
        }

        @Test("a PIN goes back through the step, and the sheet closes if no step follows")
        func answeredStepCloses() async throws {
            try await withPresenter { runtime, page, presenter, display in
                try raise(runtime, page, Self.pin)
                try presenter.perform(.accept("Next"), in: "3", pin: "1234")
                runtime.stub.pump()
                let outcome = try runtime.stub.lastUIOutcome
                #expect(
                    outcome.completion == "accept" && outcome.argumentsJSON == "{\"pin\":\"1234\"}")
                #expect(display.closed.isEmpty)
                try await Task.sleep(for: PasskeyPresenter.nextStepWait + .milliseconds(300))
                #expect(display.closed == ["3"])
            }
        }

        @Test("a withdrawn step with no next one closes its sheet")
        func withdrawnStepCloses() async throws {
            try await withPresenter { runtime, page, presenter, display in
                try raise(runtime, page, Self.mechanisms)
                try runtime.stub.withdrawUIRequests()
                runtime.stub.pump()
                for _ in 0..<5 { await Task.yield() }
                #expect(display.closed == ["3"])
                #expect(presenter.steps.isEmpty)
            }
        }
    }
}
