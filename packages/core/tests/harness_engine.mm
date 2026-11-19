/* Harness for the Chromium engine (the packaged runtime's framework). It is an application: the
   framework's start-up needs NSApplication and an app bundle around it (helpers, resources), so the
   suite runs as BDKEngineConformance.app, built by CMake with -DBDK_ENGINE_FRAMEWORK=<dir>.

   Chromium starts once per process, so the harness sets BDK_ENGINE_KEEP_CHROMIUM: each test's
   runtime shutdown leaves Chromium running. A first runtime starts Chromium; its main loop then
   runs nested inside the application's, and the suite runs from a run-loop block inside that loop,
   so pumping (a nested run of the run loop) lets Chromium work. Navigations never reach the
   network: every http and https load is served by the engine's test interceptor (bdk_testing.h). */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "bdk_mac/bdk_application.h"
#include "bdk_testing.h"
#include "harness.h"
extern "C" {
#include "test_support.h"
}

@interface BDKHarnessApplication : BDKApplicationBase
@end
@implementation BDKHarnessApplication
@end

namespace {

std::string g_data_dir;
int g_argc;
char** g_argv;

bdk_string_t View(const char* s) {
    return bdk_string_t{s, s ? strlen(s) : 0};
}

} // namespace

extern "C" {

/* Set by a test that checks what persists between runtimes. */
int bdk_harness_engine_keep_profiles = 0;

void bdk_harness_prepare_start_config(bdk_start_config_t* config) {
    /* The suite expects each runtime to start with no profiles, as with a new data directory. */
    if (!bdk_harness_engine_keep_profiles)
        bdk_testing_forget_profiles();
    /* Scripted navigations belong to one runtime, as with the stub. */
    bdk_testing_intercept_all(0);
    bdk_testing_intercept_all(1);
    /* The engine needs a browser name for its identity; everything else is the suite's. */
    config->resolved_config_json = View("{\"browser\":{\"name\":\"BDK Conformance\"}}");
    config->user_data_path_override = View(g_data_dir.c_str());
}

size_t bdk_harness_pump(void) {
    static size_t last_windows;
    if (getenv("BDK_TEST_TRACE")) {
        size_t windows = bdk_testing_chrome_window_count();
        if (windows != last_windows)
            printf("    chrome windows: %zu\n", windows);
        last_windows = windows;
    }
    /* Idle means no BDK task queued and no page loading, three slices running. */
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:20];
    int idle = 0;
    size_t slices = 0;
    while (idle < 3 && [deadline timeIntervalSinceNow] > 0) {
        bdk_testing_run_for(0.005);
        ++slices;
        idle = bdk_testing_pending_work() == 0 ? idle + 1 : 0;
    }
    return slices;
}

void bdk_harness_run_on_other_thread(void (*fn)(void*), void* ctx) {
    std::thread t([fn, ctx] { fn(ctx); });
    t.join();
}

bdk_status_t bdk_harness_script_navigation(bdk_runtime_t rt, const char* url, int32_t net_error,
                                           const char* title) {
    return bdk_testing_script_navigation(rt, View(url), net_error, View(title));
}

bdk_status_t bdk_harness_raise_ui_request(bdk_runtime_t rt, uint32_t surface, bdk_contents_t c,
                                          const char* payload) {
    return bdk_testing_raise_ui_request(rt, surface, c, View(payload));
}

bdk_status_t bdk_harness_withdraw_ui_requests(bdk_runtime_t rt) {
    return bdk_testing_withdraw_ui_requests(rt);
}

bdk_status_t bdk_harness_last_ui_outcome(bdk_runtime_t rt, bdk_harness_ui_outcome_t* out) {
    bdk_testing_ui_outcome_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    bdk_status_t s = bdk_testing_last_ui_outcome(rt, &o);
    if (s != BDK_OK)
        return s;
    memset(out, 0, sizeof *out);
    out->kind = o.kind;
    memcpy(out->completion, o.completion, sizeof out->completion);
    memcpy(out->arguments_json, o.arguments_json, sizeof out->arguments_json);
    return BDK_OK;
}

