/* Broker: the command registry, the checks every caller goes through, cancellation, progress and
   the teardown of a caller's calls. See broker_internal.h. */
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "automation_internal.h"
#include "bdk_command_table.inc"
#include "broker_internal.h"

namespace bdkcore {
namespace {

using table::CommandDef;

/* ---- The registry ---- */

const CommandDef* find_builtin(uint32_t id) {
    const CommandDef* end = table::kCommands + table::kCommandCount;
    const CommandDef* it =
        std::lower_bound(table::kCommands, end, id,
                         [](const CommandDef& def, uint32_t wanted) { return def.id < wanted; });
    return it != end && it->id == id ? it : nullptr;
}

const CommandDef* find_builtin(const std::string& name) {
    static const auto* by_name = [] {
        auto* m = new std::unordered_map<std::string, const CommandDef*>;
        for (size_t i = 0; i < table::kCommandCount; ++i)
            (*m)[table::kCommands[i].name] = &table::kCommands[i];
        return m;
    }();
    auto it = by_name->find(name);
    return it == by_name->end() ? nullptr : it->second;
}

/* ---- Callers ---- */

uint8_t caller_bit(CallerKind kind) {
    switch (kind) {
    case CallerKind::kPage:
        return table::kCallerPage;
    case CallerKind::kSession:
        return table::kCallerSession;
    case CallerKind::kHost:
        break;
    }
    return table::kCallerSwift;
}

const char* kind_name(CallerKind kind) {
    switch (kind) {
    case CallerKind::kPage:
        return "page";
    case CallerKind::kSession:
        return "session";
    case CallerKind::kHost:
        break;
    }
    return "swift";
}

bool matches_pattern(const std::string& capability, const std::string& pattern) {
    if (pattern.size() >= 2 && pattern.compare(pattern.size() - 2, 2, ".*") == 0)
        return capability.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
    return capability == pattern;
}

bool on_list(const char* const* list, const std::string& capability) {
    for (; *list; ++list) {
        if (matches_pattern(capability, *list))
            return true;
    }
    return false;
}

std::string caller_json(const Caller& c) {
    if (c.kind == CallerKind::kHost)
        return "{\"caller\":\"host\"}";
    std::string out = std::string("{\"caller\":\"") + kind_name(c.kind) + "\",\"name\":\"" +
                      json_escape(c.name) + "\"";
    if (c.kind == CallerKind::kPage) {
        if (auto p = get<Profile>(c.profile, T_PROFILE))
            out += ",\"profile\":\"" + json_escape(p->id) + "\"";
    }
    return out + "}";
}

/* What runtime.caller answers (the CallerInfo type of interface/runtime.json). */
std::string caller_info(const Caller& c) {
    std::string out = std::string("{\"kind\":\"") + kind_name(c.kind) + "\",\"name\":\"" +
                      json_escape(c.kind == CallerKind::kHost ? "swift" : c.name) + "\"";
    if (c.kind == CallerKind::kPage) {
        if (auto p = get<Profile>(c.profile, T_PROFILE))
            out += ",\"profile\":\"" + json_escape(p->id) + "\"";
    }
    out += ",\"capabilities\":[";
    for (size_t i = 0; i < c.capabilities.size(); ++i)
        out += (i ? ",\"" : "\"") + json_escape(c.capabilities[i]) + "\"";
    out += ']';
    if (c.kind == CallerKind::kPage && !c.internal_scheme.empty())
        out += ",\"internalScheme\":\"" + json_escape(c.internal_scheme) + "\"";
    return out + "}";
}

/* ---- One call ---- */

struct Pending {
    std::shared_ptr<Runtime> rt;
    std::shared_ptr<Call> call;
    std::shared_ptr<Caller> caller;
    CommandRef ref;
    std::string input;
    bdk_invoke_options_t options{};
    bool gesture = false;
    bool confirmed = false; /* BuiltinCall::confirmed */
    /* Set by the lookup. */
    const CommandDef* def = nullptr;
    Command app{};
    std::string name;
    /* Set by the scope check. */
    std::shared_ptr<Profile> profile;
    std::shared_ptr<Contents> contents;
};

void refuse(const std::shared_ptr<Pending>& p, const char* domain, const char* code,
            const std::string& message) {
    finish_call(p->call, domain, code, message, "");
}

bool is_member(const std::set<uint64_t>& set, uint64_t handle) {
    return set.count(handle) != 0;
}

/* Step 3: what the caller may address. Fills p->profile and p->contents. Returns false after
   refusing the call. */
bool check_scope(const std::shared_ptr<Pending>& p) {
    const Caller& caller = *p->caller;
    const bdk_invoke_options_t& o = p->options;
    uint8_t mode = p->def ? p->def->profile : table::kProfileOptional;

    bdk_profile_t profile = o.profile;
    switch (caller.kind) {
    case CallerKind::kHost:
        break;
    case CallerKind::kPage:
        /* A page is always in one profile, cannot name another, and addresses no other page. */
        if (profile && profile != caller.profile) {
            refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_OUT_OF_SCOPE,
                   "a page cannot address a profile other than its own");
            return false;
        }
        profile = caller.profile;
        if (o.contents) {
            refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_OUT_OF_SCOPE, "a page cannot address pages");
            return false;
        }
        break;
    case CallerKind::kSession:
        if (profile && !caller.scope.all_profiles && !is_member(caller.scope.profiles, profile)) {
            refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_OUT_OF_SCOPE,
                   "the profile is outside the session's scope");
            return false;
        }
        if (!profile && !caller.scope.all_profiles && caller.scope.profiles.size() == 1)
            profile = *caller.scope.profiles.begin();
        if (o.contents && !caller.scope.all_contents &&
            !is_member(caller.scope.contents, o.contents)) {
            refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_OUT_OF_SCOPE,
                   "the page is outside the session's scope");
            return false;
        }
        /* A page is in scope only in a profile that is: "all pages" means all pages of those. */
        if (o.contents && !caller.scope.all_profiles) {
            auto page = contents_of(o.contents);
            if (page &&
                (!page->profile || !is_member(caller.scope.profiles, page->profile->handle))) {
                refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_OUT_OF_SCOPE,
                       "the page is in a profile outside the session's scope");
                return false;
            }
        }
        break;
    }

    if (mode != table::kProfileNone && profile) {
        p->profile = get<Profile>(profile, T_PROFILE);
        if (!p->profile) {
            refuse(p, BDK_DOMAIN_PROFILE, BDK_CODE_NOT_FOUND, "the profile is no longer open");
            return false;
        }
    }
    if (o.contents) {
        p->contents = contents_of(o.contents);
        if (!p->contents) {
            refuse(p, BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "the page is gone");
            return false;
        }
    }
    /* A call that names a page and no profile addresses the page's profile (translation.state). */
    if (mode != table::kProfileNone && !p->profile && p->contents && p->contents->profile &&
        p->contents->profile->open) {
        bdk_profile_t own = p->contents->profile->handle;
        if (caller.kind == CallerKind::kSession && !caller.scope.all_profiles &&
            !is_member(caller.scope.profiles, own)) {
            refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_OUT_OF_SCOPE,
                   "the page's profile is outside the session's scope");
            return false;
        }
        p->profile = p->contents->profile;
    }
    if (mode == table::kProfileRequired && !p->profile) {
        refuse(p, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
               "'" + p->name + "' needs a profile: name one in the invoke options");
        return false;
    }
    return true;
}

