import AppKit

/// The application class of a BDK application: set it as `NSPrincipalClass`, or subclass it.
///
/// Chromium's application-protocol conformance and the routing of `sendEvent(_:)` through
/// Chromium's event handling arrive with the engine (T-MAC-01 and T-MAC-02). Until then this class
/// behaves exactly like `NSApplication`.
@MainActor
open class BDKApplication: NSApplication {}
