import AppKit
import CBDK

/// The windows registered with the runtime. A window is registered once, the first time a page
/// view enters it, and unregistered when it closes.
@MainActor
final class BDKWindows {
    private struct Entry {
        weak var window: NSWindow?
        let handle: bdk_window_t
        let observer: any NSObjectProtocol
    }

    private var entries: [ObjectIdentifier: Entry] = [:]

    var registeredCount: Int {
        entries.count
    }

    func isRegistered(_ window: NSWindow) -> Bool {
        entries[ObjectIdentifier(window)]?.window === window
    }

    /// The application window registered under this handle.
    func window(handle: bdk_window_t) -> NSWindow? {
        guard handle != 0 else { return nil }
        return entries.values.first { $0.handle == handle }?.window
    }

    /// Registers the window unless it already is.
    func register(_ window: NSWindow, with runtime: BDKRuntime) throws {
        let key = ObjectIdentifier(window)
        if let entry = entries[key], entry.window === window { return }
        let runtimeHandle = try runtime.liveHandle()
        let native = bdk_native_handle_t(
            kind: BDK_NATIVE_NSWINDOW.rawValue,
            pointer: Unmanaged.passUnretained(window).toOpaque(),
            id: UInt64(max(window.windowNumber, 0)))
        var handle: bdk_window_t = 0
        try abiCall(.runtime) { bdk_window_register(runtimeHandle, native, 0, &handle, $0) }
        let observer = NotificationCenter.default.addObserver(
            forName: NSWindow.willCloseNotification, object: window, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.unregister(key) }
        }
        entries[key] = Entry(window: window, handle: handle, observer: observer)
    }

    private func unregister(_ key: ObjectIdentifier) {
        guard let entry = entries.removeValue(forKey: key) else { return }
        NotificationCenter.default.removeObserver(entry.observer)
        // A stale window handle means the runtime shut down first and already released it.
        try? abiCall(.runtime) { bdk_window_unregister(entry.handle, $0) }
    }

    /// Forgets every window after the runtime released their handles.
    func removeAll() {
        for entry in entries.values { NotificationCenter.default.removeObserver(entry.observer) }
        entries.removeAll()
    }
}
