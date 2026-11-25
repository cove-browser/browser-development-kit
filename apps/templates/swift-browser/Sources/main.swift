import AppKit
import BDK

// The bundle's NSPrincipalClass is BDKApplication; creating it here makes the same choice for a run outside
// the bundle, such as from the debugger.
let application = BDKApplication.shared
let delegate = AppDelegate()
application.delegate = delegate
application.setActivationPolicy(.regular)
application.run()
