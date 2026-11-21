/* Runtime core: profiles, WebContents, views, windows and UI requests. The checks and the
   bookkeeping are here; what a page does is the backend's. */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "automation_internal.h"
#include "broker_internal.h"

using namespace bdkcore;

namespace bdkcore {

std::shared_ptr<Contents> contents_of(bdk_contents_t h) {
    return get<Contents>(h, T_CONTENTS);
}

void close_profile(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Profile>& p) {
    for (auto& c : std::vector<std::shared_ptr<Contents>>(rt->contents)) {
        if (c->profile == p)
            destroy_contents(rt, c);
    }
    p->open = false;
    table_free(p->handle);
    p->handle = 0;
    backend().CloseProfile(*rt, *p);
    app_settings_forget(*rt, p->id);
    if (p->ephemeral) {
        rt->profiles.erase(std::remove(rt->profiles.begin(), rt->profiles.end(), p),
                           rt->profiles.end());
    }
    emit_runtime_event(rt, BDK_RUNTIME_EVENT_PROFILE_CLOSED,
                       "{\"id\":\"" + json_escape(p->id) + "\"}");
}

void destroy_contents(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c) {
    withdraw_contents_requests(rt, c->handle);
    automation_forget_contents(rt, c->handle);
    c->dead = true;
    for (auto& s : c->subs)
        deactivate(s);
    release_later(c->key_ctx, c->key_rel);
    c->key_rel = nullptr;
    c->key_reserve = nullptr;
    c->key_unhandled = nullptr;
    table_free(c->handle);
    rt->contents.erase(std::remove(rt->contents.begin(), rt->contents.end(), c),
                       rt->contents.end());
    backend().DestroyContents(*rt, *c);
}

void emit_contents_event_now(const std::shared_ptr<Contents>& c, uint32_t kind,
                             const std::string& url, const std::string& detail, int32_t net_error,
                             uint32_t flags) {
    if (c->dead)
        return;
    automation_contents_event(c, kind, url, detail, flags);
    bdk_contents_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev;
    ev.kind = kind;
    ev.contents = c->handle;
    ev.url = view(url);
    ev.detail_json = view(detail);
    ev.net_error = net_error;
    ev.flags = flags;
    auto subs = c->subs;
    for (auto& s : subs) {
        if (s->active && s->ccb)
            s->ccb(s->ctx, &ev);
    }
    c->subs.erase(std::remove_if(c->subs.begin(), c->subs.end(),
                                 [](const std::shared_ptr<Sub>& s) { return !s->active; }),
                  c->subs.end());
}

void emit_contents_event(const std::shared_ptr<Contents>& c, uint32_t kind, const std::string& url,
                         const std::string& detail, int32_t net_error, uint32_t flags) {
    post([c, kind, url, detail, net_error, flags] {
        emit_contents_event_now(c, kind, url, detail, net_error, flags);
    });
}

bool reserve_key(Contents& c, bdk_native_handle_t key_event) {
    return c.key_reserve && c.key_reserve(c.key_ctx, key_event) != 0;
}

void report_unhandled_key(Contents& c, bdk_native_handle_t key_event) {
    if (c.key_unhandled)
        c.key_unhandled(c.key_ctx, key_event);
}

void release_withdrawn(Request& r) {
    release_later(r.wctx, r.wrel);
    r.wctx = nullptr;
    r.wrel = nullptr;
}

} // namespace bdkcore

