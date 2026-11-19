/* BDK on macOS: the application base class. Not part of the C ABI (include/bdk): it is the one
   Objective-C type a runtime exports, because the application object itself must carry Chromium's
   event handling (architecture.md section 6.1). The Swift SDK's BDKApplication subclasses it.

   The real runtime implements it as a subclass of Chromium's own application class, so that
   sendEvent:, accessibility activation and the message pump's protocols behave as in Chrome. Its
   terminate: is NSApplication's: the application decides when to quit and shuts the runtime down
   from applicationShouldTerminate:. The stub runtime implements it as a plain NSApplication
   subclass. */
#ifndef BDK_MAC_BDK_APPLICATION_H_
#define BDK_MAC_BDK_APPLICATION_H_

#if defined(__OBJC__)
#import <AppKit/AppKit.h>

__attribute__((visibility("default")))
@interface BDKApplicationBase : NSApplication
@end
#endif

#endif /* BDK_MAC_BDK_APPLICATION_H_ */
