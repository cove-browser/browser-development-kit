/* Harness for the Chromium engine (the packaged runtime's framework). It is an application: the
   framework's start-up needs NSApplication and an app bundle around it (helpers, resources), so the
   suite runs as BDKEngineConformance.app, built by CMake with -DBDK_ENGINE_FRAMEWORK=<dir>.

   Chromium starts once per process, so the harness sets BDK_ENGINE_KEEP_CHROMIUM: each test's
   runtime shutdown leaves Chromium running. A first runtime starts Chromium; its main loop then
   runs nested inside the application's, and the suite runs from a run-loop block inside that loop,
   so pumping (a nested run of the run loop) lets Chromium work. Navigations never reach the
   network: every http and https load is served by the engine's test interceptor (bdk_testing.h). */
#import <AppKit/AppKit.h>
#include <libproc.h>
#import <objc/runtime.h>

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "bdk_mac/bdk_application.h"
#include "bdk_testing.h"
#include "harness.h"
#include "harness_engine_windows.h"
extern "C" {
#include "test_support.h"
/* tests_engine_profile_versions.c: prepares the launch for the scenario `name`, if it is one. */
void bdk_engine_launch_prepare(const char* name);
/* From tests_engine_web_apps.c and tests_engine_providers.c: launches that need Chromium started
   with Google push, or with the application's Google key. */
void bdk_engine_push_launch_prepare(const char* name);
void bdk_engine_google_key_launch_prepare(const char* name);
/* From tests_engine_payments.c: a start with the payments surface disabled. */
void bdk_engine_payments_launch_prepare(const char* name);
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

/* Removes a data directory the harness made itself. Registered before Chromium starts, so it runs
   after Chromium's own exit work; Chromium's threads can still write a little after it, and a run
   that crashes or is killed leaves the directory. A failed removal is reported on stderr.
   scripts/test-engine.sh owns the directory instead (BDK_ENGINE_DATA_DIR) and removes it once the
   process has gone. */
void RemoveDataDir() {
    std::error_code error;
    std::filesystem::remove_all(g_data_dir, error);
    if (error) {
        fprintf(stderr, "could not remove the engine suite's data directory %s: %s\n",
                g_data_dir.c_str(), error.message().c_str());
    }
}

} // namespace

namespace bdk_harness {

/* BDK_ENGINE_SILENT=1 runs the suite without getting in the way of whoever is using the Mac: the
   harness never activates, has no Dock icon or menu bar, keeps its windows behind every other
   window and never takes them fullscreen. Chromium is told to keep drawing a hidden page, as
   screenshots, captures and the accessibility tree need it drawn. What needs the harness
   frontmost (an input method's composition) is skipped, as when macOS refuses to bring it
   forward. */
bool Silent() {
    static const bool silent = [] {
        const char* value = getenv("BDK_ENGINE_SILENT");
        return value && *value && strcmp(value, "0") != 0;
    }();
    return silent;
}

NSWindow* NewWindow(NSRect frame, NSWindowStyleMask style) {
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    if (Silent())
        window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenNone;
    return window;
}

/* Shows a harness window: in front, and key when `key`. A silent run puts it behind every other
   window instead; it is still key within the harness, which is never the active application. */
void ShowWindow(NSWindow* window, bool key) {
    if (Silent()) {
        [window orderBack:nil];
        if (key)
            [window makeKeyWindow];
        return;
    }
    if (key)
        [window makeKeyAndOrderFront:nil];
    else
        [window orderFront:nil];
}

} // namespace bdk_harness

using bdk_harness::NewWindow;
using bdk_harness::ShowWindow;
using bdk_harness::Silent;

