import AppKit
import CBDK

/// The windows registered with the runtime (swift-api.md section 8.1). A window is registered once:
/// by the application with `register(_:_:)`, or as a normal window the first time a page view enters
/// it. It is unregistered when it closes.
@MainActor
public final class BDKWindows {
    private struct Entry {
        let window: BDKWindow
        let observer: any NSObjectProtocol
    }

    private unowned let runtime: BDKRuntime
    private var entries: [ObjectIdentifier: Entry] = [:]

    init(runtime: BDKRuntime) {
        self.runtime = runtime
    }

    /// Every registered window.
    public var registered: [BDKWindow] {
        entries.values.map(\.window).sorted { $0.handle < $1.handle }
    }

    var registeredCount: Int {
        entries.count
    }

    func isRegistered(_ window: NSWindow) -> Bool {
        self.window(for: window) != nil
    }

    /// The registered window for an application window, if there is one.
    public func window(for nsWindow: NSWindow) -> BDKWindow? {
        guard let entry = entries[ObjectIdentifier(nsWindow)], entry.window.nsWindow === nsWindow
        else { return nil }
        return entry.window
    }

    /// The application window registered under this handle.
    func window(handle: bdk_window_t) -> NSWindow? {
        bdkWindow(handle: handle)?.nsWindow
    }

    func bdkWindow(handle: bdk_window_t) -> BDKWindow? {
        guard handle != 0 else { return nil }
        return entries.values.first { $0.window.handle == handle }?.window
    }

    /// Registers the window with its kind, or returns the one already registered (whose kind does
    /// not change: a window is registered once).
    @discardableResult
    public func register(_ nsWindow: NSWindow, _ options: BDKWindow.Options = .init()) throws
        -> BDKWindow
    {
        if let existing = window(for: nsWindow) { return existing }
        let key = ObjectIdentifier(nsWindow)
        let runtimeHandle = try runtime.liveHandle()
        let native = bdk_native_handle_t(
            kind: BDK_NATIVE_NSWINDOW.rawValue,
            pointer: Unmanaged.passUnretained(nsWindow).toOpaque(),
            id: UInt64(max(nsWindow.windowNumber, 0)))
        var handle: bdk_window_t = 0
        try abiCall(.runtime) {
            bdk_window_register(runtimeHandle, native, options.kind.rawValue, &handle, $0)
        }
        let window = BDKWindow(nsWindow, kind: options.kind, handle: handle, runtime: runtime)
        do {
            try window.installCallbacks()
        } catch {
            try? abiCall(.runtime) { bdk_window_unregister(handle, $0) }
            throw error
        }
        let observer = NotificationCenter.default.addObserver(
            forName: NSWindow.willCloseNotification, object: nsWindow, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.unregister(key) }
        }
        entries[key] = Entry(window: window, observer: observer)
        return window
    }

    private func unregister(_ key: ObjectIdentifier) {
        guard let entry = entries.removeValue(forKey: key) else { return }
        NotificationCenter.default.removeObserver(entry.observer)
        entry.window.unregistered()
    }

    /// Forgets every window after the runtime released their handles.
    func removeAll() {
        for entry in entries.values {
            NotificationCenter.default.removeObserver(entry.observer)
            entry.window.invalidate()
        }
        entries.removeAll()
    }
}
