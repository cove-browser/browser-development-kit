/* The commands the core answers itself (automation_internal.h, core_execute_command): the
   contents.* commands that need no renderer, built on the Backend's page operations so that every
   runtime behaves the same; all of scripts.*, whose registry the core keeps; and the checks of
   debug.* and devtools.* that depend only on the configuration and the caller. Everything that
   needs a renderer goes on to Backend::ExecuteCommand. */
#include <algorithm>
#include <cstring>
#include <utility>

#include "automation_state.h"
#include "broker_internal.h"
#include "core_internal.h"

namespace bdkcore {
namespace {

constexpr int64_t kDefaultWaitMs = 30000;

/* DevTools protocol domains a session may not use: they reach beyond the page it attached to
   (services.md 5.17). */
constexpr const char* kSessionBlockedDomains[] = {"Browser",       "Target",           "SystemInfo",
                                                  "Tethering",     "Storage",          "IO",
                                                  "ServiceWorker", "BackgroundService"};

std::string quoted(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

const JsonValue* member(const JsonValue& v, const char* key, JsonValue::Type type) {
    const JsonValue* f = v.find(key);
    return f && f->type == type ? f : nullptr;
}

std::string text_of(const JsonValue& v, const char* key) {
    const JsonValue* f = member(v, key, JsonValue::Type::kString);
    return f ? f->text : std::string();
}

void reject(BuiltinCall& call, const char* domain, const char* code, const std::string& message) {
    call.control->Reject(domain, code, message);
}

void invalid(BuiltinCall& call, const std::string& message) {
    reject(call, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS, message);
}

/* Answers with the outcome of an ABI function the core called on the caller's behalf. */
void answer_status(BuiltinCall& call, bdk_status_t status, bdk_error_t error) {
    switch (status) {
    case BDK_OK:
        call.control->Resolve("null");
        return;
    case BDK_ERR_FAILED:
        call.control->Reject(str(bdk_error_domain(error)).c_str(),
                             str(bdk_error_code(error)).c_str(), str(bdk_error_message(error)));
        bdk_error_release(error);
        return;
    case BDK_ERR_STALE_HANDLE:
        reject(call, BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "the page is gone");
        return;
    case BDK_ERR_NOT_STARTED:
        reject(call, BDK_DOMAIN_RUNTIME, "notStarted", "the registrations are not frozen yet");
        return;
    default:
        invalid(call, "the runtime refused the arguments");
        return;
    }
}

/* `*` matches any run of characters; everything else matches itself. */
bool glob_match(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (p < pattern.size() && pattern[p] == text[t]) {
            ++p;
            ++t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

std::string detail_json(const Runtime& rt, const Contents& c) {
    ContentsState st;
    backend().State(c, &st);
    int current = -1;
    std::vector<Entry> entries;
    backend().Entries(c, &current, &entries);
    const AutomationState& a = automation_state(rt);
    auto it = a.pages.find(c.handle);
    bool crashed = it != a.pages.end() && it->second.crashed;
    const char* security = "unknown";
    if (st.security == BDK_SECURITY_NONE)
        security = "none";
    else if (st.security == BDK_SECURITY_SECURE)
        security = "secure";
    else if (st.security == BDK_SECURITY_BROKEN)
        security = "broken";
    char numbers[96];
    snprintf(numbers, sizeof numbers, ",\"progress\":%.6g,\"zoom\":%.6g", st.progress, st.zoom);
    return "{\"id\":" + quoted(contents_id(c.handle)) + ",\"url\":" + quoted(st.url) +
           ",\"title\":" + quoted(st.title) +
           ",\"profile\":" + quoted(c.profile ? c.profile->id : std::string()) +
           ",\"visible\":" + (st.visibility == BDK_VISIBILITY_VISIBLE ? "true" : "false") +
           ",\"loading\":" + (st.loading ? "true" : "false") +
           ",\"crashed\":" + (crashed ? "true" : "false") + numbers + ",\"security\":\"" +
           security + "\",\"audible\":" + ((st.audio & BDK_AUDIO_AUDIBLE) ? "true" : "false") +
           ",\"muted\":" + ((st.audio & BDK_AUDIO_MUTED) ? "true" : "false") +
           ",\"focused\":" + (st.focused ? "true" : "false") +
           ",\"canGoBack\":" + (current > 0 ? "true" : "false") + ",\"canGoForward\":" +
           (current >= 0 && current + 1 < static_cast<int>(entries.size()) ? "true" : "false") +
           "}";
}

void list_contents(Runtime& rt, BuiltinCall& call) {
    std::string out = "{\"items\":[";
    bool first = true;
    for (const auto& c : rt.contents) {
        if (c->dead || c->closed)
            continue;
        if (call.profile && c->profile != call.profile)
            continue;
        if (!caller_sees_contents(*call.caller, *c))
            continue;
        out += (first ? "" : ",") + contents_info_json(rt, *c);
        first = false;
    }
    call.control->Resolve(out + "]}");
}

void create_contents(Runtime& rt, BuiltinCall& call, const JsonValue& args) {
    bdk_contents_options_t o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.profile = call.profile->handle;
    std::string url = text_of(args, "url");
    o.initial_url = view(url);
    bdk_contents_t handle = 0;
    bdk_error_t error = 0;
    bdk_status_t status = bdk_contents_create(rt.handle, &o, &handle, &error);
    if (status != BDK_OK) {
        if (status == BDK_ERR_INVALID_ARGUMENT)
            invalid(call, "'" + url + "' is not a URL a page can load");
        else
            answer_status(call, status, error);
        return;
    }
    auto c = contents_of(handle);
    auto shared = runtime_of(rt.handle);
    automation_page_created(shared, c, *call.caller);
    call.control->Resolve(contents_info_json(rt, *c));
}

void navigate(Runtime& rt, const std::shared_ptr<BuiltinCall>& call, const JsonValue& args) {
    const std::string url = text_of(args, "url");
    const bool until_finished = text_of(args, "waitUntil") == "finished";
    auto waiter = std::make_shared<ContentsWaiter>();
    waiter->contents = call->contents->handle;
    auto committed = std::make_shared<std::string>();
    auto error_page = std::make_shared<bool>(false);
    waiter->on_event = [call, until_finished, committed,
                        error_page](uint32_t kind, const std::string& at, const std::string& detail,
                                    uint32_t flags) {
        if (!(flags & BDK_EVENT_FLAG_MAIN_FRAME))
            return false;
        auto done = [&] {
            call->control->Resolve("{\"url\":" + quoted(*committed) +
                                   ",\"errorPage\":" + (*error_page ? "true" : "false") + "}");
            return true;
        };
        if (kind == BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED) {
            *committed = at;
            *error_page = (flags & BDK_EVENT_FLAG_ERROR_PAGE) != 0;
            return until_finished ? false : done();
        }
        if (kind == BDK_CONTENTS_EVENT_LOAD_FINISHED && !committed->empty())
            return done();
        if (kind == BDK_CONTENTS_EVENT_NAVIGATION_FAILED) {
            if (!committed->empty() && !until_finished)
                return false;
            JsonValue d;
            std::string code = "netError";
            if (json_parse(detail, &d) && d.type == JsonValue::Type::kObject &&
                !text_of(d, "code").empty())
                code = text_of(d, "code");
            call->control->Reject(BDK_DOMAIN_NAVIGATION, code.c_str(),
                                  "the navigation to " + at + " failed (" + code + ")");
            return true;
        }
        return false;
    };
    waiter->on_gone = [call] {
        call->control->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "the page closed");
    };
    std::weak_ptr<ContentsWaiter> weak = waiter;
    call->control->OnCancel([weak] {
        if (auto w = weak.lock())
            w->done = true;
    });
    bdk_error_t error = 0;
    bdk_status_t status = bdk_contents_load(call->contents->handle, view(url), &error);
    if (status != BDK_OK) {
        if (status == BDK_ERR_INVALID_ARGUMENT)
            invalid(*call, "'" + url + "' is not a URL a page can load");
        else
            answer_status(*call, status, error);
        return;
    }
    add_contents_waiter(rt, waiter);
}

void go(Runtime&, BuiltinCall& call, int offset) {
    int current = -1;
    std::vector<Entry> entries;
    backend().Entries(*call.contents, &current, &entries);
    int target = current + offset;
    if (current < 0 || target < 0 || target >= static_cast<int>(entries.size())) {
        invalid(call, offset < 0 ? "the page has no earlier entry" : "the page has no later entry");
        return;
    }
    bdk_error_t error = 0;
    answer_status(call, bdk_contents_go_to_offset(call.contents->handle, offset, &error), error);
}

/* contents.waitFor with only `load` and `url`. Returns false for conditions the backend checks. */
bool wait_for(Runtime& rt, const std::shared_ptr<BuiltinCall>& call, const JsonValue& args) {
    if (args.find("selector") || args.find("script") || args.find("networkIdle"))
        return false;
    const std::string load = text_of(args, "load");
    const std::string pattern = text_of(args, "url");
    if (load.empty() && pattern.empty()) {
        invalid(*call, "contents.waitFor needs a condition: load, url, selector, script or "
                       "networkIdle");
        return true;
    }
    int64_t timeout = kDefaultWaitMs;
    if (const JsonValue* t = member(args, "timeoutMs", JsonValue::Type::kNumber)) {
        if (t->number < 0) {
            invalid(*call, "timeoutMs must not be negative");
            return true;
        }
        timeout = static_cast<int64_t>(t->number);
    }
    struct Progress {
        bool committed = false, finished = false;
    };
    auto seen = std::make_shared<Progress>();
    std::weak_ptr<Contents> page = call->contents;
    auto satisfied = [page, load, pattern, seen] {
        auto c = page.lock();
        if (!c)
            return false;
        ContentsState st;
        backend().State(*c, &st);
        if (!pattern.empty() && !glob_match(pattern, st.url))
            return false;
        if (load == "committed")
            return seen->committed;
        if (load == "finished")
            return seen->finished;
        return true;
    };
    /* Already true when the page is not loading and has a committed entry. */
    {
        ContentsState st;
        backend().State(*call->contents, &st);
        int current = -1;
        std::vector<Entry> entries;
        backend().Entries(*call->contents, &current, &entries);
        if (current >= 0 && !st.loading)
            seen->committed = seen->finished = true;
        if (satisfied()) {
            call->control->Resolve("null");
            return true;
        }
    }
    auto waiter = std::make_shared<ContentsWaiter>();
    waiter->contents = call->contents->handle;
    waiter->on_event = [call, seen, satisfied](uint32_t kind, const std::string&,
                                               const std::string&, uint32_t flags) {
        if (!(flags & BDK_EVENT_FLAG_MAIN_FRAME))
            return false;
        if (kind == BDK_CONTENTS_EVENT_NAVIGATION_STARTED &&
            !(flags & BDK_EVENT_FLAG_SAME_DOCUMENT))
            seen->committed = seen->finished = false;
        if (kind == BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED)
            seen->committed = true;
        if (kind == BDK_CONTENTS_EVENT_LOAD_FINISHED ||
            kind == BDK_CONTENTS_EVENT_NAVIGATION_FAILED)
            seen->committed = seen->finished = true;
        if (!satisfied())
            return false;
        call->control->Resolve("null");
        return true;
    };
    waiter->on_gone = [call] {
        call->control->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "the page closed");
    };
    std::weak_ptr<ContentsWaiter> weak = waiter;
    call->control->OnCancel([weak] {
        if (auto w = weak.lock())
            w->done = true;
    });
    add_contents_waiter(rt, waiter);
    backend().PostDelayed(
        [weak, call, timeout] {
            auto w = weak.lock();
            if (!w || w->done)
                return;
            w->done = true;
            call->control->Reject(BDK_DOMAIN_BROKER, BDK_CODE_TIMEOUT,
                                  "the condition did not hold within " + std::to_string(timeout) +
                                      " ms");
        },
        timeout);
    return true;
}

/* ---- scripts.* ---- */

std::vector<std::string> strings_of(const JsonValue& v, const char* key) {
    std::vector<std::string> out;
    if (const JsonValue* a = member(v, key, JsonValue::Type::kArray)) {
        for (const JsonValue& item : a->items)
            out.push_back(item.text);
    }
    return out;
}

std::string strings_json(const std::vector<std::string>& list) {
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i)
        out += (i ? "," : "") + quoted(list[i]);
    return out + "]";
}

std::string user_script_json(const UserScript& u) {
    return "{\"id\":" + quoted(u.id) + ",\"matches\":" + strings_json(u.matches) +
           ",\"excludes\":" + strings_json(u.excludes) + ",\"runAt\":" + quoted(u.run_at) +
           ",\"world\":" + quoted(u.world) + ",\"allFrames\":" + (u.all_frames ? "true" : "false") +
           ",\"javascript\":" + strings_json(u.javascript) + ",\"css\":" + strings_json(u.css) +
           "}";
}

std::string owner_of(const Runtime& rt, const Caller& caller) {
    auto session = session_of(rt, caller);
    return session ? session->caller->name : std::string();
}

void scripts_command(Runtime& rt, BuiltinCall& call, const JsonValue& args) {
    auto& list = automation_state(rt).user_scripts[call.profile->id];
    const std::string owner = owner_of(rt, *call.caller);
    if (call.name == "scripts.list") {
        std::string out = "{\"scripts\":[";
        for (size_t i = 0; i < list.size(); ++i)
            out += (i ? "," : "") + user_script_json(list[i]);
        call.control->Resolve(out + "]}");
        return;
    }
    if (call.name == "scripts.unregister") {
        const std::string id = text_of(args, "id");
        auto it =
            std::find_if(list.begin(), list.end(), [&](const UserScript& u) { return u.id == id; });
        if (it == list.end()) {
            invalid(call, "no user script '" + id + "' in this profile");
            return;
        }
        if (call.caller->kind != CallerKind::kHost && it->owner != owner) {
            reject(call, BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED,
                   "the user script '" + id + "' was registered by another caller");
            return;
        }
        list.erase(it);
        user_scripts_changed(rt, *call.profile);
        call.control->Resolve("null");
        return;
    }
    const JsonValue* s = member(args, "script", JsonValue::Type::kObject);
    UserScript u;
    u.id = text_of(*s, "id");
    u.matches = strings_of(*s, "matches");
    u.excludes = strings_of(*s, "excludes");
    u.javascript = strings_of(*s, "javascript");
    u.css = strings_of(*s, "css");
    u.owner = owner;
    if (std::string run_at = text_of(*s, "runAt"); !run_at.empty())
        u.run_at = run_at;
    u.world = text_of(*s, "world");
    if (u.world.empty())
        u.world = u.id;
    if (const JsonValue* all = member(*s, "allFrames", JsonValue::Type::kBool))
        u.all_frames = all->boolean;
    if (u.id.empty() || u.matches.empty()) {
        invalid(call, "a user script needs an id and at least one match pattern");
        return;
    }
    if (u.javascript.empty() && u.css.empty()) {
        invalid(call, "a user script needs JavaScript or CSS");
        return;
    }
    auto it =
        std::find_if(list.begin(), list.end(), [&](const UserScript& e) { return e.id == u.id; });
    if (it != list.end()) {
        if (call.caller->kind != CallerKind::kHost && it->owner != owner) {
            reject(call, BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED,
                   "the user script '" + u.id + "' was registered by another caller");
            return;
        }
        *it = std::move(u);
    } else {
        list.push_back(std::move(u));
    }
    user_scripts_changed(rt, *call.profile);
    call.control->Resolve("null");
}

bool needs_page(BuiltinCall& call) {
    if (call.contents)
        return true;
    invalid(call, "'" + call.name + "' needs a page: name one in the invoke options");
    return false;
}

} // namespace

bool core_execute_command(Runtime& rt, const std::shared_ptr<BuiltinCall>& call,
                          const JsonValue& args) {
    const std::string& name = call->name;
    if (name.compare(0, 8, "scripts.") == 0) {
        scripts_command(rt, *call, args);
        return true;
    }
    if (name.compare(0, 6, "debug.") == 0) {
        if (!automation_state(rt).protocol) {
            reject(*call, BDK_DOMAIN_DEVTOOLS, BDK_CODE_PROTOCOL_DISABLED,
                   "[devtools] protocol is false in bdk.toml");
            return true;
        }
        if (name == "debug.attach" && !needs_page(*call))
            return true;
        if (name == "debug.send" && call->caller->kind == CallerKind::kSession) {
            const std::string method = text_of(args, "method");
            const std::string domain = method.substr(0, method.find('.'));
            for (const char* blocked : kSessionBlockedDomains) {
                if (domain == blocked) {
                    reject(*call, BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED,
                           "the protocol domain " + domain +
                               " reaches beyond one page and is refused to sessions");
                    return true;
                }
            }
        }
        return false;
    }
    if (name.compare(0, 9, "devtools.") == 0) {
        if (!needs_page(*call))
            return true;
        if (name == "devtools.open" && automation_state(rt).frontend_disabled) {
            reject(*call, BDK_DOMAIN_DEVTOOLS, BDK_CODE_DISABLED,
                   "[devtools] frontend is \"disabled\" in bdk.toml");
            return true;
        }
        return false;
    }
    if (name.compare(0, 9, "contents.") != 0)
        return false;
    if (name == "contents.list") {
        list_contents(rt, *call);
        return true;
    }
    if (name == "contents.create") {
        create_contents(rt, *call, args);
        return true;
    }
    if (!needs_page(*call))
        return true;
    bdk_contents_t page = call->contents->handle;
    bdk_error_t error = 0;
    if (name == "contents.get") {
        call->control->Resolve(detail_json(rt, *call->contents));
    } else if (name == "contents.close") {
        answer_status(*call, bdk_contents_close(page, &error), error);
    } else if (name == "contents.navigate") {
        navigate(rt, call, args);
    } else if (name == "contents.reload") {
        const JsonValue* bypass = member(args, "bypassCache", JsonValue::Type::kBool);
        answer_status(*call, bdk_contents_reload(page, bypass && bypass->boolean, &error), error);
    } else if (name == "contents.stop") {
        answer_status(*call, bdk_contents_stop(page, &error), error);
    } else if (name == "contents.goBack") {
        go(rt, *call, -1);
    } else if (name == "contents.goForward") {
        go(rt, *call, 1);
    } else if (name == "contents.waitFor") {
        return wait_for(rt, call, args);
    } else if (name == "contents.setMuted") {
        const JsonValue* muted = member(args, "muted", JsonValue::Type::kBool);
        answer_status(*call, bdk_contents_set_muted(page, muted && muted->boolean, &error), error);
    } else if (name == "contents.setZoom") {
        const JsonValue* zoom = member(args, "zoom", JsonValue::Type::kNumber);
        bdk_status_t status = bdk_contents_set_zoom(page, zoom ? zoom->number : 1.0, &error);
        if (status == BDK_ERR_INVALID_ARGUMENT)
            invalid(*call, "zoom is out of range");
        else
            answer_status(*call, status, error);
    } else if (name == "contents.requestAttention") {
        const std::string owner = owner_of(rt, *call->caller);
        auto shared = runtime_of(rt.handle);
        emit_builtin_event_for(shared, "contents.attentionRequested",
                               call->contents->profile ? call->contents->profile->handle : 0, page,
                               "{\"contents\":" + quoted(contents_id(page)) + ",\"session\":" +
                                   (owner.empty() ? std::string("null") : quoted(owner)) + "}");
        call->control->Resolve("null");
    } else {
        return false; /* needs the renderer: the backend's */
    }
    return true;
}

} // namespace bdkcore
