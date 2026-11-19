import AppKit
import BDKTestSupport
import SwiftUI
import Testing

@testable import BDK

private let example = URL(string: "https://example.com/")!
private let other = URL(string: "https://example.org/a")!

@MainActor
private func makeWindow() -> NSWindow {
    let window = NSWindow(
        contentRect: NSRect(x: 0, y: 0, width: 400, height: 300), styleMask: [.titled, .closable],
        backing: .buffered, defer: true)
    window.isReleasedWhenClosed = false
    return window
}

@MainActor
private func key(_ characters: String) throws -> NSEvent {
    try #require(
        NSEvent.keyEvent(
            with: .keyDown, location: .zero, modifierFlags: [], timestamp: 1, windowNumber: 0,
            context: nil, characters: characters, charactersIgnoringModifiers: characters,
            isARepeat: false, keyCode: 0))
}

@MainActor
private final class RecordingDelegate: BDKKeyboardDelegate {
    var reserved: [String] = []
    var unhandled: [String] = []
    var reserveKey = "r"
    var handleUnhandled = true

    func webContents(_ page: BDKWebContents, shouldReserve event: NSEvent) -> Bool {
        reserved.append(event.characters ?? "")
        return event.characters == reserveKey
    }

    func webContents(_ page: BDKWebContents, didNotHandle event: NSEvent) -> Bool {
        unhandled.append(event.characters ?? "")
        return handleUnhandled
    }
}

extension BDKStubTests {
    @MainActor
    @Suite("Views")
    struct ViewTests {
        @Test func viewIsStableAndShowsAPlaceholderWithoutANativeView() async throws {
            try await withProfile { runtime, profile in
                try runtime.stub.scriptNavigation(to: example, title: "Example")
                let page = try profile.webContents.create()
                let view = page.view
                #expect(page.view === view)
                #expect(view.contents === page)
                #expect(view.isShowingPlaceholder)
                #expect(view.placeholderText.url == "about:blank")
                try await page.navigate(to: example)
                while view.placeholderText.title != "Example" {
                    try await Task.sleep(for: .milliseconds(5))
                }
                #expect(view.placeholderText.url == example.absoluteString)
                #expect(view.placeholderText.title == "Example")
                try await page.close()
                #expect(view.placeholderText.url == "closed")
            }
        }

