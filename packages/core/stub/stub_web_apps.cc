/* The stub's installable web apps (web-apps.md, T-PWA-03): a registry per profile id, in memory,
   behind the webApps.* commands, and the test hooks that act as a page (bdk_stub.h). A page's
   manifest is one a test registered for the page's URL (bdk_stub_web_app_manifest); nothing is
   fetched, and relative URLs resolve against that URL. The core has already refused what is
   refused everywhere (web apps off, a private profile: runtime/web_apps.cc). It is the reference
   for result shapes and refusals, not for Chromium's installability rules, which are more. */
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "bdk_stub.h"
#include "stub_services.h"

using namespace bdkcore;

namespace bdkstub {
namespace {

constexpr int64_t kEpoch = 1700000000000; /* stub_services.cc's, so timestamps read alike */

struct App {
    std::string id, name, short_name, start_url, scope, origin, display;
    std::string theme_color, background_color;
    std::string icons, screenshots, shortcuts; /* JSON arrays */
    std::vector<std::string> shortcut_urls;
    std::string badge = "null";
    int64_t installed_at = 0;
};

/* A page launched as an app: which app, and whether its last committed URL was in its scope. */
struct AppPage {
    std::string app_id;
    std::string scope;
    bool in_scope = true;
};

struct WebAppState {
    std::map<std::string, std::vector<App>> profiles; /* by profile id, in install order */
    std::map<std::string, std::string> manifests;     /* document URL -> manifest JSON */
    std::map<bdk_contents_t, AppPage> pages;
    int64_t clock = kEpoch;
};

WebAppState& ws() {
    static WebAppState* s = new WebAppState;
    return *s;
}

/* ---- URLs ---------------------------------------------------------------------------------- */

std::string origin_of(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return std::string();
    const size_t path = url.find('/', scheme + 3);
    return path == std::string::npos ? url : url.substr(0, path);
}

/* The URL without query and fragment, cut after its last slash. */
std::string directory_of(const std::string& url) {
    std::string base = url.substr(0, url.find_first_of("?#"));
    const size_t origin = origin_of(base).size();
    const size_t slash = base.rfind('/');
    return slash == std::string::npos || slash < origin ? base + "/" : base.substr(0, slash + 1);
}

std::string resolve(const std::string& base, const std::string& ref) {
    if (ref.find("://") != std::string::npos)
        return ref;
    if (!ref.empty() && ref[0] == '/')
        return origin_of(base) + ref;
    return directory_of(base) + ref;
}

bool in_scope(const std::string& url, const std::string& scope) {
    return url.compare(0, scope.size(), scope) == 0;
}

/* ---- Manifests ----------------------------------------------------------------------------- */

const JsonValue* member(const JsonValue& o, const char* key) {
    return o.type == JsonValue::Type::kObject ? o.find(key) : nullptr;
}

std::string text_of(const JsonValue& o, const char* key) {
    const JsonValue* v = member(o, key);
    return v && v->type == JsonValue::Type::kString ? v->text : std::string();
}

std::string quoted(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

std::string quoted_or_null(const std::string& s) {
    return s.empty() ? "null" : quoted(s);
}

/* An icon list in the WebAppIcon shape. sizes "192x192 512x512" gives the first; purpose the
   first of Chromium's three it names. */
std::string icons_json(const JsonValue& m, const char* key, const std::string& base) {
    std::string out = "[";
    const JsonValue* list = member(m, key);
    if (list && list->type == JsonValue::Type::kArray) {
        for (const JsonValue& icon : list->items) {
            const std::string src = text_of(icon, "src");
            if (src.empty())
                continue;
            std::string width = "null", height = "null";
            const std::string sizes = text_of(icon, "sizes");
            const size_t x = sizes.find('x');
            if (x != std::string::npos && x > 0) {
                width = sizes.substr(0, x);
                height = sizes.substr(x + 1, sizes.find(' ', x) - x - 1);
                const auto digits = [](const std::string& s) {
                    return !s.empty() && std::all_of(s.begin(), s.end(),
                                                     [](char c) { return c >= '0' && c <= '9'; });
                };
                if (!digits(width) || !digits(height))
                    width = height = "null";
            }
            std::string purpose = "any";
            const std::string declared = text_of(icon, "purpose");
            for (const char* p : {"any", "monochrome", "maskable"}) {
                if (declared.find(p) != std::string::npos) {
                    purpose = p;
                    break;
                }
            }
            if (out.size() > 1)
                out += ',';
            out += "{\"url\":";
            out += quoted(resolve(base, src));
            out += ",\"width\":" + width;
            out += ",\"height\":" + height;
            out += ",\"purpose\":\"" + purpose + "\",\"data\":null}";
        }
    }
    return out + "]";
}

/* Reads the manifest registered for document_url. Empty errors: installable. */
App read_manifest(const std::string& document_url, std::vector<std::string>* errors) {
    App a;
    auto it = ws().manifests.find(document_url);
    JsonValue m;
    bool object = false;
    if (it == ws().manifests.end() || !valid_json(it->second, &object) || !object ||
        !json_parse(it->second, &m)) {
        errors->push_back("the page has no manifest");
        return a;
    }
    a.name = text_of(m, "name");
    a.short_name = text_of(m, "short_name");
    if (a.name.empty())
        a.name = a.short_name;
    if (a.name.empty())
        errors->push_back("the manifest has no name or short_name");
    const std::string start = text_of(m, "start_url");
    a.start_url = start.empty() ? document_url : resolve(document_url, start);
    const std::string scope = text_of(m, "scope");
    a.scope = scope.empty() ? directory_of(a.start_url) : resolve(document_url, scope);
    a.origin = origin_of(a.start_url);
    if (a.origin.empty() || a.origin != origin_of(document_url))
        errors->push_back("the start URL is not on the page's origin");
    if (!in_scope(a.start_url, a.scope))
        errors->push_back("the start URL is not inside the scope");
    const std::string manifest_id = text_of(m, "id");
    a.id = make_web_app_id(manifest_id.empty() ? a.start_url.substr(0, a.start_url.find('#'))
                                               : resolve(a.start_url, manifest_id));
    const std::string display = text_of(m, "display");
    if (display == "minimal-ui")
        a.display = "minimalUi";
    else if (display == "standalone" || display == "fullscreen" ||
             display == "window-controls-overlay" || display == "tabbed" || display == "borderless")
        a.display = "standalone"; /* the modes BDK does not offer fall back (web-apps.md 6) */
    else
        errors->push_back("the manifest's display is browser, so it is not an app");
    a.theme_color = text_of(m, "theme_color");
    a.background_color = text_of(m, "background_color");
    a.icons = icons_json(m, "icons", document_url);
    if (a.icons == "[]")
        errors->push_back("the manifest has no icon");
    a.screenshots = icons_json(m, "screenshots", document_url);
    a.shortcuts = "[";
    if (const JsonValue* list = member(m, "shortcuts");
        list && list->type == JsonValue::Type::kArray) {
        for (const JsonValue& s : list->items) {
            const std::string name = text_of(s, "name"), url = text_of(s, "url");
            if (name.empty() || url.empty())
                continue;
            const std::string absolute = resolve(document_url, url);
            a.shortcut_urls.push_back(absolute);
            a.shortcuts += (a.shortcuts.size() > 1 ? "," : "") + std::string("{\"name\":") +
                           quoted(name) + ",\"url\":" + quoted(absolute) +
                           ",\"icons\":" + icons_json(s, "icons", document_url) + "}";
        }
    }
    a.shortcuts += ']';
    return a;
}

std::string info_json(const App& a, const std::string& profile_id) {
    return "{\"id\":" + quoted(a.id) + ",\"profile\":" + quoted(profile_id) +
           ",\"name\":" + quoted(a.name) + ",\"shortName\":" + quoted_or_null(a.short_name) +
           ",\"startUrl\":" + quoted(a.start_url) + ",\"scope\":" + quoted(a.scope) +
           ",\"origin\":" + quoted(a.origin) + ",\"displayMode\":\"" + a.display +
           "\",\"themeColor\":" + quoted_or_null(a.theme_color) +
           ",\"backgroundColor\":" + quoted_or_null(a.background_color) + ",\"icons\":" + a.icons +
           ",\"shortcuts\":" + a.shortcuts + ",\"badge\":" + a.badge +
           ",\"installedAt\":" + std::to_string(a.installed_at) + ",\"hasPendingUpdate\":false}";
}

std::string joined(const std::vector<std::string>& errors) {
    std::string out;
    for (const std::string& e : errors)
        out += (out.empty() ? "" : "; ") + e;
    return out;
}

std::string strings_json(const std::vector<std::string>& items) {
    std::string out = "[";
    for (const std::string& s : items)
        out += (out.size() > 1 ? "," : "") + quoted(s);
    return out + "]";
}

App* find(std::vector<App>& list, const std::string& id) {
    for (App& a : list) {
        if (a.id == id)
            return &a;
    }
    return nullptr;
}

/* Installs into the profile, replacing an earlier install of the same app. */
std::string install_into(const std::shared_ptr<Runtime>& rt, const Profile& profile, App app) {
    std::vector<App>& list = ws().profiles[profile.id];
    ws().clock += 1000;
    app.installed_at = ws().clock;
    const bool replacing = find(list, app.id) != nullptr;
    if (replacing)
        *find(list, app.id) = app;
    else
        list.push_back(app);
    const std::string info = info_json(app, profile.id);
    emit_builtin_event(rt, replacing ? "webApps.updated" : "webApps.installed", profile.handle,
                       "{\"app\":" + info + "}");
    return info;
}

/* Raises a web_app_installs request; done(true) only for the granting completion. A default with
   no handler refuses: the stub has no presenter that can ask a person. */
std::shared_ptr<Request> ask(const std::shared_ptr<Runtime>& rt, const Profile& profile,
                             bdk_contents_t page, const App& app, const char* source,
                             std::function<void(bool)> done) {
    auto r = std::make_shared<Request>();
    r->surface = BDK_SURFACE_WEB_APP_INSTALLS;
    r->contents = page;
    r->profile = profile.handle;
    r->payload = "{\"app\":" + info_json(app, profile.id) + ",\"source\":\"" + source +
                 "\",\"screenshots\":" + app.screenshots + "}";
    r->on_finish = [done = std::move(done)](UiOutcome outcome, const std::string& completion,
                                            const std::string&) {
        done(outcome == UiOutcome::kCompleted &&
             (completion == "grant" || completion == "install"));
    };
    track_request(r);
    raise_ui_request(rt, r);
    return r;
}

void reject(const BuiltinCall& call, const char* code, const std::string& message) {
    call.control->Reject(BDK_DOMAIN_WEB_APPS, code, message);
}

void invalid(const BuiltinCall& call, const std::string& message) {
    call.control->Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS, message);
}

void install(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<BuiltinCall>& call,
             const std::string& url) {
    std::string document = url;
    if (document.empty()) {
        if (!call->contents)
            return invalid(*call, "webApps.install needs a url or a page");
        document = page_committed_url(*call->contents);
    }
    std::vector<std::string> errors;
    App app = read_manifest(document, &errors);
    if (!errors.empty())
        return reject(*call, BDK_CODE_NOT_INSTALLABLE,
                      document + " is not installable: " + joined(errors));
    std::shared_ptr<Profile> profile = call->profile;
    if (call->confirmed) {
        call->control->Resolve(install_into(rt, *profile, app));
        return;
    }
    auto request =
        ask(rt, *profile, call->contents ? call->contents->handle : 0, app, "command",
            [rt, call, profile, app](bool granted) {
                if (!granted)
                    return reject(*call, BDK_CODE_CANCELLED, "the install was not confirmed");
                call->control->Resolve(install_into(rt, *profile, app));
            });
    std::weak_ptr<Request> weak = request;
    /* Later: the caller learns broker.cancelled first, then the prompt goes away. */
    call->control->OnCancel([weak] {
        post([weak] {
            if (auto r = weak.lock())
                withdraw_ui_request(*r);
        });
    });
}

void launch(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<BuiltinCall>& call,
            const App& app, const std::string& shortcut) {
    if (!shortcut.empty() && std::find(app.shortcut_urls.begin(), app.shortcut_urls.end(),
                                       shortcut) == app.shortcut_urls.end())
        return invalid(*call, "shortcutUrl is not one of the app's shortcuts");
    const std::string url = shortcut.empty() ? app.start_url : shortcut;
    bdk_contents_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = call->profile->handle;
    o.initial_url = bdk_string_t{url.data(), url.size()};
    bdk_contents_t page = 0;
    bdk_error_t error = 0;
    if (bdk_contents_create(rt->handle, &o, &page, &error) != BDK_OK) {
        if (error)
            bdk_release(error);
        return reject(*call, BDK_CODE_NOT_FOUND, "the app's page could not be created");
    }
    ws().pages[page] = AppPage{app.id, app.scope, true};
    auto r = std::make_shared<Request>();
    r->surface = BDK_SURFACE_NEW_WINDOWS;
    r->profile = call->profile->handle;
    r->payload = "{\"kind\":\"webApp\",\"disposition\":\"newWindow\",\"targetURL\":" + quoted(url) +
                 ",\"features\":{},\"userGesture\":false,\"newContents\":\"" +
                 std::to_string(page) + "\",\"webApp\":" + info_json(app, call->profile->id) + "}";
    /* A rejected launch destroys the page, as for every new-window request (ui-requests.md 5.6). */
    r->on_finish = [page](UiOutcome outcome, const std::string& completion, const std::string&) {
        const bool kept = outcome == UiOutcome::kDefaulted ||
                          (outcome == UiOutcome::kCompleted && completion == "accept");
        if (!kept) {
            ws().pages.erase(page);
            bdk_contents_destroy(page);
        }
    };
    track_request(r);
    raise_ui_request(rt, r);
    call->control->Resolve("{\"contents\":\"" + std::to_string(page) + "\"}");
}

} // namespace

std::string make_web_app_id(const std::string& manifest_id) {
    /* 32 letters a..p, the shape of Chromium's app identifiers (a hash of the manifest id); two
       FNV-1a passes stand in for its SHA-256. */
    uint64_t a = 1469598103934665603ull, b = 0x84222325cbf29ce4ull;
    for (unsigned char c : manifest_id) {
        a = (a ^ c) * 1099511628211ull;
        b = (b ^ c) * 0x100000001b3ull + 0x7f4a7c15u;
    }
    std::string id;
    for (unsigned i = 0; i < 16; ++i)
        id += static_cast<char>('a' + ((a >> (i * 4u)) & 0xfu));
    for (unsigned i = 0; i < 16; ++i)
        id += static_cast<char>('a' + ((b >> (i * 4u)) & 0xfu));
    return id;
}

void web_apps_reset() {
    ws().profiles.clear();
    ws().manifests.clear();
    ws().pages.clear();
    ws().clock = kEpoch;
}

bool web_apps_execute(Runtime& runtime, const std::shared_ptr<BuiltinCall>& call) {
    if (call->name.rfind("webApps.", 0) != 0)
        return false;
    (void)runtime;
    std::shared_ptr<Runtime> rt = current_runtime();
    if (!rt || !call->profile)
        return false;
    JsonValue args;
    json_parse(call->arguments, &args);
    const std::string& name = call->name;
    const std::string id = text_of(args, "id");
    std::vector<App>& list = ws().profiles[call->profile->id];

    if (name == "webApps.list") {
        std::string out = "[";
        for (const App& a : list)
            out += (out.size() > 1 ? "," : "") + info_json(a, call->profile->id);
        call->control->Resolve(out + "]");
    } else if (name == "webApps.get") {
        if (App* a = find(list, id))
            call->control->Resolve(info_json(*a, call->profile->id));
        else
            reject(*call, BDK_CODE_NOT_FOUND, "no web app " + id + " is installed in the profile");
    } else if (name == "webApps.installability") {
        if (!call->contents)
            return invalid(*call, "webApps.installability needs a page"), true;
        std::vector<std::string> errors;
        App app = read_manifest(page_committed_url(*call->contents), &errors);
        const bool installable = errors.empty();
        const App* installed = installable ? find(list, app.id) : nullptr;
        call->control->Resolve(
            std::string("{\"isInstallable\":") + (installable ? "true" : "false") +
            ",\"installedId\":" + (installed ? quoted(installed->id) : "null") +
            ",\"manifest\":" + (installable ? info_json(app, call->profile->id) : "null") +
            ",\"errors\":" + strings_json(errors) + "}");
    } else if (name == "webApps.install") {
        install(rt, call, text_of(args, "url"));
    } else if (name == "webApps.uninstall") {
        if (!find(list, id))
            return reject(*call, BDK_CODE_NOT_FOUND, "no web app " + id + " is installed"), true;
        list.erase(
            std::remove_if(list.begin(), list.end(), [&id](const App& a) { return a.id == id; }),
            list.end());
        emit_builtin_event(rt, "webApps.uninstalled", call->profile->handle,
                           "{\"id\":" + quoted(id) + "}");
        call->control->Resolve("null");
    } else if (name == "webApps.launch") {
        App* a = find(list, id);
        if (!a)
            return reject(*call, BDK_CODE_NOT_FOUND, "no web app " + id + " is installed"), true;
        launch(rt, call, *a, text_of(args, "shortcutUrl"));
    } else {
        return false;
    }
    return true;
}

void web_apps_committed(const std::shared_ptr<Contents>& c, const std::string& url) {
    auto it = ws().pages.find(c->handle);
    if (it == ws().pages.end())
        return;
    const bool now = in_scope(url, it->second.scope);
    if (now == it->second.in_scope)
        return;
    it->second.in_scope = now;
    emit_contents_event(c, BDK_CONTENTS_EVENT_WEB_APP, url,
                        "{\"appId\":" + quoted(it->second.app_id) +
                            ",\"isInScope\":" + (now ? "true" : "false") + "}",
                        0, BDK_EVENT_FLAG_MAIN_FRAME);
}

} // namespace bdkstub

