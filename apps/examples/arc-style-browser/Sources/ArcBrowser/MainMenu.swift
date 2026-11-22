import AppKit

/// The menu bar. Every item calls into the same controller code as the sidebar and the tests.
@MainActor
enum MainMenu {
    static func make(target: AppDelegate) -> NSMenu {
        let bar = NSMenu()
        let app = NSMenu()
        app.addItem(
            withTitle: "Quit Arc-style Browser", action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q")
        bar.addItem(submenu: app, title: "Arc-style Browser")

        let file = NSMenu(title: "File")
        item(in: file, "New Page", #selector(AppDelegate.newPage(_:)), "t", target)
        item(in: file, "New Window", #selector(AppDelegate.newWindow(_:)), "n", target)
        item(in: file, "Settings", #selector(AppDelegate.openSettings(_:)), ",", target)
        item(
            in: file, "Install Unpacked Extension…", #selector(AppDelegate.installExtension(_:)),
            "",
            target)
        bar.addItem(submenu: file, title: "File")

        let view = NSMenu(title: "View")
        item(in: view, "Split with Next Page", #selector(AppDelegate.splitNext(_:)), "d", target)
        item(in: view, "Next Space", #selector(AppDelegate.nextSpace(_:)), "]", target)
        bar.addItem(submenu: view, title: "View")

        let tools = NSMenu(title: "Tools")
        item(
            in: tools, "Run Automation over Two Spaces", #selector(AppDelegate.runAutomation(_:)),
            "", target)
        bar.addItem(submenu: tools, title: "Tools")
        return bar
    }

    private static func item(
        in menu: NSMenu, _ title: String, _ action: Selector, _ key: String, _ target: AnyObject
    ) {
        let item = menu.addItem(withTitle: title, action: action, keyEquivalent: key)
        item.target = target
    }
}

extension NSMenu {
    fileprivate func addItem(submenu: NSMenu, title: String) {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.submenu = submenu
        addItem(item)
    }
}
