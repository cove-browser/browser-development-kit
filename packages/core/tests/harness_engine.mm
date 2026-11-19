/* Harness for the Chromium engine (the packaged runtime's framework). It is an application: the
   framework's start-up needs NSApplication and an app bundle around it (helpers, resources), so the
   suite runs as BDKEngineConformance.app, built by CMake with -DBDK_ENGINE_FRAMEWORK=<dir>.

   Chromium starts once per process, so the harness sets BDK_ENGINE_KEEP_CHROMIUM: each test's
   runtime shutdown leaves Chromium running. A first runtime starts Chromium; its main loop then
   runs nested inside the application's, and the suite runs from a run-loop block inside that loop,
   so pumping (a nested run of the run loop) lets Chromium work. Navigations never reach the
   network: every http and https load is served by the engine's test interceptor (bdk_testing.h). */
#import <AppKit/AppKit.h>

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
