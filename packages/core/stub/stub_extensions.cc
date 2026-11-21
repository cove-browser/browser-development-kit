/* The stub's extensions: the extensions.* commands in memory, per profile id, and the action
   activation the host calls (bdk_extensions.h). Packages are manifests: an unpacked directory's
   manifest.json is read from disk, and every other source is a manifest a test registered with
   bdk_stub_extension_package. Source policy and manifest inspection are the core's
   (runtime/extension_policy.h), so the stub and the engine refuse the same things. Nothing runs:
   there is no service worker, so an action click that would run code is "dispatchedClick". It is
   the reference for result shapes and refusals, not for Chromium's behaviour. */
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>

#include "../runtime/extension_policy.h"
#include "bdk_stub.h"
#include "stub_services.h"

using namespace bdkcore;

namespace bdkstub {
namespace {

constexpr int kStubChromeMajor = 158; /* the milestone the engine is pinned to */

struct Ext {
    std::string id, manifest, path, store, update_url;
    ext::Source source = ext::Source::kUnpacked;
    bool enabled = true;
    std::vector<std::string> disable_reasons;
    std::string site_access;
    std::vector<std::string> site_patterns;
    bool allowed_in_private = false, file_access = false, pinned = false;
    std::map<std::string, std::string> shortcuts; /* command name -> the user's binding */
    std::vector<std::string> errors;              /* ExtensionError objects as JSON */
};

struct ExtState {
    std::map<std::string, std::vector<Ext>> profiles; /* by profile id, in install order */
    std::map<std::string, std::string> packages;      /* "<kind>:<key>" -> manifest JSON */
};

ExtState& es() {
    static ExtState* s = new ExtState;
    return *s;
}

/* ---- Manifests ----------------------------------------------------------------------------- */

JsonValue parse(const std::string& text) {
    JsonValue v;
    bool object = false;
    if (valid_json(text, &object) && object)
        json_parse(text, &v);
    return v;
}

std::string text_of(const JsonValue& o, const char* key) {
    const JsonValue* v = o.type == JsonValue::Type::kObject ? o.find(key) : nullptr;
    return v && v->type == JsonValue::Type::kString ? v->text : std::string();
}

std::vector<std::string> list_of(const JsonValue& o, const char* key) {
    std::vector<std::string> out;
    const JsonValue* v = o.type == JsonValue::Type::kObject ? o.find(key) : nullptr;
    if (v && v->type == JsonValue::Type::kArray) {
        for (const JsonValue& item : v->items) {
            if (item.type == JsonValue::Type::kString)
                out.push_back(item.text);
        }
    }
    return out;
}

const JsonValue* action_of(const JsonValue& m) {
    for (const char* key : {"action", "browser_action", "page_action"}) {
        if (const JsonValue* v = m.type == JsonValue::Type::kObject ? m.find(key) : nullptr)
            return v;
    }
    return nullptr;
}

/* Host patterns: host_permissions, and content script matches as Chromium counts them. */
std::vector<std::string> host_patterns(const JsonValue& m) {
    std::vector<std::string> out = list_of(m, "host_permissions");
    if (const JsonValue* scripts = m.find("content_scripts");
        scripts && scripts->type == JsonValue::Type::kArray) {
        for (const JsonValue& s : scripts->items) {
            for (const std::string& match : list_of(s, "matches")) {
                if (std::find(out.begin(), out.end(), match) == out.end())
                    out.push_back(match);
            }
        }
    }
    return out;
}

bool all_sites(const std::vector<std::string>& patterns) {
    for (const std::string& p : patterns) {
        if (p == "<all_urls>" || p == "*://*/*" || p == "http://*/*" || p == "https://*/*")
            return true;
    }
    return false;
}

/* The stub's warning strings. Chromium's wording is the engine's; these only keep the order:
   API permissions as declared, then hosts. */
std::string warnings_json(const JsonValue& m) {
    std::string out = "[";
    bool first = true;
    for (const std::string& p : list_of(m, "permissions")) {
        out += (first ? "\"" : ",\"") + json_escape("Uses " + p) + "\"";
        first = false;
    }
    return out + "]";
}

std::string host_warnings_json(const JsonValue& m) {
    std::string out = "[";
    bool first = true;
    for (const std::string& p : host_patterns(m)) {
        out += (first ? "\"" : ",\"") + json_escape("Read and change your data on " + p) + "\"";
        first = false;
    }
    return out + "]";
}

std::string strings_json(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i)
        out += (i ? ",\"" : "\"") + json_escape(items[i]) + "\"";
    return out + "]";
}

/* 32 characters from a..p, as Chromium's identifiers are, from the source key. Two FNV-1a
   passes stand in for Chromium's SHA-256 of the path or key. */
std::string make_id(const std::string& key) {
    uint64_t a = 1469598103934665603ull, b = 1099511628211ull ^ 0x9e3779b97f4a7c15ull;
    for (unsigned char c : key) {
        a = (a ^ c) * 1099511628211ull;
        b = (b ^ c) * 0x100000001b3ull + 0x9e37u;
    }
    std::string id;
    for (unsigned i = 0; i < 16; ++i)
        id += static_cast<char>('a' + ((a >> (i * 4u)) & 0xfu));
    for (unsigned i = 0; i < 16; ++i)
        id += static_cast<char>('a' + ((b >> (i * 4u)) & 0xfu));
    return id;
}

bool valid_id(const std::string& id) {
    return id.size() == 32 &&
           std::all_of(id.begin(), id.end(), [](char c) { return c >= 'a' && c <= 'p'; });
}

ext::EngineFacts facts() {
    ext::EngineFacts f;
    f.chrome_major = kStubChromeMajor;
    f.manifest_versions = {3};
    return f;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::string();
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/* The package a source names: its manifest, or empty. */
std::string package_manifest(ext::Source source, const std::string& key) {
    auto it = es().packages.find(std::string(ext::source_name(source)) + ":" + key);
    if (it != es().packages.end())
        return it->second;
    if (source == ext::Source::kUnpacked)
        return read_file(key + "/manifest.json");
    return std::string();
}

/* ---- Values -------------------------------------------------------------------------------- */

std::string info_json(const Runtime& rt, const Ext& e, bool with_errors) {
    const JsonValue m = parse(e.manifest);
    const ext::Report report = ext::inspect_manifest(rt, facts(), e.manifest);
    const JsonValue* mv = m.find("manifest_version");
    std::string options = text_of(m, "options_page");
    if (options.empty()) {
        if (const JsonValue* ui = m.find("options_ui"))
            options = text_of(*ui, "page");
    }
    std::string out = "{\"id\":\"" + e.id + "\",\"name\":\"" + json_escape(text_of(m, "name")) +
                      "\",\"version\":\"" + json_escape(text_of(m, "version")) +
                      "\",\"description\":\"" + json_escape(text_of(m, "description")) +
                      "\",\"manifestVersion\":" +
                      std::to_string(mv && mv->integral ? static_cast<int>(mv->number) : 0) +
                      ",\"source\":\"" + ext::source_name(e.source) + "\"";
    if (!e.store.empty())
        out += ",\"store\":\"" + json_escape(e.store) + "\"";
    out += ",\"isEnabled\":" + std::string(e.enabled ? "true" : "false") +
           ",\"disableReasons\":" + strings_json(e.disable_reasons) +
           ",\"permissions\":{\"apiPermissions\":" + strings_json(list_of(m, "permissions")) +
           ",\"hostPatterns\":" + strings_json(host_patterns(m)) + ",\"warnings\":[]}" +
           ",\"siteAccess\":\"" + e.site_access +
           "\",\"siteAccessPatterns\":" + strings_json(e.site_patterns) +
           ",\"allowedInPrivate\":" + (e.allowed_in_private ? "true" : "false") +
           ",\"allowedFileAccess\":" + (e.file_access ? "true" : "false") +
           ",\"isPinned\":" + (e.pinned ? "true" : "false") +
           ",\"surfaces\":" + strings_json(ext::manifest_surfaces(e.manifest));
    if (!options.empty())
        out += ",\"optionsPage\":\"chrome-extension://" + e.id + "/" + json_escape(options) + "\"";
    if (with_errors) {
        out += ",\"errors\":[";
        for (size_t i = 0; i < e.errors.size(); ++i)
            out += (i ? "," : "") + e.errors[i];
        out += ']';
    }
    return out + ",\"compatibility\":" + report.json() + "}";
}

std::vector<Ext>& extensions_of(const BuiltinCall& call) {
    return es().profiles[call.profile ? call.profile->id : std::string()];
}

Ext* find(std::vector<Ext>& list, const std::string& id) {
    for (Ext& e : list) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

bdk_profile_t profile_handle(const BuiltinCall& call) {
    return call.profile ? call.profile->handle : 0;
}

void emit(const std::shared_ptr<Runtime>& rt, const std::string& name, bdk_profile_t profile,
          const std::string& payload) {
    emit_builtin_event(rt, name, profile, payload);
}

void reject(const BuiltinCall& call, const char* code, const std::string& message) {
    call.control->Reject(BDK_DOMAIN_EXTENSION, code, message);
}

void invalid(const BuiltinCall& call, const std::string& message) {
    call.control->Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS, message);
}

/* A new extension from a manifest, with Chromium's initial site access. */
Ext make_ext(const std::string& id, const std::string& manifest, ext::Source source) {
    Ext e;
    e.id = id;
    e.manifest = manifest;
    e.source = source;
    const std::vector<std::string> hosts = host_patterns(parse(manifest));
    e.site_access = all_sites(hosts) ? "onAllSites" : hosts.empty() ? "onClick" : "onSpecificSites";
    if (e.site_access == "onSpecificSites")
        e.site_patterns = hosts;
    return e;
}

/* Whether the new manifest asks for anything the old one did not. */
bool permissions_increase(const std::string& old_manifest, const std::string& new_manifest) {
    const JsonValue before = parse(old_manifest), after = parse(new_manifest);
    const std::vector<std::string> old_api = list_of(before, "permissions"),
                                   old_hosts = host_patterns(before);
    for (const std::string& p : list_of(after, "permissions")) {
        if (std::find(old_api.begin(), old_api.end(), p) == old_api.end())
            return true;
    }
    for (const std::string& p : host_patterns(after)) {
        if (std::find(old_hosts.begin(), old_hosts.end(), p) == old_hosts.end())
            return true;
    }
    return false;
}

/* ---- Prompts ------------------------------------------------------------------------------- */

/* Raises an install-surface request and calls done(true) only for a granting completion. A
   default with no handler, a rejection, a drop and a withdrawal all refuse: the stub has no
   presenter that can ask a person. */
void ask(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<BuiltinCall>& call,
         uint32_t surface, const std::string& payload, const std::function<void(bool)>& done) {
    auto r = std::make_shared<Request>();
    r->surface = surface;
    r->profile = profile_handle(*call);
    r->payload = payload;
    r->on_finish = [done](UiOutcome outcome, const std::string& completion, const std::string&) {
        done(outcome == UiOutcome::kCompleted && completion == "grant");
    };
    track_request(r);
    raise_ui_request(rt, r);
    std::weak_ptr<Request> weak = r;
    /* Later: the caller learns broker.cancelled first, then the prompt goes away. */
    call->control->OnCancel([weak] {
        post([weak] {
            if (auto req = weak.lock())
                withdraw_ui_request(*req);
        });
    });
}

std::string install_payload(const Runtime& rt, const char* kind, const std::string& id,
                            const std::string& manifest, ext::Source source,
                            const ext::Report& report) {
    const JsonValue m = parse(manifest);
    (void)rt;
    std::string out = std::string("{\"kind\":\"") + kind + "\"";
    if (!id.empty())
        out += ",\"extensionId\":\"" + id + "\"";
    return out + ",\"extensionName\":\"" + json_escape(text_of(m, "name")) + "\",\"source\":\"" +
           ext::source_name(source) + "\",\"warnings\":" + warnings_json(m) +
           ",\"hostWarnings\":" + host_warnings_json(m) + ",\"compatibility\":" + report.json() +
           "}";
}

/* ---- Commands ------------------------------------------------------------------------------ */

struct SourceArgs {
    ext::Source source = ext::Source::kUnpacked;
    std::string key, path, id, update_url, store;
};

/* Reads an ExtensionSource; false (with the message) when the fields its kind needs are missing. */
bool read_source(const JsonValue& v, SourceArgs* out, std::string* message) {
    if (!ext::parse_source(text_of(v, "kind"), &out->source)) {
        *message = "source.kind is not a source kind";
        return false;
    }
    out->path = text_of(v, "path");
    out->id = text_of(v, "id");
    out->update_url = text_of(v, "updateUrl");
    out->store = text_of(v, "store");
    switch (out->source) {
    case ext::Source::kUnpacked:
    case ext::Source::kCrx:
        if (out->path.empty()) {
            *message = "source.path is required for this kind";
            return false;
        }
        out->key = out->path;
        return true;
    case ext::Source::kUpdateUrl:
        if (out->update_url.empty() || !valid_id(out->id)) {
            *message = "source.id (32 letters a to p) and source.updateUrl are required";
            return false;
        }
        out->key = out->id;
        return true;
    case ext::Source::kStore:
        if (out->store.empty() || !valid_id(out->id)) {
            *message = "source.store and source.id (32 letters a to p) are required";
            return false;
        }
        out->key = out->store + "/" + out->id;
        return true;
    case ext::Source::kBundled:
        *message = "bundled extensions are declared in bdk.toml, not installed";
        return false;
    }
    return false;
}

void install(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<BuiltinCall>& call,
             const JsonValue& args) {
    SourceArgs src;
    std::string message;
    const JsonValue* source = args.find("source");
    if (!source || !read_source(*source, &src, &message))
        return invalid(*call, message);
    std::string why = ext::source_refusal(rt->extensions, src.source, src.update_url, src.store);
    if (!why.empty())
        return reject(*call, BDK_CODE_SOURCE_NOT_ALLOWED, why);
    const std::string manifest = package_manifest(src.source, src.key);
    if (manifest.empty())
        return reject(*call, BDK_CODE_MANIFEST_INVALID, "no manifest.json for " + src.key);
    const ext::Report report = ext::inspect_manifest(*rt, facts(), manifest);
    if (ext::install_refused(rt->extensions, report, &message))
        return reject(*call, BDK_CODE_INCOMPATIBLE, message);
    const std::string id =
        src.source == ext::Source::kUpdateUrl || src.source == ext::Source::kStore
            ? src.id
            : make_id(std::string(ext::source_name(src.source)) + ":" + src.key);
    ask(rt, call, BDK_SURFACE_EXTENSION_INSTALLS,
        install_payload(*rt, "install", id, manifest, src.source, report),
        [rt, call, src, id, manifest](bool granted) {
            if (!granted)
                return reject(*call, BDK_CODE_CANCELLED, "the install was not confirmed");
            std::vector<Ext>& list = extensions_of(*call);
            Ext e = make_ext(id, manifest, src.source);
            e.path = src.path;
            e.store = src.store;
            e.update_url = src.update_url;
            const bdk_profile_t profile = profile_handle(*call);
            if (Ext* old = find(list, id)) {
                const std::string previous = text_of(parse(old->manifest), "version");
                e.pinned = old->pinned;
                *old = e;
                emit(rt, "extensions.updated", profile,
                     "{\"extension\":" + info_json(*rt, e, false) + ",\"previousVersion\":\"" +
                         json_escape(previous) + "\"}");
            } else {
                list.push_back(e);
                emit(rt, "extensions.installed", profile,
                     "{\"extension\":" + info_json(*rt, e, false) + "}");
            }
            emit(rt, "extensions.loaded", profile, "{\"id\":\"" + id + "\"}");
            call->control->Resolve(info_json(*rt, e, false));
        });
}

void uninstall(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<BuiltinCall>& call,
               const std::string& id) {
    Ext* e = find(extensions_of(*call), id);
    if (!e)
        return reject(*call, BDK_CODE_NOT_FOUND, "no extension " + id + " is installed");
    const std::string payload = "{\"extensionId\":\"" + id + "\",\"extensionName\":\"" +
                                json_escape(text_of(parse(e->manifest), "name")) +
                                "\",\"requestedBy\":{\"kind\":\"user\"}}";
    ask(rt, call, BDK_SURFACE_EXTENSION_UNINSTALLS, payload, [rt, call, id](bool granted) {
        if (!granted)
            return reject(*call, BDK_CODE_CANCELLED, "the uninstall was not confirmed");
        std::vector<Ext>& list = extensions_of(*call);
        list.erase(
            std::remove_if(list.begin(), list.end(), [&id](const Ext& x) { return x.id == id; }),
            list.end());
        const bdk_profile_t profile = profile_handle(*call);
        emit(rt, "extensions.unloaded", profile,
             "{\"id\":\"" + id + "\",\"reason\":\"uninstalled\"}");
        emit(rt, "extensions.uninstalled", profile, "{\"id\":\"" + id + "\"}");
        call->control->Resolve("null");
    });
}

void set_enabled(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<BuiltinCall>& call,
                 const std::string& id, bool enabled) {
    Ext* e = find(extensions_of(*call), id);
    if (!e)
        return reject(*call, BDK_CODE_NOT_FOUND, "no extension " + id + " is installed");
    const bdk_profile_t profile = profile_handle(*call);
    auto apply = [rt, call, id, enabled, profile] {
        Ext* x = find(extensions_of(*call), id);
        if (!x)
            return reject(*call, BDK_CODE_NOT_FOUND, "no extension " + id + " is installed");
        const bool changed = x->enabled != enabled;
        x->enabled = enabled;
        if (enabled)
            x->disable_reasons.clear();
        else if (std::find(x->disable_reasons.begin(), x->disable_reasons.end(), "user") ==
                 x->disable_reasons.end())
            x->disable_reasons.push_back("user");
        if (changed) {
            emit(rt, "extensions.enabledChanged", profile,
                 "{\"id\":\"" + id + "\",\"isEnabled\":" + (enabled ? "true" : "false") +
                     ",\"disableReasons\":" + strings_json(x->disable_reasons) + "}");
            emit(rt, enabled ? "extensions.loaded" : "extensions.unloaded", profile,
                 enabled ? "{\"id\":\"" + id + "\"}"
                         : "{\"id\":\"" + id + "\",\"reason\":\"disabled\"}");
        }
        call->control->Resolve("null");
    };
    const bool increased = std::find(e->disable_reasons.begin(), e->disable_reasons.end(),
                                     "permissionsIncreased") != e->disable_reasons.end();
    if (!enabled || !increased)
        return apply();
    /* Enabling after a permissions increase asks again (extensions.md section 3). */
    const ext::Report report = ext::inspect_manifest(*rt, facts(), e->manifest);
    ask(rt, call, BDK_SURFACE_EXTENSION_INSTALLS,
        install_payload(*rt, "reEnable", id, e->manifest, e->source, report),
        [call, apply](bool granted) {
            if (!granted)
                return reject(*call, BDK_CODE_CANCELLED, "re-enabling was not confirmed");
            apply();
        });
}

/* An installed version is replaced by a newer manifest: refused when incompatible (the installed
   version keeps running), disabled when it asks for more. */
bool apply_update(const std::shared_ptr<Runtime>& rt, const BuiltinCall& call, Ext& e,
                  const std::string& manifest, ext::Finding* refused) {
    const ext::Report report = ext::inspect_manifest(*rt, facts(), manifest);
    std::string message;
    if (ext::install_refused(rt->extensions, report, &message)) {
        *refused = *report.first_blocking();
        refused->subject = e.id + ": " + refused->subject;
        return false;
    }
    const std::string previous = text_of(parse(e.manifest), "version");
    const bool increased = permissions_increase(e.manifest, manifest);
    e.manifest = manifest;
    const bdk_profile_t profile = profile_handle(call);
    emit(rt, "extensions.unloaded", profile, "{\"id\":\"" + e.id + "\",\"reason\":\"updated\"}");
    if (increased) {
        e.enabled = false;
        e.disable_reasons = {"permissionsIncreased"};
        emit(rt, "extensions.enabledChanged", profile,
             "{\"id\":\"" + e.id + "\",\"isEnabled\":false,\"disableReasons\":" +
                 strings_json(e.disable_reasons) + "}");
    } else if (e.enabled) {
        emit(rt, "extensions.loaded", profile, "{\"id\":\"" + e.id + "\"}");
    }
    emit(rt, "extensions.updated", profile,
         "{\"extension\":" + info_json(*rt, e, false) + ",\"previousVersion\":\"" +
             json_escape(previous) + "\"}");
    return true;
}

std::string finding_json(const ext::Finding& f) {
    return "{\"severity\":\"" + f.severity + "\",\"code\":\"" + json_escape(f.code) +
           "\",\"subject\":\"" + json_escape(f.subject) + "\",\"message\":\"" +
           json_escape(f.message) + "\"}";
}

void check_for_updates(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<BuiltinCall>& call,
                       const std::string& only) {
    std::vector<Ext>& list = extensions_of(*call);
    if (!only.empty() && !find(list, only))
        return reject(*call, BDK_CODE_NOT_FOUND, "no extension " + only + " is installed");
    std::vector<std::string> updated, refused;
    size_t done = 0;
    for (Ext& e : list) {
        ++done;
        call->control->Progress(static_cast<double>(done) / static_cast<double>(list.size()), e.id);
        if (!only.empty() && e.id != only)
            continue;
        if (e.source != ext::Source::kUpdateUrl && e.source != ext::Source::kStore)
            continue;
        /* Updates follow the configuration: a source switched off since is not checked. */
        if (!ext::source_refusal(rt->extensions, e.source, e.update_url, e.store).empty())
            continue;
        const std::string key = e.source == ext::Source::kStore ? e.store + "/" + e.id : e.id;
        const std::string manifest = package_manifest(e.source, key);
        if (manifest.empty() ||
            text_of(parse(manifest), "version") == text_of(parse(e.manifest), "version"))
            continue;
        ext::Finding finding;
        if (apply_update(rt, *call, e, manifest, &finding))
            updated.push_back(e.id);
        else
            refused.push_back(finding_json(finding));
    }
    std::string out = "{\"updated\":" + strings_json(updated) + ",\"refused\":[";
    for (size_t i = 0; i < refused.size(); ++i)
        out += (i ? "," : "") + refused[i];
    call->control->Resolve(out + "]}");
}

void reload(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<BuiltinCall>& call,
            const std::string& id) {
    Ext* e = find(extensions_of(*call), id);
    if (!e)
        return reject(*call, BDK_CODE_NOT_FOUND, "no extension " + id + " is installed");
    if (e->source != ext::Source::kUnpacked)
        return reject(*call, BDK_CODE_NOT_UNPACKED, "only unpacked extensions are reloaded");
    const std::string manifest = package_manifest(ext::Source::kUnpacked, e->path);
    if (manifest.empty())
        return reject(*call, BDK_CODE_MANIFEST_INVALID, "no manifest.json in " + e->path);
    ext::Finding finding;
    if (!apply_update(rt, *call, *e, manifest, &finding))
        return reject(*call, BDK_CODE_INCOMPATIBLE, finding.message + " (" + finding.code + ")");
    call->control->Resolve("null");
}

std::string commands_json(const Ext& e) {
    std::string out;
    const JsonValue m = parse(e.manifest);
    const JsonValue* commands = m.find("commands");
    if (!commands || commands->type != JsonValue::Type::kObject)
        return out;
    for (const JsonMember& c : commands->members) {
        std::string suggested;
        if (const JsonValue* key = c.second.find("suggested_key")) {
            suggested = key->type == JsonValue::Type::kString ? key->text : text_of(*key, "mac");
            if (suggested.empty() && key->type == JsonValue::Type::kObject)
                suggested = text_of(*key, "default");
        }
        auto bound = e.shortcuts.find(c.first);
        const std::string shortcut = bound != e.shortcuts.end() ? bound->second : suggested;
        const bool is_action = c.first == "_execute_action" ||
                               c.first == "_execute_browser_action" ||
                               c.first == "_execute_page_action";
        out += (out.empty() ? "" : ",") + std::string("{\"extensionId\":\"") + e.id +
               "\",\"name\":\"" + json_escape(c.first) + "\",\"description\":\"" +
               json_escape(text_of(c.second, "description")) + "\"" +
               (suggested.empty() ? "" : ",\"suggestedKey\":\"" + json_escape(suggested) + "\"") +
               (shortcut.empty() ? "" : ",\"shortcut\":\"" + json_escape(shortcut) + "\"") +
               ",\"isAction\":" + (is_action ? "true" : "false") + "}";
    }
    return out;
}

bool declares_command(const Ext& e, const std::string& name) {
    const JsonValue m = parse(e.manifest);
    const JsonValue* commands = m.find("commands");
    return commands && commands->type == JsonValue::Type::kObject && commands->find(name);
}

std::string action_json(const Ext& e) {
    const JsonValue m = parse(e.manifest);
    const JsonValue* action = action_of(m);
    std::string title = action ? text_of(*action, "default_title") : std::string();
    if (title.empty())
        title = text_of(m, "name");
    const bool popup = action && !text_of(*action, "default_popup").empty();
    return "{\"id\":\"" + e.id + "\",\"title\":\"" + json_escape(title) +
           "\",\"isEnabled\":true,\"hasPopup\":" + (popup ? "true" : "false") +
           ",\"wantsAccess\":false,\"isPinned\":" + (e.pinned ? "true" : "false") + "}";
}

} // namespace

/* ---- Entry points -------------------------------------------------------------------------- */

void extensions_reset() {
    es().profiles.clear();
    es().packages.clear();
}

void extensions_add_package(const std::string& kind, const std::string& key,
                            const std::string& manifest) {
    es().packages[kind + ":" + key] = manifest;
}

bool extensions_execute(Runtime& runtime, const std::shared_ptr<BuiltinCall>& call) {
    if (call->name.rfind("extensions.", 0) != 0)
        return false;
    std::shared_ptr<Runtime> rt = current_runtime();
    if (!rt)
        return false;
    (void)runtime;
    JsonValue args;
    json_parse(call->arguments, &args);
    const std::string id = text_of(args, "id");
    const std::string& name = call->name;
    std::vector<Ext>& list = extensions_of(*call);
    const auto bool_arg = [&args](const char* key) {
        const JsonValue* v = args.find(key);
        return v && v->type == JsonValue::Type::kBool && v->boolean;
    };

    if (name == "extensions.list") {
        std::string out = "[";
        for (size_t i = 0; i < list.size(); ++i)
            out += (i ? "," : "") + info_json(*rt, list[i], false);
        call->control->Resolve(out + "]");
    } else if (name == "extensions.get") {
        Ext* e = find(list, id);
        if (!e)
            reject(*call, BDK_CODE_NOT_FOUND, "no extension " + id + " is installed");
        else
            call->control->Resolve(info_json(*rt, *e, true));
    } else if (name == "extensions.inspect") {
        std::string manifest;
        if (const JsonValue* source = args.find("source")) {
            SourceArgs src;
            std::string message;
            if (!read_source(*source, &src, &message))
                return invalid(*call, message), true;
            manifest = package_manifest(src.source, src.key);
            if (manifest.empty())
                return reject(*call, BDK_CODE_MANIFEST_INVALID, "no manifest.json for " + src.key),
                       true;
        } else if (Ext* e = find(list, id)) {
            manifest = e->manifest;
        } else {
            if (id.empty())
                return invalid(*call, "inspect needs a source or an id"), true;
            return reject(*call, BDK_CODE_NOT_FOUND, "no extension " + id + " is installed"), true;
        }
        call->control->Resolve(ext::inspect_manifest(*rt, facts(), manifest).json());
    } else if (name == "extensions.install") {
        install(rt, call, args);
    } else if (name == "extensions.uninstall") {
        uninstall(rt, call, id);
    } else if (name == "extensions.setEnabled") {
        set_enabled(rt, call, id, bool_arg("enabled"));
    } else if (name == "extensions.checkForUpdates") {
        check_for_updates(rt, call, id);
    } else if (name == "extensions.reload") {
        reload(rt, call, id);
    } else if (name == "extensions.commands") {
        std::string out;
        for (const Ext& e : list) {
            std::string c = commands_json(e);
            if (!c.empty())
                out += (out.empty() ? "" : ",") + c;
        }
        call->control->Resolve("[" + out + "]");
    } else if (name == "extensions.actions") {
        std::string out;
        for (const Ext& e : list) {
            if (e.enabled && action_of(parse(e.manifest)))
                out += (out.empty() ? "" : ",") + action_json(e);
        }
        call->control->Resolve("[" + out + "]");
    } else {
        /* The rest change one installed extension. */
        const std::string target =
            name == "extensions.setCommandShortcut" ? text_of(args, "extensionId") : id;
        Ext* e = find(list, target);
        if (!e) {
            reject(*call, BDK_CODE_NOT_FOUND, "no extension " + target + " is installed");
            return true;
        }
        if (name == "extensions.setSiteAccess") {
            const std::string access = text_of(args, "access");
            std::vector<std::string> patterns = list_of(args, "patterns");
            if (access == "onSpecificSites" && patterns.empty())
                return invalid(*call, "onSpecificSites needs patterns"), true;
            e->site_access = access;
            e->site_patterns = access == "onSpecificSites" ? patterns : std::vector<std::string>{};
        } else if (name == "extensions.setAllowedInPrivate") {
            if (rt->extensions.private_never)
                return reject(*call, BDK_CODE_PRIVATE_NOT_ALLOWED,
                              "extensions never run in private profiles ([extensions] "
                              "allow_in_private = \"never\")"),
                       true;
            e->allowed_in_private = bool_arg("allowed");
        } else if (name == "extensions.setAllowedFileAccess") {
            e->file_access = bool_arg("allowed");
        } else if (name == "extensions.setPinned") {
            e->pinned = bool_arg("pinned");
            emit(rt, "extensions.actionChanged", profile_handle(*call),
                 "{\"id\":\"" + e->id + "\",\"contents\":null}");
        } else if (name == "extensions.clearErrors") {
            e->errors.clear();
        } else if (name == "extensions.setCommandShortcut") {
            const std::string command = text_of(args, "name");
            if (!declares_command(*e, command))
                return reject(*call, BDK_CODE_NOT_FOUND,
                              "the extension declares no command " + command),
                       true;
            const std::string shortcut = text_of(args, "shortcut");
            if (shortcut.empty())
                e->shortcuts.erase(command);
            else
                e->shortcuts[command] = shortcut;
        } else if (name == "extensions.openOptions") {
            if (!ui_surface_available(*rt, BDK_SURFACE_EXTENSION_OPTIONS_PAGES))
                return reject(*call, BDK_CODE_SURFACE_DISABLED,
                              "options pages are disabled ([extensions.ui] options_pages)"),
                       true;
            const std::vector<std::string> surfaces = ext::manifest_surfaces(e->manifest);
            if (std::find(surfaces.begin(), surfaces.end(), "optionsPage") == surfaces.end())
                return reject(*call, BDK_CODE_NOT_FOUND, "the extension has no options page"), true;
            /* The stub has no page to open; the engine raises a new-window request. */
        } else if (name == "extensions.inspectView") {
            /* Nothing to inspect without a renderer. */
        } else {
            return false;
        }
        call->control->Resolve("null");
    }
    return true;
}

bdk_status_t extensions_activate(Runtime& runtime, const std::shared_ptr<Contents>& contents,
                                 const std::string& id, const std::string& command,
                                 std::string* result, bdk_error_t* err) {
    std::shared_ptr<Runtime> rt = current_runtime();
    std::vector<Ext>& list =
        es().profiles[contents->profile ? contents->profile->id : std::string()];
    Ext* e = find(list, id);
    if (!rt || !e || !e->enabled)
        return fail(err, BDK_DOMAIN_EXTENSION, BDK_CODE_NOT_FOUND,
                    "no enabled extension \"" + id + "\" in the page's profile");
    (void)runtime;
    const bool action_command = command.empty() || command == "_execute_action" ||
                                command == "_execute_browser_action" ||
                                command == "_execute_page_action";
    if (!action_command) {
        if (!declares_command(*e, command))
            return fail(err, BDK_DOMAIN_EXTENSION, BDK_CODE_NOT_FOUND,
                        "the extension declares no command \"" + command + "\"");
        *result = "{\"outcome\":\"dispatchedClick\"}";
        return BDK_OK;
    }
    const JsonValue m = parse(e->manifest);
    const JsonValue* action = action_of(m);
    if (!action) {
        *result = "{\"outcome\":\"blocked\",\"reason\":\"the extension has no action\"}";
        return BDK_OK;
    }
    const std::string popup = text_of(*action, "default_popup");
    if (popup.empty()) {
        *result = "{\"outcome\":\"dispatchedClick\"}";
        return BDK_OK;
    }
    if (!ui_surface_available(*rt, BDK_SURFACE_EXTENSION_POPUPS)) {
        *result = "{\"outcome\":\"blocked\",\"reason\":\"popups are disabled ([extensions.ui] "
                  "popups)\"}";
        return BDK_OK;
    }
    auto r = std::make_shared<Request>();
    r->surface = BDK_SURFACE_EXTENSION_POPUPS;
    r->contents = contents->handle;
    r->payload = "{\"extensionId\":\"" + e->id + "\",\"extensionName\":\"" +
                 json_escape(text_of(m, "name")) + "\",\"url\":\"chrome-extension://" + e->id +
                 "/" + json_escape(popup) + "\",\"trigger\":\"" +
                 (command.empty() ? "userClick" : "command") +
                 "\",\"preferredSize\":{\"width\":0,\"height\":0}}";
    r->on_finish = [](UiOutcome, const std::string&, const std::string&) {};
    track_request(r);
    raise_ui_request(rt, r);
    *result = "{\"outcome\":\"popupRequested\"}";
    return BDK_OK;
}

} // namespace bdkstub