namespace {

std::string profile_json(const Profile& p) {
    return "{\"id\":\"" + json_escape(p.id) + "\",\"name\":\"" + json_escape(p.name) +
           "\",\"kind\":\"" + (p.ephemeral ? "ephemeral" : "persistent") + "\",\"parent\":null}";
}

bool name_taken(const Runtime& rt, const std::string& name, const Profile* except) {
    for (auto& p : rt.profiles) {
        if (!p->ephemeral && p.get() != except && p->name == name)
            return true;
    }
    return false;
}

std::shared_ptr<Profile> find_profile(const Runtime& rt, const std::string& id) {
    for (auto& p : rt.profiles) {
        if (p->id == id)
            return p;
    }
    return nullptr;
}

/* Common prologue of a synchronous contents call. */
std::shared_ptr<Contents> contents_for(bdk_contents_t h, bdk_error_t* out_error,
                                       bdk_status_t* status) {
    clear_out(out_error);
    *status = check_thread();
    if (*status != BDK_OK)
        return nullptr;
    auto c = get<Contents>(h, T_CONTENTS);
    if (!c)
        *status = BDK_ERR_STALE_HANDLE;
    return c;
}

std::shared_ptr<Contents> open_contents_for(bdk_contents_t h, bdk_error_t* out_error,
                                            bdk_status_t* status) {
    auto c = contents_for(h, out_error, status);
    if (c && c->closed) {
        *status = fail(out_error, BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "the contents is closed");
        return nullptr;
    }
    return c;
}

bool plausible_url(const std::string& u) {
    return u.find(':') != std::string::npos && u.find_first_of(" \t\r\n") == std::string::npos;
}

bdk_status_t nav_to_index(const std::shared_ptr<Contents>& c, int64_t index, bdk_error_t* err) {
    int current = -1;
    std::vector<Entry> entries;
    backend().Entries(*c, &current, &entries);
    if (index < 0 || index >= static_cast<int64_t>(entries.size()))
        return BDK_ERR_INVALID_ARGUMENT;
    return backend().GoToIndex(c, static_cast<int>(index), err);
}

std::shared_ptr<Request> request_of(bdk_request_t h, std::shared_ptr<Runtime>* rt) {
    auto r = get<Request>(h, T_REQUEST);
    if (r)
        *rt = current_runtime();
    return r;
}

} // namespace

extern "C" {

/* ---- bdk_profile.h ---- */

BDK_EXPORT bdk_call_t bdk_profiles_list(bdk_runtime_t h, bdk_result_cb cb, void* ctx,
                                        bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb)
        return 0;
    auto rt = runtime_of(h);
    if (!rt)
        return 0;
    auto call = new_call(rt, cb, nullptr, ctx, rel);
    post([rt, call] {
        std::string json = "[";
        bool first = true;
        for (auto& p : rt->profiles) {
            if (p->ephemeral)
                continue;
            json += (first ? "" : ",") + profile_json(*p);
            first = false;
        }
        finish_call(call, nullptr, nullptr, "", json + "]");
    });
    return call->handle;
}

BDK_EXPORT bdk_call_t bdk_profile_create(bdk_runtime_t h, bdk_string_t name, bdk_result_cb cb,
                                         void* ctx, bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb || !string_ok(name))
        return 0;
    auto rt = runtime_of(h);
    if (!rt)
        return 0;
    auto call = new_call(rt, cb, nullptr, ctx, rel);
    std::string n = str(name);
    post([rt, call, n] {
        if (call->done)
            return;
        if (n.empty()) {
            finish_call(call, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                        "a profile needs a display name", "");
        } else if (name_taken(*rt, n, nullptr)) {
            finish_call(call, BDK_DOMAIN_PROFILE, BDK_CODE_NAME_TAKEN,
                        "a profile named '" + n + "' exists", "");
        } else {
            auto p = std::make_shared<Profile>();
            p->id = "profile-" + std::to_string(rt->next_id++);
            p->name = n;
            rt->profiles.push_back(p);
            backend().ProfilesChanged(*rt);
            log_line(rt, 1, "profile created: " + p->id);
            finish_call(call, nullptr, nullptr, "", "{\"id\":\"" + json_escape(p->id) + "\"}");
        }
    });
    return call->handle;
}

BDK_EXPORT bdk_call_t bdk_profile_open(bdk_runtime_t h, bdk_string_t id, bdk_profile_t* out,
                                       bdk_result_cb cb, void* ctx, bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb || !out || !string_ok(id))
        return 0;
    auto rt = runtime_of(h);
    if (!rt)
        return 0;
    *out = 0;
    auto call = new_call(rt, cb, nullptr, ctx, rel);
    auto p = find_profile(*rt, str(id));
    if (!p || p->ephemeral) {
        finish_call(call, BDK_DOMAIN_PROFILE, BDK_CODE_NOT_FOUND, "no profile '" + str(id) + "'",
                    "");
        return call->handle;
    }
    if (p->open) {
        finish_call(call, BDK_DOMAIN_PROFILE, "inUse", "the profile is already open", "");
        return call->handle;
    }
    p->open = true;
    p->handle = table_make(T_PROFILE, p);
    *out = p->handle;
    /* A cancelled call is a failed one: the handle goes before the result is delivered. The
       profile stays open, so no second open starts, until the backend has finished this one. */
    call->on_cancel = [p] {
        table_free(p->handle);
        p->handle = 0;
    };
    backend().OpenProfile(*rt, p,
                          [rt, p, call](const std::string& code, const std::string& message) {
                              call->on_cancel = nullptr;
                              if (call->cancelled) {
                                  p->open = false;
                                  if (code.empty())
                                      backend().CloseProfile(*rt, *p);
                                  return;
                              }
                              if (!code.empty()) {
                                  /* The handle goes before the failure is reported. */
                                  p->open = false;
                                  table_free(p->handle);
                                  p->handle = 0;
                                  finish_call(call, BDK_DOMAIN_PROFILE, code.c_str(), message, "");
                                  return;
                              }
                              emit_runtime_event(rt, BDK_RUNTIME_EVENT_PROFILE_OPENED,
                                                 "{\"id\":\"" + json_escape(p->id) + "\"}");
                              finish_call(call, nullptr, nullptr, "", profile_json(*p));
                          });
    return call->handle;
}