/* Step 5, then step 6. */
void validate_and_run(const std::shared_ptr<Pending>& p) {
    const std::string& input = p->input;
    if (input.size() > kMaxPayloadBytes) {
        refuse(p, BDK_DOMAIN_BROKER, BDK_CODE_PAYLOAD_TOO_LARGE,
               "the arguments are larger than " + std::to_string(kMaxPayloadBytes) + " bytes");
        return;
    }
    if (!valid_json(input)) {
        refuse(p, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
               "the arguments are not valid JSON");
        return;
    }
    JsonValue parsed;
    if (p->def) {
        std::string problem = json_parse(input, &parsed) ? check_arguments(*p->def, parsed)
                                                         : "arguments: could not be read";
        if (!problem.empty()) {
            refuse(p, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                   "invalid arguments for '" + p->name + "': " + problem);
            return;
        }
    }
    const bool settings_call = p->def && p->name.compare(0, 9, "settings.") == 0;
    std::string not_exposed;
    if (settings_call && p->caller->kind != CallerKind::kHost &&
        !app_settings_exposed(*p->rt, parsed, &not_exposed)) {
        refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED, not_exposed);
        return;
    }

    /* Step 6: the service. Everything before this ran without it. */
    if (!p->def) {
        auto cc = std::make_shared<CallCtx>();
        cc->call = p->call;
        cc->caller = caller_json(*p->caller);
        cc->profile = p->profile;
        bdk_context_t ch = table_make(T_CONTEXT, cc);
        p->app.cb(p->app.ctx, ch, view(input));
        return;
    }
    if (p->name == "runtime.caller") {
        resolve_call(p->call, caller_info(*p->caller));
        return;
    }
    auto call = std::make_shared<BuiltinCall>();
    call->def = p->def;
    call->id = p->def->id;
    call->name = p->name;
    call->arguments = input;
    call->profile = p->profile;
    call->contents = p->contents;
    call->caller = p->caller;
    call->confirmed = p->confirmed;
    call->control =
        std::make_shared<CommandControl>(p->call, (p->def->flags & table::kProgress) != 0);
    if (p->name == "runtime.info")
        call->control->AppendMembers(web_apps_runtime_info(*p->rt));
    /* Weak: the control holds the call, so a strong reference here would keep every finished call
       alive. The BuiltinCall the backend holds keeps the control alive while it works. */
    std::weak_ptr<CommandControl> control = call->control;
    p->call->on_stop = [control] {
        if (auto c = control.lock())
            c->Stop();
    };
    if (settings_call && app_settings_execute(*p->rt, call, parsed))
        return;
    if (web_apps_refuse(*p->rt, call))
        return;
    if (core_execute_command(*p->rt, call, parsed))
        return;
    backend().ExecuteCommand(*p->rt, call);
}

