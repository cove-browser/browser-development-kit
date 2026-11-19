import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

/// Collects what the handlers of a `UIFixture` receive and what they answer.
@MainActor
final class UIBox {
    var request: BDKUIRequest?
    var response = BDKUIResponse.handled
    var count = 0
}

/// A running stub runtime with a page and a raw handler on every surface, which stores the request
/// it received in `box` and answers `box.response`.
@MainActor
struct UIFixture {
    let runtime: BDKRuntime
    let profile: BDKProfile
    let page: BDKWebContents
    let box: UIBox

    /// Raises a request, lets the runtime deliver it, and returns it as `type`.
    func deliver<Request: BDKUIRequest>(
        _ type: Request.Type, _ surface: UInt32, _ payload: String = "{}"
    ) throws -> Request {
        box.request = nil
        try raise(surface, payload)
        return try #require(box.request as? Request)
    }

    func raise(_ surface: UInt32, _ payload: String = "{}") throws {
        try runtime.stub.raiseUIRequest(surface: surface, on: page, payloadJSON: payload)
        runtime.stub.pump()
    }

    func outcome() throws -> BDKStub.UIOutcome {
        runtime.stub.pump()
        return try runtime.stub.lastUIOutcome
    }

    /// The outcome as a comparable triple.
    func expectOutcome(
        _ kind: BDKStub.UIOutcomeKind, _ completion: String = "", _ arguments: String = "",
        sourceLocation: SourceLocation = #_sourceLocation
    ) throws {
        let outcome = try outcome()
        #expect(outcome.kind == kind, sourceLocation: sourceLocation)
        #expect(outcome.completion == completion, sourceLocation: sourceLocation)
        #expect(outcome.argumentsJSON == arguments, sourceLocation: sourceLocation)
    }
}

@MainActor
func withUIFixture(_ body: (UIFixture) async throws -> Void) async throws {
    try await withStubRuntime(freezingRegistrations: false) { runtime in
        let box = UIBox()
        for surface in BDKUISurface.allCases {
            try runtime.ui.setHandler(for: surface) { request in
                box.request = request
                box.count += 1
                return box.response
            }
        }
        try runtime.freezeRegistrations()
        let profile = try await runtime.profiles.open("UI")
        let page = try profile.webContents.create()
        try await body(UIFixture(runtime: runtime, profile: profile, page: page, box: box))
    }
}

@MainActor
func confirmingEvent() throws -> NSEvent {
    try #require(
        NSEvent.mouseEvent(
            with: .leftMouseUp, location: .zero, modifierFlags: [], timestamp: 1, windowNumber: 0,
            context: nil, eventNumber: 0, clickCount: 1, pressure: 0))
}
