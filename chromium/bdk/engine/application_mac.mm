// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The implementation of BDKApplicationBase, declared for applications in
// packages/core/include/bdk_mac/bdk_application.h as a subclass of
// NSApplication. Here it subclasses Chromium's BrowserCrApplication, so the
// application object routes sendEvent: through Chromium, conforms to the
// message pump's protocols and turns on accessibility for VoiceOver as Chrome
// does. That header cannot be included here: it names another superclass.

#import <AppKit/AppKit.h>
#include <crt_externs.h>
#import <objc/runtime.h>

#include "base/command_line.h"
#import "chrome/browser/chrome_browser_application_mac.h"

__attribute__((visibility("default")))
@interface BDKApplicationBase : BrowserCrApplication
@end

@implementation BDKApplicationBase

// BrowserCrApplication's +initialize reads the process command line, and it
// runs when the application object is created, before the application starts
// the runtime. Give it the process's own arguments here; StartChromium()
// replaces them with Chromium's.
+ (void)load {
  if (!base::CommandLine::InitializedForCurrentProcess()) {
    base::CommandLine::Init(*_NSGetArgc(), *_NSGetArgv());
  }
}

// BrowserCrApplication sends both of these to Chrome's AppController, which a
// BDK application does not have. The application decides when to quit, and
// shuts the runtime down from applicationShouldTerminate:, so terminate: is
// NSApplication's own and there is no Chrome termination to cancel.
- (void)terminate:(id)sender {
  using Imp = void (*)(id, SEL, id);
  reinterpret_cast<Imp>(class_getMethodImplementation(
      [NSApplication class], _cmd))(self, _cmd, sender);
}

- (void)cancelTerminate:(id)sender {
}

@end