/* Step 4, then 5 and 6. Authentication is asked of the operating system whoever calls, and is
   asynchronous; the call carries on when it succeeds. */
void check_requirements(const std::shared_ptr<Pending>& p) {
    bool need_authentication = false;
    if (p->def) {
        uint8_t flags = p->def->flags;
        if (p->caller->kind == CallerKind::kPage && (flags & table::kGesture) != 0 && !p->gesture) {
            refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_GESTURE_REQUIRED,
                   "'" + p->name + "' needs a user gesture in the page");
            return;
        }
        need_authentication = (flags & table::kAuthentication) != 0;
        if (!need_authentication && p->def->authenticate_when && valid_json(p->input) &&
            p->input.size() <= kMaxPayloadBytes) {
            JsonValue parsed;
            if (json_parse(p->input, &parsed) && parsed.type == JsonValue::Type::kObject) {
                const JsonValue* field = parsed.find(p->def->authenticate_when);
                need_authentication = field && field->type != JsonValue::Type::kNull;
            }
        }
    }
    if (!need_authentication) {
        validate_and_run(p);
        return;
    }
    backend().Authenticate(*p->rt, "authenticate to run " + p->name, [p](bool authenticated) {
        if (p->call->done)
            return; /* cancelled while the user was being asked */
        if (!authenticated) {
            refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_REAUTHENTICATION_FAILED,
                   "authentication was not completed");
            return;
        }
        validate_and_run(p);
    });
}

