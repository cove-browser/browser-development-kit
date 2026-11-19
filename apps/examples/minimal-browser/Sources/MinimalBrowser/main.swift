import AppKit
import BDK
import MinimalBrowserCore

// The bundle's NSPrincipalClass is BDKApplication; creating it here makes the same choice for a run outside
// a bundle.
let application = BDKApplication.shared
do {
    let delegate = AppDelegate(options: try AppOptions.parse(CommandLine.arguments))
    application.delegate = delegate
    application.setActivationPolicy(.regular)
    application.run()
} catch {
    FileHandle.standardError.write(Data("minimal-browser: \(error)\n".utf8))
    exit(2)
}
