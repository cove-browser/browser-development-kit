/* The engine conformance app's windows (harness_engine.mm), shared by the harness files that open
   windows (harness_engine_ext.mm) so every one follows BDK_ENGINE_SILENT. Objective-C++ only. */
#ifndef BDK_TESTS_HARNESS_ENGINE_WINDOWS_H_
#define BDK_TESTS_HARNESS_ENGINE_WINDOWS_H_

#import <AppKit/AppKit.h>

namespace bdk_harness {

/* BDK_ENGINE_SILENT=1: the harness stays out of the way of whoever is using the Mac. */
bool Silent();
/* A harness window, which a silent run keeps out of fullscreen (its own Space). */
NSWindow* NewWindow(NSRect frame, NSWindowStyleMask style);
/* Shows a harness window: in front, and key when `key`; behind every other window when silent. */
void ShowWindow(NSWindow* window, bool key);

} // namespace bdk_harness

#endif /* BDK_TESTS_HARNESS_ENGINE_WINDOWS_H_ */