void dispatch(const std::shared_ptr<Pending>& p) {
    if (p->call->done)
        return; /* cancelled before it started */
    if (p->rt->shutting) {
        finish_call(p->call, BDK_DOMAIN_BROKER, BDK_CODE_CANCELLED, "the runtime shut down", "");
        return;
    }
    /* Lookup. */
    bool named = !p->ref.name.empty();
    p->def = named ? find_builtin(p->ref.name) : find_builtin(p->ref.id);
    if (p->def) {
        p->name = p->def->name;
    } else if (named) {
        auto it = p->rt->commands.find(p->ref.name);
        if (it != p->rt->commands.end()) {
            p->app = it->second;
            p->name = p->ref.name;
        }
    }
    if (p->name.empty()) {
        refuse(p, BDK_DOMAIN_BROKER, BDK_CODE_UNKNOWN_COMMAND,
               named ? "unknown command '" + p->ref.name + "'"
                     : "unknown command id " + std::to_string(p->ref.id));
        return;
    }
    const Caller& caller = *p->caller;

    /* 1. Caller identity. */
    if (caller.revoked) {
        refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED, "the caller is no longer bound");
        return;
    }
    /* 2. Capability. The host has full authority. An application command needs the capability named
       like itself. A caller without the capability learns nothing about the arguments. */
    if (caller.kind != CallerKind::kHost) {
        if (p->def && (p->def->callers & caller_bit(caller.kind)) == 0) {
            refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED,
                   std::string("'") + p->name + "' is not available to a " +
                       kind_name(caller.kind));
            return;
        }
        const char* needed = p->def ? p->def->capability : p->name.c_str();
        if (needed && !capability_granted(caller, needed)) {
            refuse(p, BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED,
                   std::string("the caller lacks the capability '") + needed + "'");
            return;
        }
    }
    /* 3. Scope. */
    if (!check_scope(p))
        return;
    /* 4 to 6. */
    check_requirements(p);
}

void cancel_with(const std::shared_ptr<Call>& c, const char* message) {
    if (c->done)
        return;
    c->cancelled = true;
    run_on_stop(c);
    finish_call(c, BDK_DOMAIN_BROKER, BDK_CODE_CANCELLED, message, "");
}

} // namespace

/* ---- Caller API ---- */

std::shared_ptr<Caller> host_caller() {
    static const auto* host = new std::shared_ptr<Caller>(std::make_shared<Caller>());
    return *host;
}

std::string grant_error(CallerKind kind, const std::vector<std::string>& capabilities) {
    for (const std::string& c : capabilities) {
        if (c.empty())
            return "a capability name is empty";
        size_t star = c.find('*');
        if (star != std::string::npos) {
            bool app_prefix = c.size() > 6 && c.compare(0, 4, "app.") == 0 &&
                              c.compare(c.size() - 2, 2, ".*") == 0 && star == c.size() - 1;
            if (!app_prefix)
                return "'" + c + "' is a wildcard; only app.<prefix>.* is allowed";
        }
        if (kind == CallerKind::kPage && on_list(table::kNeverForPages, c))
            return "'" + c + "' is never granted to a page";
        if (kind == CallerKind::kSession && on_list(table::kNeverForSessions, c))
            return "'" + c + "' is never granted to a session";
    }
    return std::string();
}

bool capability_granted(const Caller& caller, const std::string& capability) {
    if (caller.kind == CallerKind::kHost)
        return true;
    for (const std::string& g : caller.capabilities) {
        if (matches_pattern(capability, g))
            return true;
    }
    return false;
}

bdk_invoke_options_t read_invoke_options(const bdk_invoke_options_t* o) {
    bdk_invoke_options_t full;
    memset(&full, 0, sizeof full);
    if (o)
        memcpy(&full, o, o->struct_size < sizeof full ? o->struct_size : sizeof full);
    return full;
}

bool invoke_options_ok(const bdk_invoke_options_t* o) {
    if (!o)
        return true;
    if (o->struct_size < sizeof(size_t))
        return false;
    const bdk_invoke_options_t full = read_invoke_options(o);
    if (full.profile && !get<Profile>(full.profile, T_PROFILE))
        return false;
    if (full.contents && !get<Contents>(full.contents, T_CONTENTS))
        return false;
    return true;
}

