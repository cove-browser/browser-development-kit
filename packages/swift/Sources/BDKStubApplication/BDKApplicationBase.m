// The stub runtime's BDKApplicationBase (packages/core/include/bdk_mac/bdk_application.h): a plain
// NSApplication subclass, because the stub has no Chromium to route events through. The real runtime
// exports its own, a subclass of Chromium's application class.
#import "bdk_mac/bdk_application.h"

@implementation BDKApplicationBase
@end