BDK_EXPORT bdk_status_t bdk_profile_open_private(bdk_runtime_t h, bdk_profile_t* out,
                                                 bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    auto p = std::make_shared<Profile>();
    p->id = "ephemeral:" + std::to_string(rt->next_id++);
    p->name = "Private";
    p->ephemeral = true;
    if (bdk_status_t s = backend().OpenPrivateProfile(*rt, p, err))
        return s;
    p->open = true;
    p->handle = table_make(T_PROFILE, p);
    rt->profiles.push_back(p);
    *out = p->handle;
    emit_runtime_event(rt, BDK_RUNTIME_EVENT_PROFILE_OPENED,
                       "{\"id\":\"" + json_escape(p->id) + "\"}");
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_profile_close(bdk_profile_t h, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    auto p = get<Profile>(h, T_PROFILE);
    auto rt = current_runtime();
    if (!p || !rt)
        return BDK_ERR_STALE_HANDLE;
    close_profile(rt, p);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_profile_rename(bdk_profile_t h, bdk_string_t name, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    auto p = get<Profile>(h, T_PROFILE);
    auto rt = current_runtime();
    if (!p || !rt)
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(name) || name.length == 0)
        return BDK_ERR_INVALID_ARGUMENT;
    if (p->ephemeral)
        return fail(err, BDK_DOMAIN_PROFILE, BDK_CODE_IS_PRIVATE,
                    "an ephemeral profile cannot be renamed");
    if (name_taken(*rt, str(name), p.get()))
        return fail(err, BDK_DOMAIN_PROFILE, BDK_CODE_NAME_TAKEN,
                    "a profile named '" + str(name) + "' exists");
    p->name = str(name);
    backend().ProfilesChanged(*rt);
    return BDK_OK;
}

BDK_EXPORT bdk_call_t bdk_profile_delete(bdk_runtime_t h, bdk_string_t id, bdk_result_cb cb,
                                         void* ctx, bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb || !string_ok(id))
        return 0;
    auto rt = runtime_of(h);
    if (!rt)
        return 0;
    auto call = new_call(rt, cb, nullptr, ctx, rel);
    std::string pid = str(id);
    post([rt, call, pid] {
        if (call->done)
            return;
        auto p = find_profile(*rt, pid);
        if (!p || p->ephemeral) {
            finish_call(call, BDK_DOMAIN_PROFILE, BDK_CODE_NOT_FOUND, "no profile '" + pid + "'",
                        "");
        } else if (p->open) {
            finish_call(call, BDK_DOMAIN_PROFILE, "inUse", "the profile is open", "");
        } else {
            rt->profiles.erase(std::remove(rt->profiles.begin(), rt->profiles.end(), p),
                               rt->profiles.end());
            backend().ProfilesChanged(*rt);
            backend().DeleteProfile(
                *rt, *p, [call](const std::string& code, const std::string& message) {
                    if (!code.empty())
                        finish_call(call, BDK_DOMAIN_PROFILE, code.c_str(), message, "");
                    else
                        finish_call(call, nullptr, nullptr, "", "{}");
                });
        }
    });
    return call->handle;
}

BDK_EXPORT bdk_status_t bdk_profile_info(bdk_profile_t h, bdk_buffer_t* out, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    auto p = get<Profile>(h, T_PROFILE);
    if (!p)
        return BDK_ERR_STALE_HANDLE;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    *out = make_buffer(profile_json(*p));
    return BDK_OK;
}

/* ---- bdk_contents.h ---- */