namespace {

std::shared_ptr<Pending> submit(const std::shared_ptr<Runtime>& rt,
                                const std::shared_ptr<Caller>& caller, const CommandRef& command,
                                const std::string& arguments_json,
                                const bdk_invoke_options_t* options, bool gesture, bool confirmed,
                                bdk_result_cb cb, bdk_progress_cb prog, void* ctx,
                                bdk_release_cb rel) {
    auto call = new_call(rt, cb, prog, ctx, rel);
    auto p = std::make_shared<Pending>();
    p->rt = rt;
    p->call = call;
    p->caller = caller;
    p->ref = command;
    p->input = arguments_json;
    p->gesture = gesture;
    p->confirmed = confirmed;
    p->options = read_invoke_options(options);
    if (caller->kind != CallerKind::kHost) {
        auto& calls = caller->calls;
        calls.erase(std::remove_if(calls.begin(), calls.end(),
                                   [](const std::weak_ptr<Call>& w) { return w.expired(); }),
                    calls.end());
        calls.push_back(call);
    }
    post([p] { dispatch(p); });
    return p;
}

} // namespace

bdk_call_t broker_submit(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Caller>& caller,
                         const CommandRef& command, const std::string& arguments_json,
                         const bdk_invoke_options_t* options, bool gesture, bdk_result_cb cb,
                         bdk_progress_cb prog, void* ctx, bdk_release_cb rel) {
    return submit(rt, caller, command, arguments_json, options, gesture, false, cb, prog, ctx, rel)
        ->call->handle;
}

bdk_call_t broker_invoke_confirmed(bdk_runtime_t h, const std::string& name, bdk_string_t args,
                                   const bdk_invoke_options_t* opts, bdk_native_handle_t gesture,
                                   bdk_result_cb cb, void* ctx, bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb || !string_ok(args))
        return 0;
    auto rt = runtime_of(h);
    if (!rt || !invoke_options_ok(opts))
        return 0;
    if (gesture.kind != BDK_NATIVE_NSEVENT) {
        auto call = new_call(rt, cb, nullptr, ctx, rel);
        finish_call(call, BDK_DOMAIN_UI, BDK_CODE_GESTURE_REQUIRED,
                    "'" + name + "' needs the event that confirmed it", "");
        return call->handle;
    }
    CommandRef ref;
    ref.name = name;
    return submit(rt, host_caller(), ref, str(args), opts, false, true, cb, nullptr, ctx, rel)
        ->call->handle;
}

bdk_call_t broker_invoke_abi(bdk_runtime_t h, const std::string* name, uint32_t id,
                             bdk_string_t args, const bdk_invoke_options_t* opts, bdk_result_cb cb,
                             bdk_progress_cb prog, void* ctx, bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb || !string_ok(args))
        return 0;
    auto rt = runtime_of(h);
    if (!rt || !invoke_options_ok(opts))
        return 0;
    CommandRef ref;
    if (name)
        ref.name = *name;
    else
        ref.id = id;
    return broker_submit(rt, host_caller(), ref, str(args), opts, false, cb, prog, ctx, rel);
}

