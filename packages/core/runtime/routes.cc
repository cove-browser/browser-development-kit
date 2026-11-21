/* Logical routes. See routes.h. */
#include "routes.h"

#include <algorithm>
#include <cstring>

#include "broker_internal.h"

namespace bdkcore {
namespace {

/* Routes with a built-in page and the Chromium host that serves each (configuration.md 4.1 and
   4.2). Checked against chrome/common/webui_url_constants.h, content/public/common/url_constants.h
   and components/password_manager/content/common/web_ui_constants.h at 158.0.8127.37. `new-tab` is
   BDK's own minimal page: Chromium's needs Google services. packages/cli lists the same routes
   (src/core/config/routes.ts); a test there fails when the two disagree. */
struct Builtin {
    const char* route;
    const char* host;
};
constexpr Builtin kBuiltins[] = {
    {"new-tab", "bdk-new-tab"},
    {"settings", "settings"},
    {"extensions", "extensions"},
    {"history", "history"},
    {"bookmarks", "bookmarks"},
    {"downloads", "downloads"},
    {"passwords", "password-manager"},
    {"about", "version"},
    {"credits", "credits"},
    {"flags", "flags"},
    /* Diagnostic routes. */
    {"gpu", "gpu"},
    {"net-export", "net-export"},
    {"net-internals", "net-internals"},
    {"sandbox", "sandbox"},
    {"process-internals", "process-internals"},
    {"inspect", "inspect"},
    {"tracing", "tracing"},
    {"crashes", "crashes"},
    {"components", "components"},
    {"media-internals", "media-internals"},
    {"webrtc-internals", "webrtc-internals"},
    {"serviceworker-internals", "serviceworker-internals"},
    {"accessibility", "accessibility"},
    {"histograms", "histograms"},
    {"quota-internals", "quota-internals"},
    {"indexeddb-internals", "indexeddb-internals"},
    {"blob-internals", "blob-internals"},
    {"extensions-internals", "extensions-internals"},
};

/* Names Chromium needs and pages that are never reachable (configuration.md 4.3 and 4.4, as the
   CLI's resolver has them). Names that start `bdk-` are reserved too. */
constexpr const char* kReserved[] = {
    "resources",        "theme",         "favicon", "favicon2",       "print",
    "devtools",         "network-error", "error",   "chrome-error",   "chrome-search",
    "chrome-untrusted", "signin",        "sign-in", "sync-internals", "feedback",
    "whats-new",        "welcome",
};

/* Routes whose page must stay reachable: the licence notices have to ship (T-REL-02). */
constexpr const char* kUndisableable = "credits";

/* Schemes the branded scheme cannot be. */
constexpr const char* kTakenSchemes[] = {
    "about",
    "blob",
    "chrome",
    "chrome-error",
    "chrome-extension",
    "chrome-search",
    "chrome-untrusted",
    "data",
    "devtools",
    "file",
    "filesystem",
    "ftp",
    "http",
    "https",
    "javascript",
    "view-source",
    "ws",
    "wss",
};

constexpr const char* kChromeScheme = "chrome";
constexpr const char* kBundledHostPrefix = "bdk-";

const RouteTable& empty_table() {
    static const RouteTable* t = new RouteTable;
    return *t;
}

const RouteTable& table_of(const Runtime& rt) {
    return rt.routes ? *rt.routes : empty_table();
}

RouteTable& table_of(Runtime& rt) {
    if (!rt.routes)
        rt.routes = std::make_shared<RouteTable>();
    return *rt.routes;
}

bool valid_route_name(const std::string& name) {
    if (name.empty() || name[0] < 'a' || name[0] > 'z')
        return false;
    return std::all_of(name.begin(), name.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
    });
}

bool reserved_route(const std::string& name) {
    if (name.compare(0, std::strlen(kBundledHostPrefix), kBundledHostPrefix) == 0)
        return true;
    return std::any_of(std::begin(kReserved), std::end(kReserved),
                       [&](const char* r) { return name == r; });
}

bool valid_scheme(const std::string& scheme) {
    if (scheme.empty() || scheme[0] < 'a' || scheme[0] > 'z')
        return false;
    if (!std::all_of(scheme.begin(), scheme.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' ||
                   ch == '.';
        }))
        return false;
    return std::none_of(std::begin(kTakenSchemes), std::end(kTakenSchemes),
                        [&](const char* s) { return scheme == s; });
}

