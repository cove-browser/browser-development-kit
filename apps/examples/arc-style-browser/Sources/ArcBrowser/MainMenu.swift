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
        file.addItem(.separator())
        item(in: file, "Close Page", #selector(AppDelegate.closePage(_:)), "w", target)
        let closeWindow = file.addItem(
            withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)),
            keyEquivalent: "w")
        closeWindow.keyEquivalentModifierMask = [.command, .shift]
        file.addItem(.separator())
        item(
            in: file, "Install Unpacked Extension…", #selector(AppDelegate.installExtension(_:)),
            "",
            target)
        bar.addItem(submenu: file, title: "File")
        bar.addItem(submenu: edit(), title: "Edit")

        let view = NSMenu(title: "View")
        item(in: view, "Split with Next Page", #selector(AppDelegate.splitNext(_:)), "d", target)
        let nextSpace = view.addItem(
            withTitle: "Next Space", action: #selector(AppDelegate.nextSpace(_:)),
            keyEquivalent: "\u{F703}")  // NSRightArrowFunctionKey
        nextSpace.keyEquivalentModifierMask = [.command, .option]
        nextSpace.target = target
        bar.addItem(submenu: view, title: "View")

        // The focused page's history; ⌘[ and ⌘] as in Safari and Arc.
        let history = NSMenu(title: "Go")
        item(in: history, "Reload Page", #selector(AppDelegate.reload(_:)), "r", target)
        item(in: history, "Back", #selector(AppDelegate.goBack(_:)), "[", target)
        item(in: history, "Forward", #selector(AppDelegate.goForward(_:)), "]", target)
        bar.addItem(submenu: history, title: "Go")

        let tools = NSMenu(title: "Tools")
        item(
            in: tools, "Run Automation over Two Spaces", #selector(AppDelegate.runAutomation(_:)),
            "", target)
        bar.addItem(submenu: tools, title: "Tools")

        let window = NSMenu(title: "Window")
        window.addItem(
            withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)),
            keyEquivalent: "m")
        window.addItem(
            withTitle: "Zoom", action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
        bar.addItem(submenu: window, title: "Window")
        NSApp.windowsMenu = window
        return bar
    }

    /// The standard edit actions, sent to the first responder: the focused page's view (Chromium's
    /// own, which handles them; swift-api.md section 9) or the sidebar's address field. Without
    /// this menu, Command-C and Command-V reach neither.
    private static func edit() -> NSMenu {
        let edit = NSMenu(title: "Edit")
        // An upper-case key equivalent adds Shift.
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        edit.addItem(.separator())
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        let plain = edit.addItem(
            withTitle: "Paste and Match Style",
            action: #selector(NSTextView.pasteAsPlainText(_:)), keyEquivalent: "V")
        plain.keyEquivalentModifierMask = [.command, .option, .shift]
        edit.addItem(withTitle: "Delete", action: #selector(NSText.delete(_:)), keyEquivalent: "")
        edit.addItem(
            withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        return edit
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