extern "C" {

/* Set by a test that checks what persists between runtimes. */
int bdk_harness_engine_keep_profiles = 0;
/* Set by a test that needs surfaces configured: the value of "surfaces" in the resolved
   configuration, for example {"notifications":"disabled"}. Cleared by the next start. */
const char* bdk_harness_engine_surfaces = NULL;
/* Set by a test that needs extensions configured: the value of "extensions" in the resolved
   configuration, for example {"ui":{"popups":"disabled"}}. Cleared by the next start. */
const char* bdk_harness_engine_extensions = NULL;

/* The bundle the engine reads internal pages from: the bundled routes of the page tests
   (tests_pages.c, tests_engine_pages.c) with fixture pages. Each page sets its title to
   "page:<route>" and, in its module, holds the page broker (bdkInvoke), so a scenario can call the
   broker as the page. */
static const char* kFixtureScript =
    "import {PageBroker, PageClientCallbackRouter} from '/@bdk/page_broker.mojom-webui.js';\n"
    "const broker = PageBroker.getRemote();\n"
    "const router = new PageClientCallbackRouter();\n"
    "broker.setClient(router.$.bindNewPipeAndPassRemote());\n"
    "let next = 1n;\n"
    "window.bdkInvoke = async (command, args) => {\n"
    "  const r = await broker.invoke(next++, command, JSON.stringify(args ?? {}));\n"
    "  document.title = (r.ok ? 'ok:' : 'err:') + r.resultJson;\n"
    "};\n"
    "document.title = 'page:' + location.host.replace(/^bdk-/, '');\n";

static void write_file(const std::string& path, const char* text) {
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static const std::string& fixture_bundle(void) {
    static std::string bundle;
    if (bundle.empty()) {
        bundle = g_data_dir + "/Conformance.app";
        const std::string resources = bundle + "/Contents/Resources";
        for (const char* page : {"Settings", "Agents"}) {
            const std::string dir = resources + "/Pages/" + page;
            std::string mkdir = "/bin/mkdir -p '" + dir + "'";
            if (system(mkdir.c_str()) != 0)
                continue;
            write_file(dir + "/index.html",
                       "<!doctype html><meta charset=utf-8><title>loading</title>"
                       "<script type=module src=./main.js></script>");
            write_file(dir + "/main.js", kFixtureScript);
        }
        /* Scripts and DevTools front ends are read from the bundle's Resources/BDK: this app's,
           where CMakeLists.txt packages the fixtures. */
        std::error_code ignored;
        std::filesystem::create_symlink(std::string(NSBundle.mainBundle.resourcePath.UTF8String) +
                                            "/BDK",
                                        resources + "/BDK", ignored);
    }
    return bundle;
}

const char* bdk_harness_engine_bundle_path(void) {
    return fixture_bundle().c_str();
}

const char* bdk_harness_engine_data_dir(void) {
    return g_data_dir.c_str();
}

void bdk_harness_prepare_start_config(bdk_start_config_t* config) {
    /* The suite expects each runtime to start with no profiles, as with a new data directory. */
    if (!bdk_harness_engine_keep_profiles)
        bdk_testing_forget_profiles();
    /* Scripted navigations belong to one runtime, as with the stub. */
    bdk_testing_intercept_all(0);
    bdk_testing_intercept_all(1);
    config->user_data_path_override = View(g_data_dir.c_str());
    config->bundle_path = View(fixture_bundle().c_str());
    /* A test that names its own configuration (t_start_with) gives the browser name. */
    std::string given(config->resolved_config_json.data ? config->resolved_config_json.data : "",
                      config->resolved_config_json.length);
    if (given.find("\"routes\"") != std::string::npos ||
        given.find("\"internal_scheme\"") != std::string::npos)
        return;
    /* The engine needs a browser name for its identity; everything else is the suite's. */
    static std::string resolved;
    resolved = "{\"browser\":{\"name\":\"BDK Conformance\"}";
    if (bdk_harness_engine_surfaces)
        resolved += std::string(",\"surfaces\":") + bdk_harness_engine_surfaces;
    if (bdk_harness_engine_extensions)
        resolved += std::string(",\"extensions\":") + bdk_harness_engine_extensions;
    if (t_config_extra) /* the suite's own members, such as [automation] */
        resolved += std::string(",") + t_config_extra;
    resolved += "}";
    bdk_harness_engine_surfaces = NULL;
    bdk_harness_engine_extensions = NULL;
    config->resolved_config_json = View(resolved.c_str());
}

int bdk_harness_engine_open_descriptors(void) {
    return proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0) / (int)sizeof(proc_fdinfo);
}

static size_t PumpUntilIdle(void);

/* The autorelease pool the outermost pump leaves for the scenario's code until the next pump.
   The suite runs as one Chromium task, inside one pass of the run loop, so nothing would drain
   what the scenarios autorelease: a page's views were kept after it was destroyed (T-REL-03). An
   application's events each drain their own. */
static void* g_scenario_pool;
static int g_pump_depth;

size_t bdk_harness_pump(void) {
    if (g_pump_depth == 0 && g_scenario_pool) {
        objc_autoreleasePoolPop(g_scenario_pool);
        g_scenario_pool = nullptr;
    }
    ++g_pump_depth;
    size_t slices = PumpUntilIdle();
    --g_pump_depth;
    if (g_pump_depth == 0)
        g_scenario_pool = objc_autoreleasePoolPush();
    return slices;
}

static size_t PumpUntilIdle(void) {
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

/* Page health is not raised by the engine yet (T-UIR-15): the tests that need these skip. */
bdk_status_t bdk_harness_page_unresponsive(bdk_contents_t) {
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_page_responsive(bdk_contents_t) {
    return BDK_ERR_INVALID_ARGUMENT;
}

bdk_status_t bdk_harness_block_popup(bdk_contents_t, const char*, uint8_t*) {
    return BDK_ERR_INVALID_ARGUMENT;
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
        shared = NewWindow(NSMakeRect(60, 60, 1280, 480), NSWindowStyleMaskTitled);
        ShowWindow(shared, true);
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

/* Application windows registered with the runtime (bdk_window_register), for the host-context
   scenarios. A window is its index in this list; closing one leaves a hole. */
static NSMutableArray<NSWindow*>* RegisteredWindows() {
    static NSMutableArray<NSWindow*>* windows;
    if (!windows)
        windows = [NSMutableArray array];
    return windows;
}

static NSMutableDictionary<NSNumber*, NSNumber*>* WindowHandles() {
    static NSMutableDictionary<NSNumber*, NSNumber*>* handles;
    if (!handles)
        handles = [NSMutableDictionary dictionary];
    return handles;
}

static int OpenRegisteredWindow(bdk_runtime_t rt, uint32_t kind) {
    NSWindow* window = NewWindow(NSMakeRect(100, 100, 640, 480), NSWindowStyleMaskTitled);
    ShowWindow(window, false);
    bdk_native_handle_t native;
    memset(&native, 0, sizeof native);
    native.kind = BDK_NATIVE_NSWINDOW;
    native.pointer = (__bridge void*)window;
    bdk_window_t handle = 0;
    bdk_error_t error = 0;
    if (bdk_window_register(rt, native, kind, &handle, &error) != BDK_OK)
        return -1;
    [RegisteredWindows() addObject:window];
    int index = (int)RegisteredWindows().count - 1;
    WindowHandles()[@(index)] = @(handle);
    return index;
}

/* A window registered as BDK_WINDOW_KIND_NORMAL (a popup-kind window's Browser takes one tab). */
int bdk_harness_engine_window_open(bdk_runtime_t rt) {
    return OpenRegisteredWindow(rt, BDK_WINDOW_KIND_NORMAL);
}

/* The bdk_window_t the registered window `window` (an index of the open functions) got. */
bdk_window_t bdk_harness_engine_window_handle(int window) {
    return (bdk_window_t)WindowHandles()[@(window)].unsignedLongLongValue;
}

/* Puts a native view (a popup's secure view) in the registered window, at the top right, as an
   application embeds it. */
void bdk_harness_engine_place_view(int window, bdk_native_handle_t view, double w, double h) {
    if (!view.pointer)
        return;
    NSWindow* ns_window = RegisteredWindows()[window];
    NSView* v = (__bridge NSView*)view.pointer;
    NSRect bounds = ns_window.contentView.bounds;
    v.frame = NSMakeRect(NSMaxX(bounds) - w, NSMaxY(bounds) - h, w, h);
    [ns_window.contentView addSubview:v];
    bdk_harness_pump();
}

void bdk_harness_engine_view_size(bdk_native_handle_t view, double* w, double* h) {
    NSSize size = view.pointer ? ((__bridge NSView*)view.pointer).frame.size : NSZeroSize;
    *w = size.width;
    *h = size.height;
}

void bdk_harness_engine_remove_view(bdk_native_handle_t view) {
    if (view.pointer)
        [(__bridge NSView*)view.pointer removeFromSuperview];
}

void bdk_harness_engine_window_attach(int window, bdk_contents_t contents) {
    bdk_native_handle_t view;
    bdk_error_t error = 0;
    if (bdk_contents_native_view(contents, &view, &error) != BDK_OK || !view.pointer)
        return;
    NSWindow* w = RegisteredWindows()[window];
    NSView* page = (__bridge NSView*)view.pointer;
    page.frame = w.contentView.bounds;
    page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    /* Adding a view that is in another window's view tree moves it. */
    [w.contentView addSubview:page];
    bdk_harness_pump();
}

/* Puts the page's view in column `slot` of `slots` equal columns of the registered window, as an
   application lays out a split. Like attach, adding a view from another window moves it. */
void bdk_harness_engine_window_place(int window, bdk_contents_t contents, int slot, int slots) {
    bdk_native_handle_t view;
    bdk_error_t error = 0;
    if (slots < 1 || bdk_contents_native_view(contents, &view, &error) != BDK_OK || !view.pointer)
        return;
    NSWindow* w = RegisteredWindows()[window];
    NSView* page = (__bridge NSView*)view.pointer;
    NSRect bounds = w.contentView.bounds;
    CGFloat width = bounds.size.width / slots;
    page.frame = NSMakeRect(width * slot, 0, width, bounds.size.height);
    page.autoresizingMask = NSViewNotSizable;
    [w.contentView addSubview:page];
    bdk_harness_pump();
}

void bdk_harness_engine_window_detach(bdk_contents_t contents) {
    bdk_native_handle_t view;
    bdk_error_t error = 0;
    if (bdk_contents_native_view(contents, &view, &error) != BDK_OK || !view.pointer)
        return;
    [(__bridge NSView*)view.pointer removeFromSuperview];
    bdk_harness_pump();
}

/* Unregisters the window and closes it, as an application does when a window goes. */
void bdk_harness_engine_window_close(int window) {
    NSWindow* w = RegisteredWindows()[window];
    bdk_error_t error = 0;
    bdk_window_unregister((bdk_window_t)WindowHandles()[@(window)].unsignedLongLongValue, &error);
    [w orderOut:nil];
    [w close];
    bdk_harness_pump();
}

size_t bdk_harness_engine_app_window_count(void) {
    return NSApp.windows.count;
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

int bdk_harness_own_data_directory(void) {
    /* Chromium starts once per process, with g_data_dir. The engine's half of the snapshot and
       downgrade policy is tests_engine_profile_versions.c, whose scenarios each prepare the data
       directory of a launch of their own (bdk_engine_launch_prepare, scripts/test-engine.sh). */
    return 0;
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
        window = NewWindow(NSMakeRect(80, 80, 640, 480),
                           NSWindowStyleMaskTitled | NSWindowStyleMaskResizable);
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
        ShowWindow(window, true);
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

/* The event a person's click or key press leaves for a handler to confirm a grant with. kind: 0 a
   recent mouse-up in the page's window, 1 a recent key-down, 2 a mouse-up a minute old, 3 a
   mouse-down, 4 a mouse-up that went to no window. */
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind) {
    static NSEvent* held;
    NSWindow* window = WindowOf(contents);
    NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    NSInteger number = kind == 4 ? 0 : window.windowNumber;
    if (kind == 1) {
        held = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                location:NSZeroPoint
                           modifierFlags:0
                               timestamp:now
                            windowNumber:number
                                 context:nil
                              characters:@"a"
             charactersIgnoringModifiers:@"a"
                               isARepeat:NO
                                 keyCode:0];
    } else {
        held = [NSEvent
            mouseEventWithType:kind == 3 ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp
                      location:NSMakePoint(10, 10)
                 modifierFlags:0
                     timestamp:kind == 2 ? now - 60 : now
                  windowNumber:number
                       context:nil
                   eventNumber:1
                    clickCount:1
                      pressure:1];
    }
    bdk_native_handle_t handle;
    memset(&handle, 0, sizeof handle);
    handle.kind = BDK_NATIVE_NSEVENT;
    handle.pointer = (__bridge void*)held;
    return handle;
}

/* Answers the sheet a default presenter attached to the page's window: the button at `index`
   (0 is the first). Returns 0 when there is no sheet. */
int bdk_harness_engine_answer_sheet(bdk_contents_t contents, int index) {
    NSWindow* window = WindowOf(contents);
    NSWindow* sheet = window.attachedSheet;
    if (!sheet)
        return 0;
    [window endSheet:sheet returnCode:NSAlertFirstButtonReturn + index];
    return 1;
}

int bdk_harness_engine_window_is_key(bdk_contents_t contents) {
    /* Silent, the harness is never active, so no window of it has the keyboard. */
    return !Silent() && WindowOf(contents).isKeyWindow ? 1 : 0;
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

/* ---- a loopback server whose download stalls ----
   Serves one file of 200000 bytes: the first 20000, then nothing until released, so a download of
   it stays in progress for as long as a test needs and can be paused, resumed and cancelled. */

static int g_download_listener = -1;
static std::mutex g_download_lock;
static std::condition_variable g_download_wake;
static bool g_download_released = false;
static bool g_download_stopped = false;

static void ServeDownload(int connection) {
    char request[2048];
    (void)read(connection, request, sizeof request);
    const char header[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: 200000\r\nContent-Disposition: attachment; filename=\"big.bin\"\r\n"
        "Connection: close\r\n\r\n";
    std::string chunk(20000, 'a');
    bool ok = write(connection, header, sizeof header - 1) > 0 &&
              write(connection, chunk.data(), chunk.size()) > 0;
    if (ok) {
        std::unique_lock<std::mutex> lock(g_download_lock);
        g_download_wake.wait(lock, [] { return g_download_released || g_download_stopped; });
        if (g_download_released && !g_download_stopped) {
            lock.unlock();
            std::string rest(180000, 'b');
            size_t sent = 0;
            while (sent < rest.size()) {
                ssize_t n = write(connection, rest.data() + sent, rest.size() - sent);
                if (n <= 0)
                    break;
                sent += (size_t)n;
            }
        }
    }
    close(connection);
}

/* Starts the server on a free loopback port; returns the port, or 0. */
int bdk_harness_engine_download_server_start(void) {
    g_download_released = false;
    g_download_stopped = false;
    g_download_listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof address;
    if (g_download_listener < 0 ||
        bind(g_download_listener, (sockaddr*)&address, sizeof address) != 0 ||
        listen(g_download_listener, 8) != 0 ||
        getsockname(g_download_listener, (sockaddr*)&address, &length) != 0)
        return 0;
    int listener = g_download_listener;
    std::thread([listener] {
        for (;;) {
            int connection = accept(listener, nullptr, nullptr);
            if (connection < 0)
                return;
            std::thread(ServeDownload, connection).detach();
        }
    }).detach();
    return ntohs(address.sin_port);
}

/* Lets the stalled responses finish. */
void bdk_harness_engine_download_server_release(void) {
    std::lock_guard<std::mutex> lock(g_download_lock);
    g_download_released = true;
    g_download_wake.notify_all();
}

/* Closes the server and any stalled response. */
void bdk_harness_engine_download_server_stop(void) {
    {
        std::lock_guard<std::mutex> lock(g_download_lock);
        g_download_stopped = true;
        g_download_wake.notify_all();
    }
    if (g_download_listener >= 0) {
        shutdown(g_download_listener, SHUT_RDWR);
        close(g_download_listener);
        g_download_listener = -1;
    }
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
    /* A scenario of the downgrade policy, run alone, writes the data directory Chromium is about
       to open, and makes the starts that must fail before Chromium starts
       (tests_engine_profile_versions.c). */
    for (int i = 1; i + 1 < g_argc; ++i) {
        if (strcmp(g_argv[i], "--run") == 0) {
            bdk_engine_launch_prepare(g_argv[i + 1]);
            bdk_engine_push_launch_prepare(g_argv[i + 1]);
            bdk_engine_google_key_launch_prepare(g_argv[i + 1]);
            bdk_engine_payments_launch_prepare(g_argv[i + 1]);
        }
    }
    /* Starts Chromium with a first runtime. Its nested main loop is queued on the run loop; the
       block after it runs inside that loop. */
    bdk_start_config_t config;
    t_fill_start_config(&config);
    if (t_first_start_google_key)
        config.google_api_key = S(t_first_start_google_key);
    bdk_runtime_t warmup = 0;
    bdk_error_t error = 0;
    if (bdk_runtime_start(&config, &warmup, &error) != BDK_OK) {
        bdk_string_t message = bdk_error_message(error);
        fprintf(stderr, "the engine did not start: %.*s\n", (int)message.length,
                message.data ? message.data : "");
        exit(2);
    }
    t_config_extra = NULL;
    bdk_testing_intercept_all(1);
    /* Frontmost, so its windows become key: Chromium counts focus only in the key window. */
    if (!Silent()) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
    g_warmup = warmup;
    /* The suite runs inside Chromium's main loop, which must be the outer one for the suite's
       nested pumping to let Chromium work, but not inside a Chromium task: it runs where an
       application's own code does, from the run loop between tasks. PartitionAlloc purges what a
       task freed only when the outermost task ends, so a suite run as one task kept everything
       freed while it ran, about 2.4 MB a page (T-REL-03). A task posted through the engine runs
       once Chromium's loop has started; it adds a one-shot timer, which that loop fires after the
       task has ended. */
    bdk_post_to_ui_thread(
        [](void*) {
            CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(
                kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), 0, 0, 0, ^(CFRunLoopTimerRef) {
                  RunSuite(nullptr);
                });
            CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
            CFRelease(timer);
        },
        nullptr);
}

@end

int main(int argc, char** argv) {
    @autoreleasepool {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setenv("BDK_ENGINE_KEEP_CHROMIUM", "1", 1);
        setenv("BDK_ENGINE_MOCK_KEYCHAIN", "1", 1);
        /* A name that is not localhost for the loopback servers of the scenarios: Chromium
           applies no HSTS to *.localhost. */
        setenv("BDK_ENGINE_HOST_RESOLVER_RULES", "MAP hsts.bdk.test 127.0.0.1", 0);
        /* Chromium refuses fullscreen to a page whose window is not key, and offers autofill
           suggestions only to a view that has focus (which on macOS includes being in the key
           window). macOS does not let a process make itself frontmost while another application
           is (cooperative activation), so the harness's window cannot be relied on to be key; the
           scenarios test BDK's requests, not those policies. */
        /* Chromium starts once per process and registers the branded scheme then: the page
           tests' scheme, conf, is named here for the whole run, whatever the first runtime's
           configuration says. */
        /* Chromium offers a page for installation only after the person has engaged with the
           site for a while; the web app scenarios install at once, as Chromium's own tests do. */
        setenv("BDK_ENGINE_SWITCHES",
               "disable-features=SuppressFullscreenFromUnfocusedView,"
               "RWHVMacHasFocusReturnsFalseForNonKeyWindow bdk-internal-scheme=conf "
               "bypass-app-banner-engagement-checks",
               0);
        if (Silent()) {
            /* Behind other windows a page is occluded, and Chromium stops drawing it. */
            std::string switches = getenv("BDK_ENGINE_SWITCHES");
            switches += " disable-backgrounding-occluded-windows";
            setenv("BDK_ENGINE_SWITCHES", switches.c_str(), 1);
        }
        /* Each run has a new data directory, a Chromium profile tree of up to hundreds of
           megabytes: the caller's, or one the harness makes and removes. */
        if (const char* given = getenv("BDK_ENGINE_DATA_DIR"); given && *given) {
            g_data_dir = given;
        } else {
            char dir[] = "/tmp/bdk-engine-conformance-XXXXXX";
            if (!mkdtemp(dir)) {
                perror("mkdtemp");
                return 2;
            }
            g_data_dir = dir;
            atexit(RemoveDataDir);
        }
        g_argc = argc;
        g_argv = argv;
        [BDKHarnessApplication sharedApplication];
        BDKHarnessDelegate* delegate = [[BDKHarnessDelegate alloc] init];
        NSApp.delegate = delegate;
        /* A regular, active application: its windows become key, so pages can take focus and
           enter fullscreen, and Chromium sees them shown and hidden. A silent run is an accessory:
           no Dock icon, no menu bar, never activated. */
        [NSApp setActivationPolicy:Silent() ? NSApplicationActivationPolicyAccessory
                                            : NSApplicationActivationPolicyRegular];
        [NSApp run];
    }
    return 0;
}