namespace {

uint32_t builtin_event_id(const std::string& name) {
    for (size_t i = 0; i < table::kEventCount; ++i) {
        if (name == table::kEvents[i].name)
            return table::kEvents[i].id;
    }
    return 0;
}

/* to: nullptr delivers to the host and to every session in scope; the host's caller delivers to
   the host only; a session's caller to that session only. */
bool emit_event(const std::shared_ptr<Runtime>& rt, const Caller* to, const std::string& name,
                bdk_profile_t profile, bdk_contents_t contents, const std::string& payload_json) {
    uint32_t id = builtin_event_id(name);
    if (id == 0)
        return false;
    std::shared_ptr<Caller> only;
    if (to && to->kind != CallerKind::kHost)
        only = std::const_pointer_cast<Caller>(to->shared_from_this());
    const bool host = !to || to->kind == CallerKind::kHost;
    const bool sessions = !to || only;
    post([rt, name, id, profile, contents, payload_json, host, sessions, only] {
        if (host) {
            bool hidden_from_pages = false;
            if (name == "settings.changed") {
                JsonValue parsed;
                std::string not_exposed;
                hidden_from_pages = !json_parse(payload_json, &parsed) ||
                                    !app_settings_exposed(*rt, parsed, &not_exposed);
            }
            auto subs = rt->subs;
            for (auto& s : subs) {
                if (!s->active || !s->ecb)
                    continue;
                if (s->page && hidden_from_pages)
                    continue;
                const bool everywhere = profile == 0 && s->page; /* application-wide, to pages */
                if ((s->profile != 0 && s->profile != profile && !everywhere) ||
                    (s->contents != 0 && s->contents != contents))
                    continue;
                if ((!s->event_name.empty() && s->event_name == name) ||
                    (s->event_id != 0 && s->event_id == id))
                    s->ecb(s->ctx, id, view(name), view(payload_json));
            }
        }
        if (sessions)
            automation_deliver_event(rt, name, id, profile, contents, payload_json, only.get());
    });
    return true;
}

} // namespace

bool emit_builtin_event(const std::shared_ptr<Runtime>& rt, const std::string& name,
                        bdk_profile_t profile, const std::string& payload_json) {
    return emit_event(rt, nullptr, name, profile, 0, payload_json);
}

bool emit_builtin_event_for(const std::shared_ptr<Runtime>& rt, const std::string& name,
                            bdk_profile_t profile, bdk_contents_t contents,
                            const std::string& payload_json) {
    return emit_event(rt, nullptr, name, profile, contents, payload_json);
}

bool emit_builtin_event_to(const std::shared_ptr<Runtime>& rt,
                           const std::shared_ptr<Caller>& caller, const std::string& name,
                           bdk_profile_t profile, bdk_contents_t contents,
                           const std::string& payload_json) {
    return emit_event(rt, caller ? caller.get() : host_caller().get(), name, profile, contents,
                      payload_json);
}

void broker_revoke_caller(const std::shared_ptr<Caller>& caller) {
    caller->revoked = true;
    auto calls = std::move(caller->calls);
    caller->calls.clear();
    for (auto& w : calls) {
        if (auto c = w.lock())
            cancel_with(c, "the caller was revoked");
    }
    auto subs = std::move(caller->subs);
    caller->subs.clear();
    for (auto& w : subs) {
        if (auto s = w.lock())
            deactivate(s);
    }
}

bdk_subscription_t broker_subscribe_as(const std::shared_ptr<Runtime>& rt,
                                       const std::shared_ptr<Caller>& caller,
                                       const std::string& event, bdk_event_cb cb, void* ctx,
                                       bdk_release_cb rel, std::string* domain, std::string* code,
                                       std::string* message) {
    auto refuse_with = [&](const char* d, const char* c, const std::string& m) {
        *domain = d;
        *code = c;
        *message = m;
        return bdk_subscription_t{0};
    };
    if (caller->kind != CallerKind::kPage)
        return refuse_with(BDK_DOMAIN_BROKER, BDK_CODE_NOT_IMPLEMENTED,
                           "only internal pages subscribe through the broker before M7");
    if (caller->revoked)
        return refuse_with(BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED, "the caller was revoked");
    std::string capability;
    uint32_t id = 0;
    for (size_t i = 0; i < table::kEventCount; ++i) {
        if (event == table::kEvents[i].name) {
            capability = table::kEvents[i].capability;
            id = table::kEvents[i].id;
        }
    }
    if (id == 0) {
        auto declared = rt->events.find(event);
        if (declared == rt->events.end())
            return refuse_with(BDK_DOMAIN_BROKER, "unknownEvent",
                               "there is no event '" + event + "'");
        capability = event;
        id = declared->second;
    }
    if (!capability_granted(*caller, capability))
        return refuse_with(BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED,
                           "'" + event + "' needs the capability '" + capability + "'");
    auto s = std::make_shared<Sub>();
    s->ecb = cb;
    s->ctx = ctx;
    s->rel = rel;
    s->event_id = id;
    s->event_name = event;
    s->profile = caller->profile; /* a page sees its own profile's and application-wide events */
    s->page = true;
    s->handle = table_make(T_SUB, s);
    rt->subs.push_back(s);
    caller->subs.push_back(s);
    return s->handle;
}