using namespace bdkstub;

namespace {

/* The page a hook acts as, in a runtime with web apps on. */
std::shared_ptr<Contents> app_page(bdk_contents_t h, std::shared_ptr<Runtime>* rt,
                                   bdk_status_t* status, bdk_error_t* err) {
    *status = check_thread();
    if (*status != BDK_OK)
        return nullptr;
    auto c = contents_of(h);
    *rt = current_runtime();
    if (!c || !*rt || !c->profile) {
        *status = BDK_ERR_STALE_HANDLE;
        return nullptr;
    }
    if (!(*rt)->web_apps.enabled) {
        *status = fail(err, BDK_DOMAIN_WEB_APPS, BDK_CODE_DISABLED,
                       "web apps are off: the page receives no beforeinstallprompt");
        return nullptr;
    }
    if (c->profile->ephemeral) {
        *status = fail(err, BDK_DOMAIN_WEB_APPS, BDK_CODE_PRIVATE_PROFILE,
                       "a page in a private profile receives no beforeinstallprompt");
        return nullptr;
    }
    return c;
}

} // namespace

BDK_EXPORT bdk_status_t bdk_stub_web_app_manifest(bdk_runtime_t h, bdk_string_t document_url,
                                                  bdk_string_t manifest) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(document_url) || !string_ok(manifest) || document_url.length == 0)
        return BDK_ERR_INVALID_ARGUMENT;
    ws().manifests[str(document_url)] = str(manifest);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_web_app_prompt(bdk_contents_t h, uint8_t user_gesture,
                                                bdk_error_t* err) {
    clear_out(err);
    bdk_status_t status;
    std::shared_ptr<Runtime> rt;
    auto c = app_page(h, &rt, &status, err);
    if (!c)
        return status;
    if (!user_gesture)
        return fail(err, BDK_DOMAIN_WEB_APPS, BDK_CODE_GESTURE_REQUIRED,
                    "prompt() needs a user gesture in the page");
    std::vector<std::string> errors;
    App app = read_manifest(page_committed_url(*c), &errors);
    if (!errors.empty())
        return fail(err, BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_INSTALLABLE, joined(errors));
    std::shared_ptr<Profile> profile = c->profile;
    ask(rt, *profile, c->handle, app, "page", [rt, profile, app](bool granted) {
        if (granted)
            install_into(rt, *profile, app);
    });
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_web_app_set_badge(bdk_contents_t h, bdk_string_t badge,
                                                   bdk_error_t* err) {
    clear_out(err);
    bdk_status_t status;
    std::shared_ptr<Runtime> rt;
    auto c = app_page(h, &rt, &status, err);
    if (!c)
        return status;
    if (!string_ok(badge))
        return BDK_ERR_INVALID_ARGUMENT;
    std::string value = str(badge);
    bool object = false;
    if (value.empty())
        value = "null";
    else if (value != "null" && (!valid_json(value, &object) || !object))
        return BDK_ERR_INVALID_ARGUMENT;
    const std::string url = page_committed_url(*c);
    for (App& a : ws().profiles[c->profile->id]) {
        if (!in_scope(url, a.scope))
            continue;
        a.badge = value;
        emit_builtin_event(rt, "webApps.badgeChanged", c->profile->handle,
                           "{\"id\":" + quoted(a.id) + ",\"badge\":" + value + "}");
        return BDK_OK;
    }
    return fail(err, BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_FOUND,
                "no installed app has the page in its scope");
}
