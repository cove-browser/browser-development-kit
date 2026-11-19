import AppKit
import CBDK

/// The application class of a BDK application: set it as `NSPrincipalClass`, or subclass it.
///
/// It subclasses the runtime's `BDKApplicationBase` (`packages/core/include/bdk_mac/bdk_application.h`).
/// In the real runtime that is a subclass of Chromium's application class, so `sendEvent(_:)` goes through
/// Chromium's event handling and VoiceOver turns on page accessibility as in Chrome; with the stub runtime it
/// is a plain `NSApplication`. `terminate(_:)` is `NSApplication`'s: quit as usual, and shut the runtime down
/// from `applicationShouldTerminate(_:)` with `BDKRuntime.terminate(replyingTo:)`.
@MainActor
open class BDKApplication: BDKApplicationBase {}
