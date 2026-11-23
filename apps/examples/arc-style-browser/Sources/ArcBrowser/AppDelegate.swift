import AppKit
import ArcBrowserCore
import ArcBrowserKit
import BDK
import os

/// Starts the runtime from the bundled configuration, restores the last session (or opens the
/// initial layout), runs the launch actions, and saves the session when the application quits.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let options: AppOptions
    private let log = Logger(subsystem: "dev.bdk.examples.arc-style-browser", category: "app")
    private var runtime: BDKRuntime?
    private var controller: BrowserController?
    private let extensions = ExtensionPlacement()
    private let pages = PagePlacement()
    private var phase = AppStatus.Phase.launching
    private var actionsDone = false
    private var errors: [String] = []
    private var didReportTermination = false

    init(options: AppOptions) {
        self.options = options
    }

    private var supportDirectory: URL? {
        options.supportDirectory
            ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)
            .first?.appendingPathComponent(
                Bundle.main.bundleIdentifier ?? "arc-style-browser", isDirectory: true)
    }

    // MARK: Launch

    func applicationWillFinishLaunching(_ notification: Notification) {
        NSApp.mainMenu = MainMenu.make(target: self)
        do {
            var configuration = try BDKConfiguration.load()
            if let directory = options.supportDirectory {
                configuration.launch.userDataDirectory = directory.appendingPathComponent("Engine")
            }
            try extensions.install(in: configuration)
            try pages.install(in: configuration)
            runtime = try RuntimeSource.start(configuration)
        } catch {
            let alert = NSAlert()
            alert.messageText = "Arc-style Browser cannot start"
            alert.informativeText = "\(error)"
            alert.runModal()
            exit(1)
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        if !options.background { NSApp.activate() }
        Task { @MainActor in
            do {
                try await launch()
            } catch {
                log.error("launch failed: \(error)")
                errors.append("launch: \(error)")
                report()
            }
        }
    }

    private func launch() async throws {
        guard let runtime else { return }
        let controller = BrowserController(
            runtime: runtime, session: loadSession(), extensions: extensions, pages: pages)
        controller.onChange = { [weak self] in self?.report() }
        controller.showsWindowsInBackground = options.background
        controller.windowSubtitle = RuntimeSource.notice
        self.controller = controller
        try await controller.start()
        phase = .running
        await controller.refreshExtensionActions()
        for action in options.actions {
            do {
                try await controller.perform(action)
            } catch {
                log.error("\(String(describing: action)) failed: \(error)")
                errors.append("\(action): \(error)")
            }
        }
        actionsDone = true
        report()
        if options.quitAfterActions {
            // From a run-loop timer, as a menu would: the quit waits for the runtime to shut down.
            Timer.scheduledTimer(withTimeInterval: 0.2, repeats: false) { _ in
                MainActor.assumeIsolated { NSApp.terminate(nil) }
            }
        }
    }

    private func loadSession() -> SessionFile? {
        guard !options.fresh, let directory = supportDirectory else { return nil }
        do {
            return try SessionFile.load(from: directory)
        } catch {
            log.error("starting from the initial layout: \(error)")
            errors.append("session: \(error)")
            return nil
        }
    }

    // MARK: Menus

    @objc func newPage(_ sender: Any?) {
        run { try await $0.open(URL(string: "about:blank") ?? URL(fileURLWithPath: "/")) }
    }

    @objc func splitNext(_ sender: Any?) { run { try $0.splitNext() } }
    @objc func closePage(_ sender: Any?) { run { try await $0.perform(.closePage) } }

    @objc func installExtension(_ sender: Any?) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.message = "Choose an unpacked extension's directory"
        guard panel.runModal() == .OK, let directory = panel.url else { return }
        run { try await $0.installExtension(directory) }
    }
    @objc func openSettings(_ sender: Any?) { run { try await $0.openSettings() } }
    @objc func runAutomation(_ sender: Any?) { run { try await $0.perform(.automation) } }

    @objc func newWindow(_ sender: Any?) {
        run { controller in
            let space = try controller.layout.window(controller.focusedWindow).space
            let name = try controller.layout.space(space).name
            try await controller.perform(.newWindow(name))
        }
    }

    @objc func nextSpace(_ sender: Any?) {
        run { controller in
            let spaces = controller.layout.spaces
            let current = try controller.layout.window(controller.focusedWindow).space
            let index = spaces.firstIndex { $0.id == current } ?? 0
            try await controller.perform(.switchSpace(spaces[(index + 1) % spaces.count].name))
        }
    }

    private func run(_ body: @escaping @MainActor (BrowserController) async throws -> Void) {
        guard let controller else { return }
        Task { @MainActor in
            do {
                try await body(controller)
            } catch {
                log.error("menu action failed: \(error)")
                NSSound.beep()
            }
        }
    }

    // MARK: Report

    private func report() {
        guard let file = options.statusFile, let controller, !didReportTermination else { return }
        if phase == .terminating { didReportTermination = true }
        let status = controller.status(
            phase: phase, runtimeName: RuntimeSource.name, actionsDone: actionsDone, errors: errors)
        do {
            try status.write(to: file)
        } catch {
            log.error("status file not written: \(error)")
        }
    }

    // MARK: Quit

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // Called twice: once to shut the runtime down, then again with the runtime stopped.
        if BDKRuntime.isRunning {
            saveSession()
            controller?.isQuitting = true
            phase = .terminating
            report()
        }
        return runtime?.terminate(replyingTo: sender) ?? .terminateNow
    }

    /// Saves the layout and each page's state while the pages are alive.
    private func saveSession() {
        guard let controller, let directory = supportDirectory else { return }
        do {
            try controller.sessionFile().save(to: directory)
        } catch {
            log.error("session not saved: \(error)")
        }
    }
}