const char* route_for_builtin_host(const std::string& host) {
    for (const Builtin& b : kBuiltins) {
        if (host == b.host)
            return b.route;
    }
    return nullptr;
}

/* How a route is served now: its entry, or Chromium's built-in page when it has one and nothing
   names it. Null for a route that does not exist. */
const RouteEntry* effective_entry(const RouteTable& t, const std::string& route,
                                  RouteEntry* scratch) {
    auto it = t.routes.find(route);
    if (it != t.routes.end())
        return &it->second;
    if (const char* host = builtin_host(route)) {
        scratch->mode = PageMode::kChromium;
        scratch->host = host;
        return scratch;
    }
    return nullptr;
}

/* A URL split as BDK needs it: `scheme://host` and the rest (path, query and fragment), or false
   when it is not of that form. The scheme and host are lower-cased. */
struct SplitUrl {
    std::string scheme, host, rest;
};

std::string lower(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

bool split_url(const std::string& url, SplitUrl* out) {
    size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0 || url.compare(colon, 3, "://") != 0)
        return false;
    out->scheme = lower(url.substr(0, colon));
    size_t start = colon + 3;
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos)
        end = url.size();
    out->host = lower(url.substr(start, end - start));
    out->rest = url.substr(end);
    return true;
}

std::string with_path(const std::string& base, const std::string& rest) {
    return base + (rest.empty() || rest[0] != '/' ? "/" + rest : rest);
}

/* The route that serves an internal host now, or empty. */
std::string route_serving(const RouteTable& t, const std::string& host) {
    /* `bdk-<route>` is a bundled page or BDK's new-tab page; any other host is a built-in's. */
    std::string candidate;
    if (host.compare(0, std::strlen(kBundledHostPrefix), kBundledHostPrefix) == 0) {
        candidate = host.substr(std::strlen(kBundledHostPrefix));
    } else if (const char* r = route_for_builtin_host(host)) {
        candidate = r;
    }
    if (candidate.empty())
        return std::string();
    RouteEntry scratch;
    const RouteEntry* e = effective_entry(t, candidate, &scratch);
    if (!e || (e->mode != PageMode::kChromium && e->mode != PageMode::kBundled) || e->host != host)
        return std::string();
    return candidate;
}

/* The part of `rest` that is the path, and the query without its `?`. */
void path_and_query(const std::string& rest, std::string* path, std::string* query) {
    size_t q = rest.find('?');
    size_t f = rest.find('#');
    size_t path_end = std::min(q, f);
    *path = rest.substr(0, path_end == std::string::npos ? rest.size() : path_end);
    if (path->empty())
        *path = "/";
    query->clear();
    if (q != std::string::npos && (f == std::string::npos || q < f))
        *query = rest.substr(q + 1, (f == std::string::npos ? rest.size() : f) - q - 1);
}

const JsonValue* member(const JsonValue& object, const char* key) {
    return object.type == JsonValue::Type::kObject ? object.find(key) : nullptr;
}

bdk_status_t invalid(bdk_error_t* out_error, const std::string& message) {
    return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID, message);
}

bdk_status_t read_route(const std::string& route, const JsonValue& value, RouteEntry* out,
                        bdk_error_t* out_error) {
    const std::string where = "routes." + route;
    if (!valid_route_name(route) || reserved_route(route))
        return invalid(out_error, "\"" + route + "\" in the resolved routes is not a route name");
    const JsonValue* mode = member(value, "mode");
    if (!mode || mode->type != JsonValue::Type::kString)
        return invalid(out_error, where + " has no mode");
    if (mode->text == "chromium") {
        const char* host = builtin_host(route);
        if (!host)
            return invalid(out_error, where + " is \"chromium\" but has no built-in page");
        out->mode = PageMode::kChromium;
        out->host = host;
    } else if (mode->text == "native") {
        out->mode = PageMode::kNative;
    } else if (mode->text == "disabled") {
        if (route == kUndisableable)
            return invalid(out_error, where + " cannot be disabled");
        out->mode = PageMode::kDisabled;
    } else if (mode->text == "bundled") {
        const JsonValue* host = member(value, "host");
        const JsonValue* path = member(value, "path");
        const JsonValue* caps = member(value, "capabilities");
        if (!host || host->type != JsonValue::Type::kString ||
            host->text != kBundledHostPrefix + route)
            return invalid(out_error, where + ".host must be \"bdk-" + route + "\"");
        if (!path || path->type != JsonValue::Type::kString || path->text.empty() ||
            path->text[0] == '/' || path->text.find("..") != std::string::npos)
            return invalid(out_error, where + ".path must be a path inside the bundle");
        out->mode = PageMode::kBundled;
        out->host = host->text;
        out->path = path->text;
        if (caps) {
            if (caps->type != JsonValue::Type::kArray)
                return invalid(out_error, where + ".capabilities is not an array");
            for (const JsonValue& c : caps->items) {
                if (c.type != JsonValue::Type::kString)
                    return invalid(out_error,
                                   where + ".capabilities holds a value that is not a name");
                out->capabilities.push_back(c.text);
            }
        }
        std::string refused = grant_error(CallerKind::kPage, out->capabilities);
        if (!refused.empty())
            return invalid(out_error, where + ": " + refused);
    } else {
        return invalid(out_error, where + ".mode \"" + mode->text + "\" is not a page mode");
    }
    return BDK_OK;
}

} // namespace