BDK_EXPORT bdk_status_t bdk_contents_create(bdk_runtime_t h, const bdk_contents_options_t* opts,
                                            bdk_contents_t* out, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!opts || !out || opts->struct_size < sizeof(size_t))
        return BDK_ERR_INVALID_ARGUMENT;
    if (!rt->frozen)
        return BDK_ERR_NOT_STARTED;
    bdk_contents_options_t o;
    memset(&o, 0, sizeof o);
    memcpy(&o, opts, opts->struct_size < sizeof o ? opts->struct_size : sizeof o);
    auto p = get<Profile>(o.profile, T_PROFILE);
    if (!p)
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(o.initial_url))
        return BDK_ERR_INVALID_ARGUMENT;
    std::string url = str(o.initial_url);
    if (!url.empty() && !plausible_url(url))
        return BDK_ERR_INVALID_ARGUMENT;
    auto c = std::make_shared<Contents>();
    c->rt = rt;
    c->profile = std::move(p);
    c->handle = table_make(T_CONTENTS, c);
    if (bdk_status_t s = backend().CreateContents(*rt, c, url, err)) {
        table_free(c->handle);
        return s;
    }
    rt->contents.push_back(c);
    *out = c->handle;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_close(bdk_contents_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto c = contents_for(h, err, &st);
    if (!c)
        return st;
    if (c->closed)
        return BDK_OK;
    c->closed = true;
    if (auto rt = c->rt.lock())
        withdraw_contents_requests(rt, c->handle);
    backend().CloseContents(c);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_destroy(bdk_contents_t h) {
    bdk_status_t st;
    auto c = contents_for(h, nullptr, &st);
    auto rt = current_runtime();
    if (!c || !rt)
        return st != BDK_OK ? st : BDK_ERR_STALE_HANDLE;
    destroy_contents(rt, c);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_load(bdk_contents_t h, bdk_string_t url, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    if (!string_ok(url) || !plausible_url(str(url)))
        return BDK_ERR_INVALID_ARGUMENT;
    return backend().Load(c, str(url), err);
}

BDK_EXPORT bdk_status_t bdk_contents_reload(bdk_contents_t h, uint8_t bypass_cache,
                                            bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    int current = -1;
    std::vector<Entry> entries;
    backend().Entries(*c, &current, &entries);
    if (current < 0)
        return fail(err, BDK_DOMAIN_NAVIGATION, "aborted", "there is nothing to reload");
    return backend().Reload(c, bypass_cache != 0, err);
}

BDK_EXPORT bdk_status_t bdk_contents_stop(bdk_contents_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    backend().Stop(c);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_go_to_offset(bdk_contents_t h, int32_t offset,
                                                  bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    if (offset == 0)
        return BDK_ERR_INVALID_ARGUMENT;
    int current = -1;
    std::vector<Entry> entries;
    backend().Entries(*c, &current, &entries);
    return nav_to_index(c, static_cast<int64_t>(current) + static_cast<int64_t>(offset), err);
}

BDK_EXPORT bdk_status_t bdk_contents_go_to_index(bdk_contents_t h, int32_t index,
                                                 bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    return nav_to_index(c, index, err);
}

BDK_EXPORT bdk_status_t bdk_contents_entries(bdk_contents_t h, bdk_buffer_t* out,
                                             bdk_error_t* err) {
    bdk_status_t st;
    auto c = contents_for(h, err, &st);
    if (!c)
        return st;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    int current = -1;
    std::vector<Entry> entries;
    backend().Entries(*c, &current, &entries);
    std::string json = "{\"current\":" + std::to_string(current) + ",\"entries\":[";
    for (size_t i = 0; i < entries.size(); ++i) {
        json += (i ? "," : "") + std::string("{\"url\":\"") + json_escape(entries[i].url) +
                "\",\"title\":\"" + json_escape(entries[i].title) + "\"}";
    }
    *out = make_buffer(json + "]}");
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_state(bdk_contents_t h, bdk_contents_state_t* inout,
                                           bdk_error_t* err) {
    bdk_status_t st;
    auto c = contents_for(h, err, &st);
    if (!c)
        return st;
    if (!inout || inout->struct_size < sizeof(size_t))
        return BDK_ERR_INVALID_ARGUMENT;
    ContentsState state;
    backend().State(*c, &state);
    c->url_buf = state.url;
    c->title_buf = state.title;
    bdk_contents_state_t full;
    memset(&full, 0, sizeof full);
    full.struct_size = inout->struct_size < sizeof full ? inout->struct_size : sizeof full;
    full.url = view(c->url_buf);
    full.title = view(c->title_buf);
    full.loading = state.loading ? 1 : 0;
    full.progress = state.progress;
    full.security = state.security;
    full.audio = state.audio;
    full.capture = 0;
    full.visibility = state.visibility;
    full.focused = state.focused ? 1 : 0;
    full.zoom = state.zoom;
    size_t declared = inout->struct_size;
    /* Only the bytes the caller declared are written; struct_size itself is left as the caller set
     * it. */
    memcpy(reinterpret_cast<char*>(inout) + sizeof(size_t),
           reinterpret_cast<char*>(&full) + sizeof(size_t),
           (declared < sizeof full ? declared : sizeof full) - sizeof(size_t));
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_session_state(bdk_contents_t h, bdk_buffer_t* out,
                                                   bdk_error_t* err) {
    bdk_status_t st;
    auto c = contents_for(h, err, &st);
    if (!c)
        return st;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    *out = make_buffer(backend().SessionState(*c));
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_restore(bdk_contents_t h, bdk_bytes_t state,
                                             bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    if (!state.data || state.length == 0)
        return BDK_ERR_INVALID_ARGUMENT;
    return backend().Restore(c, state, err);
}

BDK_EXPORT bdk_status_t bdk_contents_set_muted(bdk_contents_t h, uint8_t muted, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    return backend().SetMuted(*c, muted != 0, err);
}

BDK_EXPORT bdk_status_t bdk_contents_set_zoom(bdk_contents_t h, double zoom, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    if (!(zoom > 0.0))
        return BDK_ERR_INVALID_ARGUMENT;
    return backend().SetZoom(*c, zoom, err);
}

BDK_EXPORT bdk_status_t bdk_contents_set_user_agent(bdk_contents_t h, bdk_string_t ua,
                                                    bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    if (!string_ok(ua))
        return BDK_ERR_INVALID_ARGUMENT;
    return backend().SetUserAgent(*c, str(ua), err);
}

BDK_EXPORT bdk_status_t bdk_contents_discard(bdk_contents_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    return backend().Discard(*c, err); /* the handle stays valid */
}

BDK_EXPORT bdk_status_t bdk_contents_set_auto_discardable(bdk_contents_t h, uint8_t discardable,
                                                          bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    return backend().SetAutoDiscardable(*c, discardable != 0, err);
}

BDK_EXPORT bdk_status_t bdk_contents_find_start(bdk_contents_t h, bdk_string_t text,
                                                const bdk_find_options_t* options,
                                                bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    if (!string_ok(text) || (options && options->struct_size < sizeof(size_t)))
        return BDK_ERR_INVALID_ARGUMENT;
    /* Only the bytes the caller declared are read; match_case is missing (0) in a smaller struct */
    bool match_case = options &&
                      options->struct_size >=
                          offsetof(bdk_find_options_t, match_case) + sizeof options->match_case &&
                      options->match_case != 0;
    return backend().FindStart(c, str(text), match_case, err);
}

BDK_EXPORT bdk_status_t bdk_contents_find_next(bdk_contents_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    return backend().FindNext(c, true, err);
}

BDK_EXPORT bdk_status_t bdk_contents_find_previous(bdk_contents_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    return backend().FindNext(c, false, err);
}

BDK_EXPORT bdk_status_t bdk_contents_find_stop(bdk_contents_t h, uint8_t clear_selection,
                                               bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    return backend().FindStop(c, clear_selection != 0, err);
}

BDK_EXPORT bdk_status_t bdk_contents_find_present(bdk_contents_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    return backend().FindPresent(c, err);
}

BDK_EXPORT bdk_status_t bdk_contents_print(bdk_contents_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    return backend().Print(c, err);
}

BDK_EXPORT bdk_subscription_t bdk_contents_subscribe(bdk_contents_t h, bdk_contents_event_cb cb,
                                                     void* ctx, bdk_release_cb rel) {
    bdk_status_t st;
    auto c = contents_for(h, nullptr, &st);
    if (!c || !cb)
        return 0;
    auto s = std::make_shared<Sub>();
    s->ccb = cb;
    s->ctx = ctx;
    s->rel = rel;
    s->handle = table_make(T_SUB, s);
    c->subs.push_back(s);
    return s->handle;
}

/* ---- bdk_view.h ---- */

BDK_EXPORT bdk_status_t bdk_contents_native_view(bdk_contents_t h, bdk_native_handle_t* out,
                                                 bdk_error_t* err) {
    bdk_status_t st;
    auto c = contents_for(h, err, &st);
    if (!c)
        return st;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    return backend().NativeView(*c, out, err);
}

BDK_EXPORT bdk_status_t bdk_contents_focus(bdk_contents_t h) {
    bdk_status_t st;
    auto c = open_contents_for(h, nullptr, &st);
    auto rt = c ? c->rt.lock() : nullptr;
    if (!c || !rt)
        return st != BDK_OK ? st : BDK_ERR_STALE_HANDLE;
    return backend().Focus(*rt, c);
}

BDK_EXPORT void bdk_contents_set_keyboard_handler(bdk_contents_t h, bdk_key_reserve_cb reserve,
                                                  bdk_key_unhandled_cb unhandled, void* ctx,
                                                  bdk_release_cb rel) {
    bdk_status_t st;
    auto c = contents_for(h, nullptr, &st);
    if (!c) { /* rejected: BDK never took the context, and this function cannot report it */
        release_later(ctx, rel);
        return;
    }
    release_later(c->key_ctx, c->key_rel);
    c->key_reserve = reserve;
    c->key_unhandled = unhandled;
    c->key_ctx = ctx;
    c->key_rel = rel;
}

/* ---- bdk_window.h ---- */

BDK_EXPORT bdk_status_t bdk_window_register(bdk_runtime_t h, bdk_native_handle_t native,
                                            uint32_t kind, bdk_window_t* out, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!out || native.kind == 0)
        return BDK_ERR_INVALID_ARGUMENT;
    auto w = std::make_shared<Window>();
    w->kind = kind;
    w->native = native;
    if (bdk_status_t s = backend().RegisterWindow(*rt, *w, err))
        return s;
    *out = table_make(T_WINDOW, w);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_window_unregister(bdk_window_t h, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    auto w = get<Window>(h, T_WINDOW);
    if (!w)
        return BDK_ERR_STALE_HANDLE;
    table_free(h);
    backend().UnregisterWindow(*w);
    return BDK_OK;
}

/* ---- bdk_ui.h ---- */

BDK_EXPORT bdk_status_t bdk_ui_set_handler(bdk_runtime_t h, uint32_t surface, bdk_ui_handler_cb cb,
                                           void* ctx, bdk_release_cb rel, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!cb || surface < BDK_SURFACE_PERMISSIONS || surface > BDK_SURFACE_EXTERNAL_PROTOCOLS)
        return BDK_ERR_INVALID_ARGUMENT;
    if (rt->frozen)
        return fail(err, BDK_DOMAIN_CONFIG, BDK_CODE_REGISTRATION_AFTER_START,
                    "UI handlers must be set before the freeze");
    auto it = rt->ui_handlers.find(surface);
    if (it != rt->ui_handlers.end())
        release_later(it->second.ctx, it->second.rel);
    rt->ui_handlers[surface] = UiHandler{cb, ctx, rel};
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_ui_complete(bdk_request_t h, bdk_string_t completion, bdk_string_t args,
                                        bdk_native_handle_t gesture, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    std::shared_ptr<Runtime> rt;
    auto r = request_of(h, &rt);
    if (!r || !rt)
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(completion) || completion.length == 0 || !string_ok(args))
        return BDK_ERR_INVALID_ARGUMENT;
    std::string c = str(completion), a = str(args);
    if (!a.empty() && !valid_json(a))
        return BDK_ERR_INVALID_ARGUMENT;
    return ui_complete(*r, c, a, gesture, err);
}

BDK_EXPORT bdk_status_t bdk_ui_use_default(bdk_request_t h, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    std::shared_ptr<Runtime> rt;
    auto r = request_of(h, &rt);
    if (!r || !rt)
        return BDK_ERR_STALE_HANDLE;
    return ui_use_default(*r, err);
}

BDK_EXPORT void bdk_ui_set_withdrawn_handler(bdk_request_t h, void (*cb)(void*), void* ctx,
                                             bdk_release_cb rel) {
    auto r = check_thread() == BDK_OK ? get<Request>(h, T_REQUEST) : nullptr;
    if (!r || r->finished) { /* rejected or already over: BDK will never use the context */
        release_later(ctx, rel);
        return;
    }
    release_withdrawn(*r); /* a replaced handler is released */
    r->withdrawn = cb;
    r->wctx = ctx;
    r->wrel = rel;
}

BDK_EXPORT void bdk_ui_request_release(bdk_request_t h) {
    if (check_thread() != BDK_OK)
        return;
    std::shared_ptr<Runtime> rt;
    auto r = request_of(h, &rt);
    if (!r || !rt)
        return;
    ui_release(*r);
    table_free(h);
}

} /* extern "C" */