        @Test func nativeViewFillsTheBounds() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                let native = NSView()
                let view = BDKWebView(contents: page, nativeView: native)
                view.frame = NSRect(x: 0, y: 0, width: 320, height: 200)
                view.layoutSubtreeIfNeeded()
                #expect(native.superview === view)
                #expect(native.frame == view.bounds)
                #expect(!view.isShowingPlaceholder)
                view.setFrameSize(NSSize(width: 500, height: 100))
                view.layoutSubtreeIfNeeded()
                #expect(native.frame == view.bounds)
            }
        }

        @Test func enteringAWindowRegistersItOnce() async throws {
            try await withProfile { runtime, profile in
                let window = makeWindow()
                let first = try profile.webContents.create()
                let second = try profile.webContents.create()
                #expect(runtime.windows.registeredCount == 0)
                let container = try #require(window.contentView)
                container.addSubview(first.view)
                container.addSubview(second.view)
                #expect(runtime.windows.registeredCount == 1)
                #expect(runtime.windows.isRegistered(window))
                window.close()
                #expect(runtime.windows.registeredCount == 0)
            }
        }

        @Test func movingAPageToAnotherWindowKeepsItsContents() async throws {
            try await withProfile { runtime, profile in
                let firstWindow = makeWindow()
                let secondWindow = makeWindow()
                let left = try profile.webContents.create()
                let right = try profile.webContents.create()
                try await left.navigate(to: example)
                try await right.navigate(to: other)
                try await right.navigate(to: example)
                let leftID = left.id
                let rightState = try right.sessionState()
                let rightEntries = try right.navigation.entries
                let container = try #require(firstWindow.contentView)
                container.addSubview(left.view)
                container.addSubview(right.view)
                #expect(left.view.window === firstWindow && right.view.window === firstWindow)
                #expect(runtime.windows.registeredCount == 1)

                try #require(secondWindow.contentView).addSubview(right.view)  // reparents
                #expect(right.view.window === secondWindow)
                #expect(left.view.window === firstWindow)
                #expect(runtime.windows.registeredCount == 2)
                #expect(
                    runtime.windows.isRegistered(firstWindow)
                        && runtime.windows.isRegistered(secondWindow))

                #expect(left.id == leftID && left.isAlive && right.isAlive)
                #expect(runtime.webContents.count == 2)
                #expect(try right.sessionState() == rightState)
                #expect(try right.navigation.entries == rightEntries)
                #expect(right.url == example)
                #expect(left.url == example)

                // Moving it back registers nothing new.
                container.addSubview(right.view)
                #expect(runtime.windows.registeredCount == 2)
                secondWindow.close()
                #expect(runtime.windows.registeredCount == 1)
                firstWindow.close()
                #expect(runtime.windows.registeredCount == 0)
            }
        }

        @Test func shutdownForgetsWindows() async throws {
            let runtime = try BDKRuntime.startStub()
            let profile = try await runtime.profiles.open("Test")
            let page = try profile.webContents.create()
            let window = makeWindow()
            try #require(window.contentView).addSubview(page.view)
            #expect(runtime.windows.registeredCount == 1)
            await runtime.shutdown()
            #expect(runtime.windows.registeredCount == 0)
            page.view.removeFromSuperview()
            // No runtime is running, so there is nothing to register.
            try #require(window.contentView).addSubview(page.view)
            #expect(runtime.windows.registeredCount == 0)
            window.close()
        }

        @Test func focusReachesTheRuntime() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                let window = makeWindow()
                try #require(window.contentView).addSubview(page.view)
                #expect(page.view.acceptsFirstResponder)
                page.view.focusContent()
                #expect(page.isFocused)
                #expect(window.firstResponder === page.view)
            }
        }

        @Test func keysReachTheDelegate() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                let delegate = RecordingDelegate()
                page.view.keyboardDelegate = delegate
                #expect(page.keyboardDelegate === delegate)

                #expect(try runtime.stub.simulateKey(on: page, event: key("r")) == true)
                // The reserved key never reaches the page, so it is not reported as unhandled.
                #expect(delegate.reserved == ["r"] && delegate.unhandled.isEmpty)

                #expect(try runtime.stub.simulateKey(on: page, event: key("x")) == false)
                #expect(delegate.reserved == ["r", "x"] && delegate.unhandled == ["x"])

                delegate.handleUnhandled = false
                _ = try runtime.stub.simulateKey(on: page, event: key("y"))
                #expect(delegate.unhandled == ["x", "y"])
            }
        }

        @Test func keysWithoutADelegateAreNotReserved() async throws {
            try await withProfile { runtime, profile in
                let page = try profile.webContents.create()
                #expect(try runtime.stub.simulateKey(on: page, event: key("z")) == false)
                page.destroy()
                #expect(throws: BDKError.self) {
                    try runtime.stub.simulateKey(on: page, event: key("z"))
                }
            }
        }

        @Test func swiftUIHostsTheSameView() async throws {
            try await withProfile { _, profile in
                let page = try profile.webContents.create()
                let host = NSHostingView(rootView: BDKWebContentsView(page))
                let window = makeWindow()
                window.contentView = host
                host.frame = NSRect(x: 0, y: 0, width: 300, height: 200)
                host.layoutSubtreeIfNeeded()
                #expect(page.view.window === window)
                #expect(page.view.isDescendant(of: host))
            }
        }

        @Test func swiftUIShowsTheNewPageWhenThePageChanges() async throws {
            try await withProfile { _, profile in
                let first = try profile.webContents.create()
                let second = try profile.webContents.create()
                let host = NSHostingView(rootView: BDKWebContentsView(first))
                let window = makeWindow()
                window.contentView = host
                host.frame = NSRect(x: 0, y: 0, width: 300, height: 200)
                host.layoutSubtreeIfNeeded()
                #expect(first.view.isDescendant(of: host))
                host.rootView = BDKWebContentsView(second)
                host.layoutSubtreeIfNeeded()
                #expect(second.view.isDescendant(of: host))
                #expect(!first.view.isDescendant(of: host))
            }
        }
    }
}