const char* builtin_host(const std::string& route) {
    for (const Builtin& b : kBuiltins) {
        if (route == b.route)
            return b.host;
    }
    return nullptr;
}

bdk_status_t routes_configure(Runtime& rt, const std::string& resolved, bdk_error_t* out_error) {
    RouteTable t;
    JsonValue root;
    if (!json_parse(resolved, &root) || root.type != JsonValue::Type::kObject)
        return invalid(out_error, "the resolved configuration is not a JSON object");

    if (const JsonValue* browser = member(root, "browser")) {
        if (const JsonValue* scheme = member(*browser, "internal_scheme")) {
            if (scheme->type != JsonValue::Type::kString || !valid_scheme(scheme->text))
                return invalid(out_error, "browser.internal_scheme is not a scheme BDK can use");
            t.scheme = scheme->text;
        }
    }
    if (const JsonValue* routes = member(root, "routes")) {
        if (routes->type != JsonValue::Type::kObject)
            return invalid(out_error, "\"routes\" in the resolved configuration is not an object");
        for (const JsonMember& m : routes->members) {
            RouteEntry entry;
            if (bdk_status_t s = read_route(m.first, m.second, &entry, out_error))
                return s;
            if (entry.mode == PageMode::kBundled)
                t.bundled[m.first] = entry;
            t.routes[m.first] = std::move(entry);
        }
    }
    if (const JsonValue* aliases = member(root, "aliases")) {
        if (aliases->type != JsonValue::Type::kObject)
            return invalid(out_error, "\"aliases\" in the resolved configuration is not an object");
        for (const JsonMember& m : aliases->members) {
            const std::string& alias = m.first;
            if (!valid_route_name(alias) || reserved_route(alias) || t.routes.count(alias) ||
                builtin_host(alias))
                return invalid(out_error, "the alias \"" + alias + "\" is not a free route name");
            if (m.second.type != JsonValue::Type::kString)
                return invalid(out_error, "aliases." + alias + " is not a route name");
            const std::string& target = m.second.text;
            if (aliases->find(target))
                return invalid(out_error, "aliases." + alias + " points at another alias");
            if (!t.routes.count(target) && !builtin_host(target))
                return invalid(out_error, "aliases." + alias + " points at no route");
            t.aliases[alias] = target;
        }
    }
    rt.routes = std::make_shared<RouteTable>(std::move(t));
    return BDK_OK;
}

bdk_status_t routes_register(Runtime& rt, const std::string& route, uint32_t mode,
                             bdk_native_page_cb cb, void* ctx, bdk_error_t* out_error) {
    RouteTable& t = table_of(rt);
    if (!valid_route_name(route) || reserved_route(route) || t.aliases.count(route))
        return fail(
            out_error, BDK_DOMAIN_CONFIG, "routeInvalid",
            "\"" + route + "\" is not a route that can be registered" +
                (t.aliases.count(route) ? ": it is an alias, which follows its target" : ""));
    RouteEntry entry;
    switch (mode) {
    case BDK_PAGE_MODE_CHROMIUM: {
        const char* host = builtin_host(route);
        if (!host)
            return fail(out_error, BDK_DOMAIN_CONFIG, "noBuiltin",
                        "the route \"" + route + "\" has no built-in page");
        entry.mode = PageMode::kChromium;
        entry.host = host;
        break;
    }
    case BDK_PAGE_MODE_BUNDLED: {
        auto it = t.bundled.find(route);
        if (it == t.bundled.end())
            return fail(out_error, BDK_DOMAIN_CONFIG, "routeUnknown",
                        "the route \"" + route +
                            "\" is not a bundled page in the resolved configuration");
        entry = it->second;
        break;
    }
    case BDK_PAGE_MODE_NATIVE:
        if (!cb)
            return BDK_ERR_INVALID_ARGUMENT;
        entry.mode = PageMode::kNative;
        break;
    case BDK_PAGE_MODE_DISABLED:
        if (route == kUndisableable)
            return fail(out_error, BDK_DOMAIN_CONFIG, "routeMode",
                        "the route \"" + route + "\" cannot be disabled");
        entry.mode = PageMode::kDisabled;
        break;
    default:
        return BDK_ERR_INVALID_ARGUMENT;
    }
    t.routes[route] = std::move(entry);
    if (mode == BDK_PAGE_MODE_NATIVE)
        t.handlers[route] = NativeHandler{cb, ctx};
    else
        t.handlers.erase(route);
    return BDK_OK;
}

