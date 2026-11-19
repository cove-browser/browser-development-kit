import AppKit
import BDK
import MinimalBrowserCore
import MinimalBrowserKit
import os

/// Starts the runtime from the bundled configuration, opens a profile and two pages, saves their session on
/// quit and restores it on the next launch.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    /// The pages the example opens when there is no saved session.
    static let homePages = [
        URL(string: "https://example.com/")!, URL(string: "https://example.org/")!,
    ]

    private let options: AppOptions
    private let log = Logger(subsystem: "dev.bdk.examples.minimal-browser", category: "app")
    private var runtime: BDKRuntime?
    private var browser: BrowserWindow?
    private var phase = AppStatus.Phase.launching
    private var restored = false
    private var didQuitAfterReport = false
    private var didReportTermination = false
    private var launchFinished = false
    private var privateProfiles: [BDKProfile] = []

    init(options: AppOptions) {
        self.options = options
    }

    private var store: SessionStore? {
        let directory =
            options.supportDirectory
            ?? (try? SessionStore.defaultDirectory(
                bundleIdentifier: Bundle.main.bundleIdentifier ?? "minimal-browser"))
        return directory.map(SessionStore.init(directory:))
    }

    // MARK: Launch

    func applicationWillFinishLaunching(_ notification: Notification) {
        do {
            var configuration = try BDKConfiguration.load()
            // A test run keeps the engine's data (profiles, cookies, caches) beside its session.
            if let directory = options.supportDirectory {
                configuration.launch.userDataDirectory = directory.appendingPathComponent("Engine")
            }
            runtime = try RuntimeSource.start(configuration)
        } catch {
            // Never continue without a runtime.
            let alert = NSAlert()
            alert.messageText = "Minimal Browser cannot start"
            alert.informativeText = "\(error)"
            alert.runModal()
            exit(1)
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.activate()
        Task { @MainActor in
            do {
                try await openBrowser()
            } catch {
                log.error("launch failed: \(error)")
                report()
            }
        }
    }

    private func openBrowser() async throws {
        guard let runtime else { return }
        var pages: [BDKWebContents] = []
        if options.privatePages {
            // Each page in its own private profile: nothing is shared between them or kept after quit.
            for _ in 0..<Self.homePages.count {
                // A profile closes, with its pages, when its last reference goes.
                let profile = try runtime.profiles.openPrivate()
                privateProfiles.append(profile)
                pages.append(try profile.webContents.create(.init()))
            }
        } else {
            let profile = try await runtime.profiles.open("Personal")
            let saved = loadSession()
            for index in 0..<Self.homePages.count {
                let state = saved?.pages.indices.contains(index) == true ? saved?.pages[index] : nil
                pages.append(try profile.webContents.create(.init(restoring: state)))
                restored = restored || state != nil
            }
        }
        let browser = BrowserWindow(pages: pages, notice: RuntimeSource.notice)
        browser.onChange = { [weak self] in self?.report() }
        self.browser = browser
        phase = .running
        browser.show()
        if !restored {
            for (pane, home) in zip(browser.panes, Self.homePages) { await pane.navigate(to: home) }
        }
        // Each one loads completely before the next starts, so a test can depend on their order.
        for navigation in options.navigations where browser.panes.indices.contains(navigation.page)
        {
            let pane = browser.panes[navigation.page]
            await pane.navigate(to: navigation.url)
            await pane.waitUntilLoaded()
        }
        for index in options.crashPages where browser.panes.indices.contains(index) {
            // A debug URL Chromium handles itself: the renderer crashes, and the pane reloads the page.
            let page = browser.panes[index].page
            Task { _ = try? await page.navigate(to: URL(string: "chrome://crash")!) }
        }
        for pane in browser.panes { pane.refresh() }
        launchFinished = true
        report()
        // Layout settles after the window is on screen; report again from there.
        try? await Task.sleep(for: .milliseconds(200))
        report()
        if let file = options.snapshotFile {
            do {
                try browser.snapshotPNG()?.write(to: file, options: .atomic)
            } catch {
                log.error("snapshot not written: \(error)")
            }
        }
    }

    private func loadSession() -> SessionStore.Snapshot? {
        do {
            return try store?.load()
        } catch {
            log.error("starting without a session: \(error)")
            return nil
        }
    }

    // MARK: Report

    private func report() {
        // The report made when quitting starts is the last: the shutdown that follows ends the pages.
        if didReportTermination { return }
        if phase == .terminating { didReportTermination = true }
        let status = AppStatus(
            phase: phase, runtime: RuntimeSource.name, restored: restored,
            windows: browser.map { [$0.status] } ?? [])
        if let file = options.statusFile {
            do {
                try status.write(to: file)
            } catch {
                log.error("status file not written: \(error)")
            }
        }
        // Not before the launch has finished: with the real runtime the home pages can load before the
        // navigations the options ask for have run.
        if options.quitAfterReport, launchFinished, !didQuitAfterReport,
            status.showsPages(Self.homePages.count, loaded: true),
            status.recovered(pages: options.crashPages)
        {
            didQuitAfterReport = true
            // Quit from a run-loop timer, as a menu or an Apple event would: the quit waits for the runtime to
            // shut down, and a nested run loop inside a main-actor job or a dispatch callout would never run
            // the shutdown task.
            Timer.scheduledTimer(withTimeInterval: 0, repeats: false) { _ in
                MainActor.assumeIsolated { NSApp.terminate(nil) }
            }
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
            phase = .terminating
            report()
        }
        return runtime?.terminate(replyingTo: sender) ?? .terminateNow
    }

    /// Saves the session while the pages are still alive: the shutdown that follows ends them. With no
    /// window (the launch had not finished) the previous session is kept rather than replaced by nothing.
    private func saveSession() {
        guard !options.privatePages, let browser, let store else { return }
        do {
            let states = try browser.panes.map { try $0.page.sessionState() }
            try store.save(.init(pages: states))
        } catch {
            log.error("session not saved: \(error)")
        }
    }
}
