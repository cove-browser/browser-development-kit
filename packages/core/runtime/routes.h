/* Logical routes (architecture.md section 8; T-WUI-02, T-WUI-05, T-WUI-06): the route table read
   from the resolved configuration, the Swift registrations applied on top of it before the freeze,
   what a navigation to a branded or internal URL becomes, the branded form of an internal URL for
   display, native-page dispatch, and the identity of an internal page that asks for the broker
   (T-WUI-04). Platform neutral C++23, like the rest of the core; the stub and the Chromium engine
   both call these, so the rules are in one place and the conformance suite tests them on both.

   Terms. A route is a logical page name (`settings`). The branded URL is `<scheme>://settings/…`,
   with the scheme from `browser.internal_scheme`. The internal URL is what Chromium loads and
   commits: `chrome://bdk-<route>/…` for a bundled page, `chrome://<host>/…` for a built-in one
   (`passwords` is `chrome://password-manager`, `about` is `chrome://version`, and BDK's own
   new-tab page is `chrome://bdk-new-tab`). */
#ifndef BDK_ROUTES_H_
#define BDK_ROUTES_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core_internal.h"

namespace bdkcore {

struct Caller;

/* How a route is served (BDK_PAGE_MODE_*). */
enum class PageMode : uint8_t {
    kChromium = BDK_PAGE_MODE_CHROMIUM,
    kBundled = BDK_PAGE_MODE_BUNDLED,
    kNative = BDK_PAGE_MODE_NATIVE,
    kDisabled = BDK_PAGE_MODE_DISABLED,
};

struct RouteEntry {
    PageMode mode = PageMode::kChromium;
    std::string
        host; /* the internal host this route is served at now; empty for native and disabled */
    std::string path; /* a bundled page: its directory in the bundle's resources */
    std::vector<std::string> capabilities; /* a bundled page's grants */
};

struct NativeHandler {
    bdk_native_page_cb cb = nullptr;
    void* ctx = nullptr;
};

struct RouteTable {
    std::string scheme; /* the branded scheme; empty when the configuration names none */
    /* Routes the configuration or a registration names. A built-in route that is in neither is
       served by Chromium (kChromium); an application route that is in neither does not exist. */
    std::map<std::string, RouteEntry> routes;
    std::map<std::string, std::string> aliases; /* alias -> route; never an alias of an alias */
    /* Bundled pages of the resolved file, kept apart so a registration can point a route back at
       its own bundled page after another registration changed it. */
    std::map<std::string, RouteEntry> bundled;
    std::map<std::string, NativeHandler> handlers;
};

/* Net errors the core decides with (Chromium's numbers, which every backend reports). */
constexpr int32_t kNetErrorBlockedByClient = -20; /* a disabled route */
constexpr int32_t kNetErrorInvalidUrl = -300;     /* a route that does not exist */

/* Reads `browser.internal_scheme`, `routes` and `aliases` from the resolved configuration into
   rt.routes. A malformed table is config.resolvedInvalid. Called by bdk_runtime_start before the
   backend starts. */
bdk_status_t routes_configure(Runtime& rt, const std::string& resolved_config,
                              bdk_error_t* out_error);

/* bdk_page_handler_register after the runtime and thread checks (see bdk_broker.h). */
bdk_status_t routes_register(Runtime& rt, const std::string& route, uint32_t mode,
                             bdk_native_page_cb cb, void* ctx, bdk_error_t* out_error);

/* At the freeze: a native route with no handler is config.handlerMissing. */
bdk_status_t routes_check_handlers(const Runtime& rt, bdk_error_t* out_error);

/* The Chromium host of a route that has a built-in page, or nullptr. */
const char* builtin_host(const std::string& route);

/* What a navigation to a URL becomes. */
struct RouteNavigation {
    enum class Kind : uint8_t {
        kPass,    /* not a branded or internal page URL: the backend handles it as it is */
        kLoad,    /* load `url`, the internal URL; display it as `display` */
        kNative,  /* cancel it and deliver it to the native handler of `route` */
        kBlocked, /* commit the standard error page for the URL with net_error */
        kRefused, /* drop it, with no error page: a web page asked for a privileged URL */
    };
    Kind kind = Kind::kPass;
    std::string url;     /* kLoad: the internal URL */
    std::string display; /* kLoad, kNative, kBlocked: the branded URL the application sees */
    std::string route;   /* kLoad, kNative: the route, after aliases */
    int32_t net_error = 0;
};

/* `trusted` is true for a navigation the application starts and for one an internal page starts;
   false for one a web page, an extension or anything else starts (which is refused for a branded
   URL; Chromium itself refuses them an internal one). An internal URL is loaded only if it is the
   one its route is served at now, so an overridden or disabled built-in page has no other way in.
 */
RouteNavigation resolve_navigation(const Runtime& rt, const std::string& url, bool trusted);

/* Whether a committed URL is one of BDK's internal pages: an internal URL its route serves now. */
bool is_internal_page(const Runtime& rt, const std::string& url);

/* The URL the application sees for a URL a backend reports: an internal URL of a route becomes
   its branded form (`chrome://bdk-settings/a?b` is `cove://settings/a?b`); anything else is
   returned as it is. Every URL a backend reports to the application goes through this. */
std::string display_url(const Runtime& rt, const std::string& url);

/* Delivers a navigation of a native route to its handler, later, on the UI thread. done(true) when
   the handler showed the page itself, done(false) when the standard error page should show; it is
   called once, also at shutdown (false) if the handler never answered. */
void dispatch_native_page(const std::shared_ptr<Runtime>& rt, const RouteNavigation& navigation,
                          const std::shared_ptr<Contents>& source,
                          std::function<void(bool handled)> done);

/* At shutdown: every native page request not answered yet finishes as blocked. */
void routes_shutdown(Runtime& rt);

/* The broker caller of an internal page that asks for the broker interface: a page caller with its
   route's capabilities (an alias's are its target's) in the page's profile, or null when the page
   may not have one; then `why` says why and the backend terminates the renderer. Only a bundled
   page in the primary main frame of its page may. `committed_url` is the frame's last committed
   URL. */
std::shared_ptr<Caller> page_caller_bind(const Runtime& rt, const std::string& committed_url,
                                         bdk_profile_t profile, bool primary_main_frame,
                                         std::string* why);

/* Re-checked on every call: the frame still shows the page the caller was bound for. */
bool page_caller_current(const Runtime& rt, const Caller& caller, const std::string& committed_url);

/* What the bundled data source serves for a route's host: the route's directory in the bundle and
   whether it is BDK's own new-tab page (no directory). False for a host that is not bundled now. */
struct BundledSource {
    std::string route;
    std::string path;
    bool builtin_new_tab = false;
};
bool bundled_source(const Runtime& rt, const std::string& host, BundledSource* out);

} // namespace bdkcore

#endif /* BDK_ROUTES_H_ */