bdk_status_t bdk_harness_simulate_key(bdk_contents_t c, uint8_t* out_reserved) {
    bdk_native_handle_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = BDK_NATIVE_NSEVENT;
    ev.id = 1;
    return bdk_testing_simulate_key(c, ev, out_reserved);
}

void bdk_harness_attach_view(bdk_contents_t contents) {
    /* Every attached page goes in one window, side by side, so focus can move between them. */
    static NSWindow* shared;
    static CGFloat next_x;
    if (!shared) {
        shared = [[NSWindow alloc] initWithContentRect:NSMakeRect(60, 60, 1280, 480)
                                             styleMask:NSWindowStyleMaskTitled
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        shared.releasedWhenClosed = NO;
        [shared makeKeyAndOrderFront:nil];
    }
    bdk_native_handle_t view;
    bdk_error_t error = 0;
    if (bdk_contents_native_view(contents, &view, &error) != BDK_OK || !view.pointer)
        return;
    NSView* page = (__bridge NSView*)view.pointer;
    page.frame = NSMakeRect(next_x, 0, 320, 480);
    next_x = next_x >= 960 ? 0 : next_x + 320;
    [shared.contentView addSubview:page];
    bdk_harness_pump();
}

const char* bdk_harness_session_format(void) {
    return "BDKC1";
}

size_t bdk_harness_newer_session_state(uint8_t* out, size_t cap) {
    /* A Chromium pickle: payload size 8, then the magic "BDKC" and format version 2. */
    static const uint8_t newer[] = {8, 0, 0, 0, 'B', 'D', 'K', 'C', 2, 0, 0, 0};
    size_t n = sizeof newer < cap ? sizeof newer : cap;
    memcpy(out, newer, n);
    return n;
}

void bdk_harness_engine_show(bdk_contents_t contents, int visible) {
    static NSMutableDictionary<NSNumber*, NSWindow*>* windows;
    if (!windows)
        windows = [NSMutableDictionary dictionary];
    NSNumber* key = @(contents);
    NSWindow* window = windows[key];
    if (!window) {
        bdk_native_handle_t view;
        bdk_error_t error = 0;
        if (bdk_contents_native_view(contents, &view, &error) != BDK_OK || !view.pointer)
            return;
        window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(80, 80, 640, 480)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.releasedWhenClosed = NO;
        NSView* page = (__bridge NSView*)view.pointer;
        page.frame = window.contentView.bounds;
        page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [window.contentView addSubview:page];
        windows[key] = window;
    }
    /* Hidden the way an application hides a page: the view hidden, its window off screen. */
    NSView* page = window.contentView.subviews.firstObject;
    page.hidden = !visible;
    if (visible) {
        [window makeKeyAndOrderFront:nil];
    } else {
        [window orderOut:nil];
    }
    bdk_harness_pump();
}

/* The window bdk_harness_engine_show made for the page, or nil. */
static NSWindow* WindowOf(bdk_contents_t contents) {
    bdk_native_handle_t view;
    bdk_error_t error = 0;
    if (bdk_contents_native_view(contents, &view, &error) != BDK_OK || !view.pointer)
        return nil;
    return ((__bridge NSView*)view.pointer).window;
}

void bdk_harness_engine_key(bdk_contents_t contents, unsigned short key_code,
                            const char* characters, int command) {
    NSWindow* window = WindowOf(contents);
    if (!window)
        return;
    NSString* chars = [NSString stringWithUTF8String:characters];
    for (NSEventType type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
        NSEvent* event = [NSEvent keyEventWithType:type
                                          location:NSZeroPoint
                                     modifierFlags:command ? NSEventModifierFlagCommand : 0
                                         timestamp:[NSProcessInfo processInfo].systemUptime
                                      windowNumber:window.windowNumber
                                           context:nil
                                        characters:chars
                       charactersIgnoringModifiers:chars
                                         isARepeat:NO
                                           keyCode:key_code];
        /* As the window server delivers it: key equivalents, then the first responder. */
        [NSApp sendEvent:event];
    }
    bdk_harness_pump();
}

int bdk_harness_engine_window_is_key(bdk_contents_t contents) {
    return WindowOf(contents).isKeyWindow ? 1 : 0;
}

int bdk_harness_engine_compose(bdk_contents_t contents, const char* marked, const char* text) {
    NSWindow* window = WindowOf(contents);
    id responder = window.firstResponder;
    if (![responder conformsToProtocol:@protocol(NSTextInputClient)])
        return 0;
    /* An input method works inside -interpretKeyEvents: of each key-down: it marks text while
       composing and inserts the result when the user commits. Stand in for one on the page's view
       for two keys: "1" composes `marked`, "2" commits `text`. */
    Class cls = [responder class];
    SEL sel = @selector(interpretKeyEvents:);
    Method method = class_getInstanceMethod(cls, sel);
    IMP original = method_getImplementation(method);
    NSString* m = [NSString stringWithUTF8String:marked];
    NSString* t = [NSString stringWithUTF8String:text];
    IMP ime = imp_implementationWithBlock(^(id view, NSArray<NSEvent*>* events) {
      id<NSTextInputClient> client = view;
      if ([events.firstObject.characters isEqualToString:@"1"]) {
          [client setMarkedText:m
                  selectedRange:NSMakeRange(m.length, 0)
               replacementRange:NSMakeRange(NSNotFound, 0)];
      } else {
          [client insertText:t replacementRange:NSMakeRange(NSNotFound, 0)];
      }
    });
    method_setImplementation(method, ime);
    bdk_harness_engine_key(contents, 18 /* kVK_ANSI_1 */, "1", 0);
    bdk_harness_engine_key(contents, 19 /* kVK_ANSI_2 */, "2", 0);
    method_setImplementation(method, original);
    imp_removeBlock(ime);
    return 1;
}

static void CollectAccessibility(id element, int depth, NSMutableString* out, int* web_area) {
    if (!element || depth > 60 || out.length > 4000)
        return;
    NSString* role = [element respondsToSelector:@selector(accessibilityRole)]
                         ? [element accessibilityRole]
                         : nil;
    if ([role isEqualToString:@"AXWebArea"])
        *web_area = 1;
    if (getenv("BDK_TEST_TRACE"))
        printf("    ax %*s%s %s\n", depth, "", role ? role.UTF8String : "-",
               NSStringFromClass([element class]).UTF8String);
    for (SEL sel : {
             @selector(accessibilityValue), @selector(accessibilityTitle),
                 @selector(accessibilityLabel)
         }) {
        if (![element respondsToSelector:sel])
            continue;
        id value = ((id (*)(id, SEL))[element methodForSelector:sel])(element, sel);
        if ([value isKindOfClass:[NSString class]] && [value length]) {
            [out appendString:value];
            [out appendString:@"|"];
        }
    }
    NSArray* children = [element respondsToSelector:@selector(accessibilityChildren)]
                            ? [element accessibilityChildren]
                            : nil;
    /* A view that is not an accessibility element is ignored, and its descendants count as its
       parent's children, as VoiceOver sees them. */
    if ([element isKindOfClass:[NSView class]] && ![element isAccessibilityElement] &&
        [element subviews].count)
        children = [element subviews];
    for (id child in children)
        CollectAccessibility(child, depth + 1, out, web_area);
}

int bdk_harness_engine_accessibility_text(bdk_contents_t contents, char* out, size_t n) {
    bdk_native_handle_t view;
    bdk_error_t error = 0;
    if (bdk_contents_native_view(contents, &view, &error) != BDK_OK || !view.pointer)
        return 0;
    /* What VoiceOver does when it starts: Chromium's application class turns page accessibility
       on in response (BDKApplicationBase inherits it). */
    static bool announced;
    if (!announced) {
        announced = true;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp accessibilitySetValue:@YES forAttribute:@"AXEnhancedUserInterface"];
#pragma clang diagnostic pop
        /* Chromium waits two seconds for the setting to settle before turning accessibility on. */
        bdk_testing_run_for(2.5);
    }
    NSMutableString* text = [NSMutableString string];
    int web_area = 0;
    NSView* page = (__bridge NSView*)view.pointer;
    CollectAccessibility(page.window ? (id)page.window.contentView : (id)page, 0, text, &web_area);
    snprintf(out, n, "%s", text.UTF8String);
    return web_area;
}

} /* extern "C" */