bdk_status_t routes_check_handlers(const Runtime& rt, bdk_error_t* out_error) {
    const RouteTable& t = table_of(rt);
    std::string missing;
    for (const auto& [route, entry] : t.routes) {
        if (entry.mode == PageMode::kNative && !t.handlers.count(route))
            missing += (missing.empty() ? "" : ", ") + route;
    }
    if (missing.empty())
        return BDK_OK;
    return fail(out_error, BDK_DOMAIN_CONFIG, "handlerMissing",
                "native routes need a handler before the freeze: " + missing);
}

RouteNavigation resolve_navigation(const Runtime& rt, const std::string& url, bool trusted) {
    const RouteTable& t = table_of(rt);
    RouteNavigation nav;
    SplitUrl u;
    if (!split_url(url, &u))
        return nav;
    if (!t.scheme.empty() && u.scheme == t.scheme) {
        if (!trusted) {
            nav.kind = RouteNavigation::Kind::kRefused;
            return nav;
        }
        nav.display = url;
        auto alias = t.aliases.find(u.host);
        std::string route = alias != t.aliases.end() ? alias->second : u.host;
        RouteEntry scratch;
        const RouteEntry* e =
            valid_route_name(route) ? effective_entry(t, route, &scratch) : nullptr;
        if (!e) {
            nav.kind = RouteNavigation::Kind::kBlocked;
            nav.net_error = kNetErrorInvalidUrl;
            return nav;
        }
        nav.route = route;
        switch (e->mode) {
        case PageMode::kDisabled:
            nav.kind = RouteNavigation::Kind::kBlocked;
            nav.net_error = kNetErrorBlockedByClient;
            break;
        case PageMode::kNative:
            nav.kind = RouteNavigation::Kind::kNative;
            break;
        case PageMode::kChromium:
        case PageMode::kBundled:
            nav.kind = RouteNavigation::Kind::kLoad;
            nav.url = with_path(std::string(kChromeScheme) + "://" + e->host, u.rest);
            nav.display = with_path(t.scheme + "://" + route, u.rest);
            break;
        }
        return nav;
    }
    if (u.scheme != kChromeScheme)
        return nav;
    std::string route = route_serving(t, u.host);
    if (route.empty()) {
        /* An overridden or disabled built-in page, a page that is not a route, or an excluded one:
           no way in by its internal URL either. */
        nav.kind = RouteNavigation::Kind::kBlocked;
        nav.net_error = kNetErrorBlockedByClient;
        nav.display = url;
        return nav;
    }
    nav.kind = RouteNavigation::Kind::kLoad;
    nav.url = url;
    nav.route = route;
    nav.display = display_url(rt, url);
    return nav;
}

bool is_internal_page(const Runtime& rt, const std::string& url) {
    SplitUrl u;
    return split_url(url, &u) && u.scheme == kChromeScheme &&
           !route_serving(table_of(rt), u.host).empty();
}

std::string display_url(const Runtime& rt, const std::string& url) {
    const RouteTable& t = table_of(rt);
    SplitUrl u;
    if (t.scheme.empty() || !split_url(url, &u) || u.scheme != kChromeScheme)
        return url;
    std::string route = route_serving(t, u.host);
    if (route.empty())
        return url;
    return with_path(t.scheme + "://" + route, u.rest);
}