void resolve_call(const std::shared_ptr<Call>& call, const std::string& result_json) {
    if (call->done)
        return;
    if (result_json.size() > kMaxPayloadBytes) {
        finish_call(call, BDK_DOMAIN_BROKER, BDK_CODE_PAYLOAD_TOO_LARGE,
                    "the result is larger than " + std::to_string(kMaxPayloadBytes) + " bytes", "");
        return;
    }
    if (!valid_json(result_json)) {
        finish_call(call, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                    "the command resolved with invalid JSON", "");
        return;
    }
    finish_call(call, nullptr, nullptr, "", result_json);
}

/* ---- CommandControl ---- */

Caller::Caller() = default;
Caller::~Caller() = default;

CommandControl::CommandControl(std::shared_ptr<Call> call, bool progress)
    : call_(std::move(call)), progress_(progress) {}

CommandControl::~CommandControl() = default;

void CommandControl::Progress(double fraction, const std::string& message) {
    if (!progress_ || call_->done || call_->cancelled || !call_->prog || std::isnan(fraction))
        return;
    double f = std::min(1.0, std::max(0.0, fraction));
    auto call = call_;
    post([call, f, message] {
        if (!call->delivered && !call->cancelled)
            call->prog(call->ctx, f, view(message));
    });
}

void CommandControl::Resolve(const std::string& result_json) {
    if (!append_.empty() && result_json.size() >= 2 && result_json.front() == '{' &&
        result_json.back() == '}') {
        const bool empty = result_json.find_first_not_of(" \t\r\n", 1) == result_json.size() - 1;
        resolve_call(call_, result_json.substr(0, result_json.size() - 1) + (empty ? "" : ",") +
                                append_ + "}");
        return;
    }
    resolve_call(call_, result_json);
}

void CommandControl::ResolveBinary(const std::string& result_json, std::string bytes) {
    if (call_->done)
        return;
    if (result_json.size() > kMaxPayloadBytes || !valid_json(result_json)) {
        resolve_call(call_, result_json); /* answers payloadTooLarge or the invalid JSON */
        return;
    }
    finish_call_binary(call_, result_json, std::move(bytes));
}

void CommandControl::Reject(const char* domain, const char* code, const std::string& message) {
    finish_call(call_, domain && *domain ? domain : BDK_DOMAIN_BROKER, code, message, "");
}

bool CommandControl::cancelled() const {
    return call_->cancelled;
}

void CommandControl::OnCancel(std::function<void()> stop) {
    if (call_->cancelled) {
        stop();
        return;
    }
    stop_ = std::move(stop);
}

void CommandControl::AppendMembers(std::string members) {
    append_ = std::move(members);
}

void CommandControl::Stop() {
    if (stop_) {
        auto stop = std::move(stop_);
        stop_ = nullptr;
        stop();
    }
}

/* ---- Backend defaults ---- */

void Backend::ExecuteCommand(Runtime&, const std::shared_ptr<BuiltinCall>& call) {
    call->control->Reject(BDK_DOMAIN_BROKER, BDK_CODE_NOT_IMPLEMENTED,
                          "'" + call->name + "' is not implemented by this runtime yet");
}

std::string Backend::LoadAppSettings(Runtime&, const Profile*) {
    return std::string();
}

void Backend::SaveAppSettings(Runtime&, const Profile*, const std::string&) {}

void Backend::Authenticate(Runtime&, const std::string&, std::function<void(bool)> done) {
    post([done = std::move(done)] { done(false); });
}

} // namespace bdkcore