namespace {

bdk_runtime_t g_warmup;
int g_warmup_stopped;

void RunSuite(void*) {
    bdk_runtime_shutdown(
        g_warmup, [](void*, bdk_error_t, bdk_string_t, bdk_buffer_t) { g_warmup_stopped = 1; },
        nullptr, nullptr);
    bdk_harness_pump();
    if (!g_warmup_stopped) {
        fprintf(stderr, "the warm-up runtime did not shut down\n");
        exit(2);
    }
    int rc = bdk_conformance_main(g_argc, g_argv);
    /* BDK never opens a Chrome window; one here means Chromium opened it on its own. */
    if (size_t windows = bdk_testing_chrome_window_count()) {
        printf("FAIL: Chromium opened %zu browser window(s) of its own\n", windows);
        rc = 1;
    }
    fflush(stdout);
    exit(rc);
}

} // namespace

@interface BDKHarnessDelegate : NSObject <NSApplicationDelegate>
@end

@implementation BDKHarnessDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    /* Starts Chromium with a first runtime. Its nested main loop is queued on the run loop; the
       block after it runs inside that loop. */
    bdk_start_config_t config;
    t_fill_start_config(&config);
    bdk_runtime_t warmup = 0;
    bdk_error_t error = 0;
    if (bdk_runtime_start(&config, &warmup, &error) != BDK_OK) {
        bdk_string_t message = bdk_error_message(error);
        fprintf(stderr, "the engine did not start: %.*s\n", (int)message.length,
                message.data ? message.data : "");
        exit(2);
    }
    bdk_testing_intercept_all(1);
    /* Frontmost, so its windows become key: Chromium counts focus only in the key window. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    g_warmup = warmup;
    /* Posted through the engine, so it runs as a Chromium task inside Chromium's main loop, which
       must be the outer one for the suite's nested pumping to let Chromium work. */
    bdk_post_to_ui_thread(RunSuite, nullptr);
}

@end

int main(int argc, char** argv) {
    @autoreleasepool {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setenv("BDK_ENGINE_KEEP_CHROMIUM", "1", 1);
        setenv("BDK_ENGINE_MOCK_KEYCHAIN", "1", 1);
        /* Chromium refuses fullscreen to a page whose window is not key. macOS does not let a
           process make itself frontmost while another application is (cooperative activation), so
           the harness's window cannot be relied on to be key; the fullscreen scenarios test BDK's
           requests, not that policy. */
        setenv("BDK_ENGINE_SWITCHES", "disable-features=SuppressFullscreenFromUnfocusedView", 0);
        char dir[] = "/tmp/bdk-engine-conformance-XXXXXX";
        if (!mkdtemp(dir)) {
            perror("mkdtemp");
            return 2;
        }
        g_data_dir = dir;
        g_argc = argc;
        g_argv = argv;
        [BDKHarnessApplication sharedApplication];
        BDKHarnessDelegate* delegate = [[BDKHarnessDelegate alloc] init];
        NSApp.delegate = delegate;
        /* A regular, active application: its windows become key, so pages can take focus and
           enter fullscreen, and Chromium sees them shown and hidden. */
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp run];
    }
    return 0;
}
