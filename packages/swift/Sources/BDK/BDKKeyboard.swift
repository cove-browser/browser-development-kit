import AppKit
import CBDK

/// The application's side of keyboard handling (swift-api.md section 9).
@MainActor
public protocol BDKKeyboardDelegate: AnyObject {
    /// Before the page sees the event. Return true to keep it from the page, for shortcuts a page
    /// must never capture, such as new page, close page and quit.
    func webContents(_ page: BDKWebContents, shouldReserve event: NSEvent) -> Bool
    /// After the page declined the event. Return true if the application handled it. When there
    /// is no delegate or it returns false, the event goes on to the main menu.
    func webContents(_ page: BDKWebContents, didNotHandle event: NSEvent) -> Bool
}

/// Routes the engine's key callbacks to the page's delegate. The engine retains it until its release
/// callback; it holds the page weakly.
@MainActor
final class KeyRouter {
    weak var contents: BDKWebContents?

    init(contents: BDKWebContents) {
        self.contents = contents
    }

    func shouldReserve(_ native: bdk_native_handle_t) -> Bool {
        guard let contents, let event = Self.event(native),
            let delegate = contents.keyboardDelegate
        else { return false }
        return delegate.webContents(contents, shouldReserve: event)
    }

    func didNotHandle(_ native: bdk_native_handle_t) {
        guard let contents, let event = Self.event(native) else { return }
        if contents.keyboardDelegate?.webContents(contents, didNotHandle: event) == true { return }
        _ = NSApp?.mainMenu?.performKeyEquivalent(with: event)
    }

    /// The key event, when the runtime passed a real one.
    private static func event(_ native: bdk_native_handle_t) -> NSEvent? {
        guard native.kind == BDK_NATIVE_NSEVENT.rawValue, let pointer = native.pointer else {
            return nil
        }
        return Unmanaged<NSEvent>.fromOpaque(pointer).takeUnretainedValue()
    }
}

extension BDKWebContents {
    func installKeyboardHandler() {
        let router = Unmanaged.passRetained(KeyRouter(contents: self))
        bdk_contents_set_keyboard_handler(
            handle,
            { context, native in
                guard let context else { return 0 }
                let router = Unmanaged<KeyRouter>.fromOpaque(context).takeUnretainedValue()
                return MainActor.assumeIsolated { router.shouldReserve(native) ? 1 : 0 }
            },
            { context, native in
                guard let context else { return }
                let router = Unmanaged<KeyRouter>.fromOpaque(context).takeUnretainedValue()
                MainActor.assumeIsolated { router.didNotHandle(native) }
            }, router.toOpaque(),
            { context in
                guard let context else { return }
                Unmanaged<KeyRouter>.fromOpaque(context).release()
            })
    }
}