void dispatch_native_page(const std::shared_ptr<Runtime>& rt, const RouteNavigation& nav,
                          const std::shared_ptr<Contents>& source,
                          std::function<void(bool handled)> done) {
    const RouteTable& t = table_of(*rt);
    auto handler = t.handlers.find(nav.route);
    if (handler == t.handlers.end()) {
        post([done] { done(false); });
        return;
    }
    SplitUrl u;
    std::string path = "/", query;
    if (split_url(nav.display, &u))
        path_and_query(u.rest, &path, &query);
    std::string request = "{\"route\":\"" + json_escape(nav.route) + "\",\"url\":\"" +
                          json_escape(nav.display) + "\",\"path\":\"" + json_escape(path) +
                          "\",\"query\":\"" + json_escape(query) + "\"}";

    auto cc = std::make_shared<CallCtx>();
    cc->call = std::make_shared<Call>(); /* answered through native_done, never as a call */
    cc->call->done = true;
    if (source) {
        cc->profile = source->profile;
        cc->source = source->handle;
    }
    auto once = std::make_shared<std::function<void(bool)>>(std::move(done));
    cc->native_done = [once](uint32_t result) {
        if (*once) {
            auto d = std::move(*once);
            *once = nullptr;
            d(result == BDK_NATIVE_PAGE_HANDLED);
        }
    };
    bdk_context_t handle = table_make(T_CONTEXT, cc);
    rt->native_contexts.push_back(cc);
    NativeHandler h = handler->second;
    post([rt, h, handle, request] {
        if (rt->shutting || table_type(handle) != T_CONTEXT)
            return;
        h.cb(h.ctx, handle, view(request));
    });
}

void routes_shutdown(Runtime& rt) {
    auto pending = std::move(rt.native_contexts);
    rt.native_contexts.clear();
    for (auto& w : pending) {
        if (auto cc = w.lock()) {
            auto finish = std::move(cc->native_done);
            cc->native_done = nullptr;
            if (finish)
                post([finish] { finish(BDK_NATIVE_PAGE_BLOCKED); });
        }
    }
}

std::shared_ptr<Caller> page_caller_bind(const Runtime& rt, const std::string& committed_url,
                                         bdk_profile_t profile, bool primary_main_frame,
                                         std::string* why) {
    const RouteTable& t = table_of(rt);
    if (!primary_main_frame) {
        *why = "the broker is bound only to the primary main frame of an internal page";
        return nullptr;
    }
    SplitUrl u;
    if (!split_url(committed_url, &u) || u.scheme != kChromeScheme ||
        u.host.compare(0, std::strlen(kBundledHostPrefix), kBundledHostPrefix) != 0) {
        *why = "the frame is not a bundled internal page";
        return nullptr;
    }
    std::string route = u.host.substr(std::strlen(kBundledHostPrefix));
    auto it = t.routes.find(route);
    if (it == t.routes.end() || it->second.mode != PageMode::kBundled ||
        it->second.host != u.host) {
        *why = "\"" + u.host + "\" is not a bundled page in the route table";
        return nullptr;
    }
    auto caller = std::make_shared<Caller>();
    caller->kind = CallerKind::kPage;
    caller->name = route;
    caller->profile = profile;
    caller->capabilities = it->second.capabilities;
    caller->internal_scheme = t.scheme;
    return caller;
}

bool page_caller_current(const Runtime& rt, const Caller& caller,
                         const std::string& committed_url) {
    if (caller.kind != CallerKind::kPage || caller.revoked)
        return false;
    SplitUrl u;
    if (!split_url(committed_url, &u) || u.scheme != kChromeScheme ||
        u.host != kBundledHostPrefix + caller.name)
        return false;
    const RouteTable& t = table_of(rt);
    auto it = t.routes.find(caller.name);
    return it != t.routes.end() && it->second.mode == PageMode::kBundled;
}

bool bundled_source(const Runtime& rt, const std::string& host, BundledSource* out) {
    const RouteTable& t = table_of(rt);
    std::string route = route_serving(t, lower(host));
    if (route.empty())
        return false;
    RouteEntry scratch;
    const RouteEntry* e = effective_entry(t, route, &scratch);
    if (e->mode == PageMode::kBundled) {
        out->route = route;
        out->path = e->path;
        out->builtin_new_tab = false;
        return true;
    }
    if (route == "new-tab") {
        out->route = route;
        out->path.clear();
        out->builtin_new_tab = true;
        return true;
    }
    return false;
}

} // namespace bdkcore
