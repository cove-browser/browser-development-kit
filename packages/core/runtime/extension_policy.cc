/* Extension source policy and manifest inspection (extension_policy.h). */
#include "extension_policy.h"

#include <algorithm>
#include <cstdlib>

#include "broker_internal.h"

namespace bdkcore {
namespace ext {
namespace {

/* ---- Reading JSON -------------------------------------------------------------------------- */

const JsonValue* member(const JsonValue& object, const char* key) {
    return object.type == JsonValue::Type::kObject ? object.find(key) : nullptr;
}

bool is_string(const JsonValue* v) {
    return v && v->type == JsonValue::Type::kString;
}

std::string string_or(const JsonValue* v, const std::string& fallback = "") {
    return is_string(v) ? v->text : fallback;
}

/* The string items of an array member; objects and other values are skipped. */
std::vector<std::string> strings(const JsonValue& object, const char* key) {
    std::vector<std::string> out;
    const JsonValue* v = member(object, key);
    if (!v || v->type != JsonValue::Type::kArray)
        return out;
    for (const JsonValue& item : v->items) {
        if (item.type == JsonValue::Type::kString)
            out.push_back(item.text);
    }
    return out;
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

/* The leading integer of a version string ("120.0.6099.0" is 120); 0 for none. */
int major_of(const std::string& version) {
    return static_cast<int>(std::strtol(version.c_str(), nullptr, 10));
}

/* ---- What this product lacks --------------------------------------------------------------- */

/* APIs an extension may require that BDK does not provide (extensions.md section 7). Absent ones
   are blocking (the API is missing from the extension's chrome object); degraded ones work in
   part and fail explicitly where they cannot. */
struct MissingApi {
    const char* permission;
    const char* severity;
    const char* code;
    const char* message;
};

// clang-format off
constexpr MissingApi kMissingApis[] = {
    {"identity", "degrading", "api.identity.getAuthToken.unavailable",
     "identity.getAuthToken and getProfileUserInfo are absent: there is no Google sign-in. launchWebAuthFlow works."},
    {"sessions", "degrading", "api.sessions.restore.unavailable",
     "BDK keeps no session history: sessions.getRecentlyClosed is empty and restore rejects."},
    {"tabGroups", "degrading", "api.tabGroups.notShown",
     "Groups exist in the engine's tab model, but the application shows no tab groups."},
    {"enterprise.deviceAttributes", "blocking", "api.enterprise.unavailable",
     "Enterprise APIs are absent."},
    {"enterprise.hardwarePlatform", "blocking", "api.enterprise.unavailable",
     "Enterprise APIs are absent."},
    {"enterprise.networkingAttributes", "blocking", "api.enterprise.unavailable",
     "Enterprise APIs are absent."},
    {"enterprise.platformKeys", "blocking", "api.enterprise.unavailable",
     "Enterprise APIs are absent."},
    {"platformKeys", "blocking", "api.platformKeys.unavailable", "platformKeys is ChromeOS only."},
    {"loginState", "blocking", "api.loginState.unavailable", "loginState is ChromeOS only."},
    {"certificateProvider", "blocking", "api.certificateProvider.unavailable",
     "certificateProvider is ChromeOS only."},
    {"webstorePrivate", "blocking", "api.webstorePrivate.unavailable",
     "webstorePrivate is reserved for the store's own pages."},
};
// clang-format on

/* The surfaces the checks need, from the configured modes. */
struct Surfaces {
    bool actions, popups, side_panels, context_menus, options_pages, devtools, notifications,
        screen_capture;
};

Surfaces surfaces_of(const Runtime& rt) {
    return Surfaces{
        ui_surface_available(rt, BDK_SURFACE_EXTENSION_ACTIONS),
        ui_surface_available(rt, BDK_SURFACE_EXTENSION_POPUPS),
        ui_surface_available(rt, BDK_SURFACE_EXTENSION_SIDE_PANELS),
        ui_surface_available(rt, BDK_SURFACE_EXTENSION_CONTEXT_MENUS),
        ui_surface_available(rt, BDK_SURFACE_EXTENSION_OPTIONS_PAGES),
        ui_surface_available(rt, BDK_SURFACE_EXTENSION_DEVTOOLS),
        ui_surface_available(rt, BDK_SURFACE_NOTIFICATIONS),
        ui_surface_available(rt, BDK_SURFACE_SCREEN_CAPTURE),
    };
}

/* The action key of either manifest version, or null. */
const JsonValue* action_of(const JsonValue& manifest) {
    for (const char* key : {"action", "browser_action", "page_action"}) {
        if (const JsonValue* v = member(manifest, key))
            return v;
    }
    return nullptr;
}

const char* action_key(const JsonValue& manifest) {
    for (const char* key : {"action", "browser_action", "page_action"}) {
        if (member(manifest, key))
            return key;
    }
    return "action";
}

bool has_options(const JsonValue& manifest) {
    return is_string(member(manifest, "options_page")) || member(manifest, "options_ui");
}

bool has_side_panel(const JsonValue& manifest, const std::vector<std::string>& permissions) {
    return member(manifest, "side_panel") || contains(permissions, "sidePanel");
}

void add(Report* report, const char* severity, const std::string& code, const std::string& subject,
         const std::string& message) {
    report->findings.push_back(Finding{severity, code, subject, message});
}

} // namespace

bool parse_source(const std::string& kind, Source* out) {
    static constexpr struct {
        const char* name;
        Source source;
    } kNames[] = {{"unpacked", Source::kUnpacked},
                  {"crx", Source::kCrx},
                  {"updateUrl", Source::kUpdateUrl},
                  {"store", Source::kStore},
                  {"bundled", Source::kBundled}};
    for (const auto& n : kNames) {
        if (kind == n.name) {
            *out = n.source;
            return true;
        }
    }
    return false;
}

const char* source_name(Source source) {
    switch (source) {
    case Source::kUnpacked:
        return "unpacked";
    case Source::kCrx:
        return "crx";
    case Source::kUpdateUrl:
        return "updateUrl";
    case Source::kStore:
        return "store";
    case Source::kBundled:
        return "bundled";
    }
    return "unpacked";
}

bool store_known(const std::string& store) {
    return store == "chrome-web-store";
}

bool url_pattern_matches(const std::string& pattern, const std::string& url) {
    /* Iterative glob with backtracking to the last star: linear in practice, no recursion. */
    size_t p = 0, u = 0, star = std::string::npos, mark = 0;
    while (u < url.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = u;
        } else if (p < pattern.size() && pattern[p] == url[u]) {
            ++p;
            ++u;
        } else if (star != std::string::npos) {
            p = star + 1;
            u = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

std::string source_refusal(const ExtensionConfig& config, Source source,
                           const std::string& update_url, const std::string& store) {
    if (!config.enabled)
        return "extensions are disabled ([extensions] enabled = false)";
    switch (source) {
    case Source::kBundled:
        return "";
    case Source::kUnpacked:
        if (!config.unpacked)
            return "unpacked extensions are not allowed ([extensions.install] unpacked = false)";
        if (config.developer_mode == ExtensionConfig::DeveloperMode::kOff)
            return "loading unpacked extensions is unavailable ([extensions] developer_mode = "
                   "\"off\")";
        return "";
    case Source::kCrx:
        return config.crx ? "" : "CRX files are not allowed ([extensions.install] crx = false)";
    case Source::kUpdateUrl:
        if (config.any_update_url)
            return "";
        for (const std::string& pattern : config.update_urls) {
            if (url_pattern_matches(pattern, update_url))
                return "";
        }
        return "the update URL " + update_url +
               " matches no pattern in [extensions.install] update_urls";
    case Source::kStore:
        if (!store_known(store))
            return "BDK has no store named \"" + store + "\"";
        if (!contains(config.stores, store))
            return "the store \"" + store + "\" is not enabled ([extensions.install] stores)";
        return "";
    }
    return "";
}

bool blocklisted(const ExtensionConfig& config, const std::string& id) {
    return contains(config.blocklist, id);
}

std::string blocklist_refusal(const std::string& id) {
    return "the extension " + id + " is in [extensions.install] blocklist";
}

std::string bundled_directory(const Runtime& rt, const ExtensionConfig::Bundled& bundled) {
    return rt.bundle_path + "/Contents/Resources/" + bundled.path;
}

std::string Report::verdict() const {
    bool degraded = false;
    for (const Finding& f : findings) {
        if (f.severity == "blocking")
            return "incompatible";
        if (f.severity == "degrading")
            degraded = true;
    }
    return degraded ? "degraded" : "compatible";
}

const Finding* Report::first_blocking() const {
    for (const Finding& f : findings) {
        if (f.severity == "blocking")
            return &f;
    }
    return nullptr;
}

std::string Report::json() const {
    std::string out = "{\"verdict\":\"" + verdict() + "\",\"findings\":[";
    for (size_t i = 0; i < findings.size(); ++i) {
        const Finding& f = findings[i];
        out += (i ? "," : "") + std::string("{\"severity\":\"") + f.severity + "\",\"code\":\"" +
               json_escape(f.code) + "\",\"subject\":\"" + json_escape(f.subject) +
               "\",\"message\":\"" + json_escape(f.message) + "\"}";
    }
    return out + "]}";
}

Report inspect_manifest(const Runtime& rt, const EngineFacts& facts,
                        const std::string& manifest_json) {
    Report report;
    JsonValue manifest;
    bool object = false;
    if (!valid_json(manifest_json, &object) || !object || !json_parse(manifest_json, &manifest)) {
        add(&report, "blocking", "manifest.invalid", "manifest.json",
            "manifest.json is not a JSON object");
        return report;
    }
    if (!is_string(member(manifest, "name")) || !is_string(member(manifest, "version"))) {
        add(&report, "blocking", "manifest.invalid", "manifest.json",
            "the manifest has no name or no version");
    }

    const JsonValue* mv = member(manifest, "manifest_version");
    const int version = mv && mv->type == JsonValue::Type::kNumber && mv->integral
                            ? static_cast<int>(mv->number)
                            : 0;
    const auto& supported = facts.manifest_versions;
    if (std::find(supported.begin(), supported.end(), version) == supported.end()) {
        add(&report, "blocking", "manifest.version.unsupported", "manifest_version",
            "manifest version " + std::to_string(version) + " is not one this engine loads");
    }

    const std::string minimum = string_or(member(manifest, "minimum_chrome_version"));
    if (!minimum.empty() && facts.chrome_major > 0 && major_of(minimum) > facts.chrome_major) {
        add(&report, "blocking", "manifest.minimumChromeVersion", "minimum_chrome_version",
            "the extension needs Chromium " + minimum + " and the engine is Chromium " +
                std::to_string(facts.chrome_major));
    }

    const std::vector<std::string> permissions = strings(manifest, "permissions");
    for (const MissingApi& api : kMissingApis) {
        if (contains(permissions, api.permission))
            add(&report, api.severity, api.code, api.permission, api.message);
    }

    const Surfaces s = surfaces_of(rt);
    const JsonValue* action = action_of(manifest);
    if (action && !s.actions) {
        add(&report, "blocking", "surface.action.disabled", action_key(manifest),
            "the extension has an action and actions are disabled ([extensions.ui] actions)");
    }
    if (action && s.actions && !s.popups && is_string(member(*action, "default_popup"))) {
        add(&report, "blocking", "surface.popup.disabled",
            std::string(action_key(manifest)) + ".default_popup",
            "the extension's action opens a popup and popups are disabled ([extensions.ui] "
            "popups)");
    }
    if (has_side_panel(manifest, permissions) && !s.side_panels) {
        add(&report, "blocking", "surface.sidePanel.disabled", "side_panel",
            "the extension has a side panel and side panels are disabled ([extensions.ui] "
            "side_panels)");
    }
    if (contains(permissions, "contextMenus") && !s.context_menus) {
        add(&report, "blocking", "surface.contextMenus.disabled", "contextMenus",
            "the extension adds context-menu items and they are disabled ([extensions.ui] "
            "context_menus)");
    }
    if (has_options(manifest) && !s.options_pages) {
        add(&report, "degrading", "surface.optionsPage.disabled",
            member(manifest, "options_ui") ? "options_ui" : "options_page",
            "the extension's options page cannot be opened ([extensions.ui] options_pages)");
    }
    if (member(manifest, "devtools_page") && !s.devtools) {
        add(&report, "degrading", "surface.devTools.disabled", "devtools_page",
            "the extension's DevTools panels are not loaded ([extensions.ui] devtools)");
    }
    if (contains(permissions, "notifications") && !s.notifications) {
        add(&report, "degrading", "surface.notifications.disabled", "notifications",
            "notifications are disabled ([ui.surfaces] notifications)");
    }
    if ((contains(permissions, "desktopCapture") || contains(permissions, "tabCapture")) &&
        !s.screen_capture) {
        add(&report, "degrading", "surface.screenCapture.disabled",
            contains(permissions, "desktopCapture") ? "desktopCapture" : "tabCapture",
            "screen capture is disabled ([ui.surfaces] screen_capture)");
    }
    if (member(manifest, "omnibox")) {
        add(&report, "degrading", "surface.omnibox.noHost", "omnibox",
            "BDK has no address-bar service yet, so omnibox keywords reach no address bar");
    }
    if (member(manifest, "chrome_url_overrides")) {
        add(&report, "informational", "manifest.chromeUrlOverrides", "chrome_url_overrides",
            "the application's internal pages decide what new-tab, history and bookmarks show");
    }
    if (contains(permissions, "debugger")) {
        add(&report, "informational", "api.debugger.notice", "debugger",
            "the application is expected to show that the extension is debugging a page");
    }
    if (string_or(member(manifest, "incognito")) == "split" && rt.extensions.private_never) {
        add(&report, "informational", "private.never", "incognito",
            "extensions never run in private profiles ([extensions] allow_in_private = never)");
    }

    const std::string update_url = string_or(member(manifest, "update_url"));
    if (!update_url.empty() && !rt.extensions.any_update_url) {
        const std::string why = source_refusal(rt.extensions, Source::kUpdateUrl, update_url, "");
        if (!why.empty())
            add(&report, "blocking", "source.updateUrl.notAllowed", "update_url", why);
    }
    return report;
}

std::vector<std::string> manifest_surfaces(const std::string& manifest_json) {
    std::vector<std::string> out;
    JsonValue manifest;
    bool object = false;
    if (!valid_json(manifest_json, &object) || !object || !json_parse(manifest_json, &manifest))
        return out;
    const std::vector<std::string> permissions = strings(manifest, "permissions");
    const JsonValue* action = action_of(manifest);
    if (action)
        out.emplace_back("action");
    if (action && is_string(member(*action, "default_popup")))
        out.emplace_back("popup");
    if (has_side_panel(manifest, permissions))
        out.emplace_back("sidePanel");
    if (contains(permissions, "contextMenus"))
        out.emplace_back("contextMenus");
    if (has_options(manifest))
        out.emplace_back("optionsPage");
    if (member(manifest, "devtools_page"))
        out.emplace_back("devTools");
    if (member(manifest, "omnibox"))
        out.emplace_back("omnibox");
    return out;
}

bool install_refused(const ExtensionConfig& config, const Report& report, std::string* message) {
    const Finding* blocking = report.first_blocking();
    if (!blocking || config.allow_all)
        return false;
    *message = blocking->message + " (" + blocking->code + ")";
    return true;
}

std::string store_install_label(const std::string& browser_name) {
    return "Add to " + browser_name;
}

} // namespace ext

/* ---- Configuration ------------------------------------------------------------------------- */

namespace {

bdk_status_t config_error(bdk_error_t* out_error, const std::string& what) {
    return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                what + " in the resolved configuration");
}

bdk_status_t read_bool(const JsonValue& object, const char* key, bool* out, bdk_error_t* err,
                       const std::string& where) {
    const JsonValue* v = object.find(key);
    if (!v)
        return BDK_OK;
    if (v->type != JsonValue::Type::kBool)
        return config_error(err, "\"" + where + key + "\" is not a boolean");
    *out = v->boolean;
    return BDK_OK;
}

} // namespace

bdk_status_t extension_configure(Runtime& rt, const std::string& resolved, bdk_error_t* err) {
    JsonValue root;
    if (!json_parse(resolved, &root) || root.type != JsonValue::Type::kObject)
        return config_error(err, "the configuration is not a JSON object; it is invalid");
    if (const JsonValue* browser = root.find("browser")) {
        if (const JsonValue* name = browser->find("name");
            name && name->type == JsonValue::Type::kString)
            rt.browser_name = name->text;
    }
    const JsonValue* ext = root.find("extensions");
    if (!ext)
        return BDK_OK;
    if (ext->type != JsonValue::Type::kObject)
        return config_error(err, "\"extensions\" is not an object");
    ExtensionConfig& c = rt.extensions;
    if (bdk_status_t s = read_bool(*ext, "enabled", &c.enabled, err, "extensions."))
        return s;
    if (const JsonValue* policy = ext->find("install_policy")) {
        if (policy->type != JsonValue::Type::kString ||
            (policy->text != "reject_known_incompatible" && policy->text != "allow_all"))
            return config_error(err, "\"extensions.install_policy\" is not a known policy");
        c.allow_all = policy->text == "allow_all";
    }
    if (const JsonValue* in_private = ext->find("allow_in_private")) {
        if (in_private->type != JsonValue::Type::kString ||
            (in_private->text != "user" && in_private->text != "never"))
            return config_error(err, "\"extensions.allow_in_private\" is not user or never");
        c.private_never = in_private->text == "never";
    }
    if (const JsonValue* mode = ext->find("developer_mode")) {
        if (mode->type != JsonValue::Type::kString ||
            (mode->text != "user" && mode->text != "on" && mode->text != "off"))
            return config_error(err, "\"extensions.developer_mode\" is not user, on or off");
        c.developer_mode = mode->text == "on"    ? ExtensionConfig::DeveloperMode::kOn
                           : mode->text == "off" ? ExtensionConfig::DeveloperMode::kOff
                                                 : ExtensionConfig::DeveloperMode::kUser;
    }
    if (const JsonValue* bundled = ext->find("bundled")) {
        if (bundled->type != JsonValue::Type::kObject)
            return config_error(err, "\"extensions.bundled\" is not an object");
        for (const auto& [name, entry] : bundled->members) {
            const JsonValue* path = entry.find("path");
            if (entry.type != JsonValue::Type::kObject || !path ||
                path->type != JsonValue::Type::kString || path->text.empty() ||
                path->text.front() == '/' || path->text.find("..") != std::string::npos)
                return config_error(err, "\"extensions.bundled." + name +
                                             "\" has no path inside the bundle");
            ExtensionConfig::Bundled b;
            b.name = name;
            b.path = path->text;
            if (bdk_status_t s = read_bool(entry, "required", &b.required, err,
                                           "extensions.bundled." + name + "."))
                return s;
            c.bundled.push_back(std::move(b));
        }
    }
    const JsonValue* install = ext->find("install");
    if (!install)
        return BDK_OK;
    if (install->type != JsonValue::Type::kObject)
        return config_error(err, "\"extensions.install\" is not an object");
    if (bdk_status_t s = read_bool(*install, "unpacked", &c.unpacked, err, "extensions.install."))
        return s;
    if (bdk_status_t s = read_bool(*install, "crx", &c.crx, err, "extensions.install."))
        return s;
    if (const JsonValue* urls = install->find("update_urls")) {
        if (urls->type == JsonValue::Type::kString && urls->text == "any") {
            c.any_update_url = true;
        } else if (urls->type == JsonValue::Type::kArray) {
            c.any_update_url = false;
            for (const JsonValue& u : urls->items) {
                if (u.type != JsonValue::Type::kString)
                    return config_error(err, "\"extensions.install.update_urls\" has a non-string");
                c.update_urls.push_back(u.text);
            }
        } else {
            return config_error(err, "\"extensions.install.update_urls\" is not any or a list");
        }
    }
    if (const JsonValue* stores = install->find("stores")) {
        if (stores->type != JsonValue::Type::kArray)
            return config_error(err, "\"extensions.install.stores\" is not a list");
        for (const JsonValue& st : stores->items) {
            if (st.type != JsonValue::Type::kString || !ext::store_known(st.text))
                return fail(err, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                            "\"extensions.install.stores\" names a store BDK does not ship");
            c.stores.push_back(st.text);
        }
    }
    if (const JsonValue* blocklist = install->find("blocklist")) {
        if (blocklist->type != JsonValue::Type::kArray)
            return config_error(err, "\"extensions.install.blocklist\" is not a list");
        for (const JsonValue& id : blocklist->items) {
            const bool valid = id.type == JsonValue::Type::kString && id.text.size() == 32 &&
                               std::all_of(id.text.begin(), id.text.end(),
                                           [](char ch) { return ch >= 'a' && ch <= 'p'; });
            if (!valid)
                return config_error(err, "\"extensions.install.blocklist\" has an entry that is "
                                         "not an extension identifier");
            c.blocklist.push_back(id.text);
        }
    }
    return BDK_OK;
}

} // namespace bdkcore
