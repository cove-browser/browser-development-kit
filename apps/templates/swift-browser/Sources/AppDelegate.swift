import AppKit
import BDK

/// Starts the runtime from the bundled configuration (bdk.resolved.json, written by `bdk build config`), opens
/// one window with one page, and shuts the runtime down when the application quits.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    static let homePage = URL(string: "https://example.com/")!

    private var runtime: BDKRuntime?
    private var browser: BrowserWindow?

    func applicationWillFinishLaunching(_ notification: Notification) {
        do {
            let configuration = try BDKConfiguration.load()
            runtime = try RuntimeSource.start(configuration)
        } catch {
            // Never continue without a runtime.
            let alert = NSAlert()
            alert.messageText = "{{name}} cannot start"
            alert.informativeText = "\(error)"
            alert.runModal()
            exit(1)
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.activate()
        Task { @MainActor in
            guard let runtime else { return }
            do {
                let profile = try await runtime.profiles.open("Personal")
                let page = try profile.webContents.create(.init())
                let browser = BrowserWindow(page: page, notice: RuntimeSource.notice)
                self.browser = browser
                browser.show()
                await browser.navigate(to: Self.homePage)
            } catch {
                let alert = NSAlert()
                alert.messageText = "{{name}} could not open a window"
                alert.informativeText = "\(error)"
                alert.runModal()
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // Called twice: once to shut the runtime down, then again with the runtime stopped.
        runtime?.terminate(replyingTo: sender) ?? .terminateNow
    }
}
