import AppKit
import BDKTestSupport
import CBDK
import Foundation
import Testing

@testable import BDK

@MainActor
private final class RecordingDelegate: BDKWindowDelegate {
    var requests: [WindowRequest] = []
    var response = BDKUIResponse.handled

    func window(_ window: BDKWindow, didReceive request: WindowRequest) -> BDKUIResponse {
        requests.append(request)
        return response
    }
}

@MainActor
private func makeWindow() -> NSWindow {
    let window = NSWindow(
        contentRect: NSRect(x: 0, y: 0, width: 400, height: 300), styleMask: [.titled],
        backing: .buffered, defer: true)
    window.isReleasedWhenClosed = false
    return window
}

extension BDKStubTests {
    @MainActor
    @Suite("Window registration")
    struct WindowRegistrationTests {
        @Test func registeringReturnsTheSameWindowAndKeepsItsKind() async throws {
            try await withProfile { runtime, _ in
                let native = makeWindow()
                let window = try runtime.windows.register(native, .init(kind: .popup))
                #expect(window.kind == .popup && window.nsWindow === native)
                #expect(try runtime.windows.register(native) === window)
                #expect(runtime.windows.registered.count == 1)
                #expect(runtime.windows.window(for: native) === window)
                native.close()
                #expect(runtime.windows.registered.isEmpty)
                #expect(window.pages.isEmpty)
            }
        }

        @Test func aPageViewRegistersAnUnregisteredWindowAsNormal() async throws {
            try await withProfile { runtime, profile in
                let native = makeWindow()
                let page = try profile.webContents.create()
                native.contentView?.addSubview(page.view)
                let window = try #require(runtime.windows.window(for: native))
                #expect(window.kind == .normal)
                native.close()
            }
        }

        @Test func aRegisteredWindowKeepsItsKindWhenAViewEntersIt() async throws {
            try await withProfile { runtime, profile in
                let native = makeWindow()
                let window = try runtime.windows.register(native, .init(kind: .popup))
                let page = try profile.webContents.create()
                native.contentView?.addSubview(page.view)
                #expect(runtime.windows.window(for: native) === window && window.kind == .popup)
                native.close()
            }
        }

        @Test func pagesActivePageAndExtensionID() async throws {
            try await withProfile { runtime, profile in
                let native = makeWindow()
                let window = try runtime.windows.register(native)
                let first = try profile.webContents.create()
                let second = try profile.webContents.create()
                #expect(window.pages.isEmpty && window.activePage == nil)
                #expect(window.extensionWindowID == nil && first.window == nil)

                try runtime.stub.attach(first, to: window)
                try runtime.stub.attach(second, to: window)
                #expect(
                    window.pages.count == 2 && window.pages[0] === first
                        && window.pages[1] === second)
                #expect(window.activePage === first)
                #expect(window.extensionWindowID == 1)
                #expect(first.window === window && second.window === window)

                try second.focus()
                #expect(window.activePage === second)
                native.close()
            }
        }

        @Test func anAnchorIsConvertedToTopLeftCoordinates() async throws {
            try await withProfile { runtime, _ in
                let native = makeWindow()
                let window = try runtime.windows.register(native)
                #expect(try runtime.stub.anchor(of: window, role: .extensions) == nil)

                let height = native.frame.height
                window.anchors[.extensions] = { CGRect(x: 10, y: 20, width: 30, height: 40) }
                let rect = try #require(try runtime.stub.anchor(of: window, role: .extensions))
                #expect(rect == CGRect(x: 10, y: height - 60, width: 30, height: 40))
                #expect(try runtime.stub.anchor(of: window, role: .addressBar) == nil)

                window.anchors[.extensions] = { .zero }
                #expect(try runtime.stub.anchor(of: window, role: .extensions) == nil)
                window.anchors[.extensions] = nil
                native.close()
            }
        }

        @Test func theDelegateAnswersAWindowRequest() async throws {
            try await withProfile { runtime, _ in
                let native = makeWindow()
                let window = try runtime.windows.register(native)
                let delegate = RecordingDelegate()
                window.delegate = delegate

                try runtime.stub.requestWindowChange(
                    window,
                    json: """
                        {"kind":"setFrame","frame":{"x":1,"y":2,"width":300,"height":200},\
                        "initiator":{"extensionId":"abc"}}
                        """)
                runtime.stub.pump()
                #expect(delegate.requests.count == 1)
                #expect(
                    delegate.requests.first?.action
                        == .setFrame(CGRect(x: 1, y: 2, width: 300, height: 200)))
                #expect(delegate.requests.first?.extensionID == "abc")
                #expect(try runtime.stub.lastWindowAnswer == BDK_UI_HANDLED.rawValue)

                delegate.response = .reject
                try runtime.stub.requestWindowChange(window, json: "{\"kind\":\"minimize\"}")
                runtime.stub.pump()
                #expect(delegate.requests.last?.action == .minimize)
                #expect(delegate.requests.last?.extensionID == nil)
                #expect(try runtime.stub.lastWindowAnswer == BDK_UI_REJECT.rawValue)
                native.close()
            }
        }

        @Test func withoutADelegateTheRequestTakesTheDefault() async throws {
            try await withProfile { runtime, _ in
                let native = makeWindow()
                let window = try runtime.windows.register(native)
                try runtime.stub.requestWindowChange(window, json: "{\"kind\":\"zoom\"}")
                runtime.stub.pump()
                #expect(try runtime.stub.lastWindowAnswer == BDK_UI_USE_DEFAULT.rawValue)

                let delegate = RecordingDelegate()
                delegate.response = .useDefault
                window.delegate = delegate
                try runtime.stub.requestWindowChange(window, json: "{\"kind\":\"close\"}")
                runtime.stub.pump()
                #expect(try runtime.stub.lastWindowAnswer == BDK_UI_USE_DEFAULT.rawValue)
                native.close()
            }
        }

        @Test func everyWindowRequestKindDecodes() {
            let kinds = [
                "activate": WindowRequest.Action.activate, "minimize": .minimize, "zoom": .zoom,
                "enterFullscreen": .enterFullscreen, "exitFullscreen": .exitFullscreen,
                "close": .close,
            ]
            for (kind, action) in kinds {
                #expect(WindowRequest(json: "{\"kind\":\"\(kind)\"}")?.action == action)
            }
            #expect(WindowRequest(json: "{\"kind\":\"setFrame\"}") == nil)
            #expect(WindowRequest(json: "{\"kind\":\"nope\"}") == nil)
            #expect(WindowRequest(json: "[]") == nil)
        }
    }
}
