/* Stub runtime: profiles, WebContents, views, windows, UI requests and the test hooks. */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "stub_internal.h"

using namespace bdkstub;

namespace bdkstub {

void close_profile(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Profile>& p) {
    for (auto& c : std::vector<std::shared_ptr<Contents>>(rt->contents)) {
        if (c->profile == p)
            destroy_contents(rt, c);
    }
    p->open = false;
    table_free(p->handle);
    p->handle = 0;
    if (p->ephemeral) {
        rt->profiles.erase(std::remove(rt->profiles.begin(), rt->profiles.end(), p),
                           rt->profiles.end());
    }
    emit_runtime_event(rt, BDK_RUNTIME_EVENT_PROFILE_CLOSED,
                       "{\"id\":\"" + json_escape(p->id) + "\"}");
}

void destroy_contents(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c) {
    c->dead = true;
    if (c->nav)
        c->nav->cancelled = true;
    for (auto& s : c->subs)
        deactivate(s);
    release_later(c->key_ctx, c->key_rel);
    c->key_rel = nullptr;
    table_free(c->handle);
    rt->contents.erase(std::remove(rt->contents.begin(), rt->contents.end(), c),
                       rt->contents.end());
}

void emit_contents_event_now(const std::shared_ptr<Contents>& c, uint32_t kind,
                             const std::string& url, const std::string& detail, int32_t net_error,
                             uint32_t flags) {
    if (c->dead)
        return;
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

} // namespace bdkstub

namespace {

std::string profile_json(const Profile& p) {
    return "{\"id\":\"" + json_escape(p.id) + "\",\"name\":\"" + json_escape(p.name) +
           "\",\"kind\":\"" + (p.ephemeral ? "ephemeral" : "persistent") + "\",\"parent\":null}";
}

bdk_buffer_t make_buffer(const std::string& data) {
    auto b = std::make_shared<Buf>();
    b->data = data;
    return table_make(T_BUFFER, b);
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

void emit_later(const std::shared_ptr<Contents>& c, uint32_t kind, const std::string& url,
                const std::string& detail, int32_t net_error, uint32_t flags) {
    post([c, kind, url, detail, net_error, flags] {
        emit_contents_event_now(c, kind, url, detail, net_error, flags);
    });
}

constexpr int32_t kErrAborted = -3;
constexpr uint32_t kMainFrame = BDK_EVENT_FLAG_MAIN_FRAME;

void cancel_nav(const std::shared_ptr<Contents>& c, const char* why) {
    if (!c->nav)
        return;
    c->nav->cancelled = true;
    c->nav.reset();
    c->loading = false;
    std::string url =
        c->current >= 0 ? c->entries[static_cast<size_t>(c->current)].url : "about:blank";
    emit_later(c, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, url,
               std::string("{\"code\":\"") + why + "\"}", kErrAborted, kMainFrame);
}

/* target_index -1: a new entry. Otherwise history navigation (or reload) to that entry. */
void begin_nav(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c,
               const std::string& url, int target_index) {
    cancel_nav(c, "superseded");
    auto nav = std::make_shared<Nav>();
    c->nav = nav;
    c->loading = true;
    c->progress = 0.1;
    uint32_t flags = kMainFrame | BDK_EVENT_FLAG_USER_INITIATED;
    emit_later(c, BDK_CONTENTS_EVENT_NAVIGATION_STARTED, url, "{}", 0, flags);
    post([rt, c, nav, url, target_index, flags] {
        if (nav->cancelled || c->dead)
            return;
        int32_t net_error = 0;
        std::string title = url;
        auto it = rt->scripts.find(url);
        if (it != rt->scripts.end()) {
            net_error = it->second.first;
            if (!it->second.second.empty())
                title = it->second.second;
        }
        if (net_error != 0) {
            c->nav.reset();
            c->loading = false;
            c->progress = 1.0;
            emit_contents_event_now(c, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, url,
                                    "{\"code\":\"netError\"}", net_error,
                                    flags | BDK_EVENT_FLAG_ERROR_PAGE);
            return;
        }
        if (target_index < 0) {
            c->entries.resize(static_cast<size_t>(c->current) + 1);
            c->entries.push_back(Entry{url, title});
            c->current = static_cast<int>(c->entries.size()) - 1;
        } else {
            c->current = target_index;
            c->entries[static_cast<size_t>(target_index)].title = title;
        }
        c->progress = 0.8;
        emit_contents_event_now(c, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, url, "{}", 0, flags);
        emit_contents_event_now(c, BDK_CONTENTS_EVENT_TITLE_CHANGED, url,
                                "{\"title\":\"" + json_escape(title) + "\"}", 0, kMainFrame);
        post([c, nav, url, flags] {
            if (nav->cancelled || c->dead)
                return;
            c->nav.reset();
            c->loading = false;
            c->progress = 1.0;
            emit_contents_event_now(c, BDK_CONTENTS_EVENT_LOAD_FINISHED, url, "{}", 0, flags);
        });
    });
}

bdk_status_t nav_to_index(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c,
                          int index, bdk_error_t*) {
    if (index < 0 || index >= static_cast<int>(c->entries.size()))
        return BDK_ERR_INVALID_ARGUMENT;
    begin_nav(rt, c, c->entries[static_cast<size_t>(index)].url, index);
    return BDK_OK;
}

bool granting(const std::string& completion) {
    return completion == "grant";
}

void set_outcome(const std::shared_ptr<Runtime>& rt, const Request& r, uint32_t kind,
                 const std::string& completion, const std::string& args) {
    bdk_stub_ui_outcome_t& o = rt->outcome;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.kind = kind;
    o.surface = r.surface;
    snprintf(o.completion, sizeof o.completion, "%s", completion.c_str());
    snprintf(o.arguments_json, sizeof o.arguments_json, "%s", args.c_str());
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
    } else if (p->open) {
        finish_call(call, BDK_DOMAIN_PROFILE, "inUse", "the profile is already open", "");
    } else {
        p->open = true;
        p->handle = table_make(T_PROFILE, p);
        *out = p->handle;
        emit_runtime_event(rt, BDK_RUNTIME_EVENT_PROFILE_OPENED,
                           "{\"id\":\"" + json_escape(p->id) + "\"}");
        finish_call(call, nullptr, nullptr, "", profile_json(*p));
    }
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
            finish_call(call, nullptr, nullptr, "", "{}");
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
    rt->contents.push_back(c);
    *out = c->handle;
    if (!url.empty())
        begin_nav(rt, c, url, -1);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_close(bdk_contents_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto c = contents_for(h, err, &st);
    if (!c)
        return st;
    if (c->closed)
        return BDK_OK;
    cancel_nav(c, "aborted");
    c->closed = true;
    emit_later(c, BDK_CONTENTS_EVENT_CLOSED, "", "{}", 0, 0);
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
    auto rt = c->rt.lock();
    if (!string_ok(url) || !plausible_url(str(url)))
        return BDK_ERR_INVALID_ARGUMENT;
    begin_nav(rt, c, str(url), -1);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_reload(bdk_contents_t h, uint8_t, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    if (c->current < 0)
        return fail(err, BDK_DOMAIN_NAVIGATION, "aborted", "there is nothing to reload");
    return nav_to_index(c->rt.lock(), c, c->current, err);
}

BDK_EXPORT bdk_status_t bdk_contents_stop(bdk_contents_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    cancel_nav(c, "aborted");
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
    return nav_to_index(c->rt.lock(), c, c->current + offset, err);
}

BDK_EXPORT bdk_status_t bdk_contents_go_to_index(bdk_contents_t h, int32_t index,
                                                 bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    return nav_to_index(c->rt.lock(), c, index, err);
}

BDK_EXPORT bdk_status_t bdk_contents_entries(bdk_contents_t h, bdk_buffer_t* out,
                                             bdk_error_t* err) {
    bdk_status_t st;
    auto c = contents_for(h, err, &st);
    if (!c)
        return st;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    std::string json = "{\"current\":" + std::to_string(c->current) + ",\"entries\":[";
    for (size_t i = 0; i < c->entries.size(); ++i) {
        json += (i ? "," : "") + std::string("{\"url\":\"") + json_escape(c->entries[i].url) +
                "\",\"title\":\"" + json_escape(c->entries[i].title) + "\"}";
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
    bool has = c->current >= 0;
    c->url_buf = has ? c->entries[static_cast<size_t>(c->current)].url : "about:blank";
    c->title_buf = has ? c->entries[static_cast<size_t>(c->current)].title : "";
    bdk_contents_state_t full;
    memset(&full, 0, sizeof full);
    full.struct_size = inout->struct_size < sizeof full ? inout->struct_size : sizeof full;
    full.url = view(c->url_buf);
    full.title = view(c->title_buf);
    full.loading = c->loading ? 1 : 0;
    full.progress = c->progress;
    if (c->url_buf.compare(0, 8, "https://") == 0) {
        full.security = BDK_SECURITY_SECURE;
    } else if (c->url_buf.compare(0, 7, "http://") == 0) {
        full.security = BDK_SECURITY_NONE;
    } else {
        full.security = BDK_SECURITY_UNKNOWN;
    }
    full.audio = c->muted ? BDK_AUDIO_MUTED : 0;
    full.capture = 0;
    full.visibility = BDK_VISIBILITY_VISIBLE;
    full.focused = c->focused ? 1 : 0;
    full.zoom = c->zoom;
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
    std::string s =
        "BDKSTUB1 " + std::to_string(c->current) + " " + std::to_string(c->entries.size()) + "\n";
    for (auto& e : c->entries) {
        s += std::to_string(e.url.size()) + " " + std::to_string(e.title.size()) + "\n" + e.url +
             e.title + "\n";
    }
    *out = make_buffer(s);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_restore(bdk_contents_t h, bdk_bytes_t state,
                                             bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    if (!state.data && state.length)
        return BDK_ERR_INVALID_ARGUMENT;
    std::string s(reinterpret_cast<const char*>(state.data), state.length);
    if (s.compare(0, 7, "BDKSTUB") != 0 || s.size() < 9)
        return BDK_ERR_INVALID_ARGUMENT;
    if (s[7] != '1')
        return fail(err, BDK_DOMAIN_CONTENTS, "stateTooNew",
                    "the session state is from a newer format");
    const char* p = s.c_str() + 9;
    char* end = nullptr;
    long current = strtol(p, &end, 10);
    long count = strtol(end, &end, 10);
    if (count < 0 || current < -1 || current >= count || *end != '\n')
        return BDK_ERR_INVALID_ARGUMENT;
    std::vector<Entry> entries;
    size_t pos = static_cast<size_t>(end - s.c_str()) + 1;
    for (long i = 0; i < count; ++i) {
        const char* q = s.c_str() + pos;
        long ul = strtol(q, &end, 10);
        long tl = strtol(end, &end, 10);
        if (ul < 0 || tl < 0 || *end != '\n')
            return BDK_ERR_INVALID_ARGUMENT;
        pos = static_cast<size_t>(end - s.c_str()) + 1;
        if (pos + static_cast<size_t>(ul + tl) > s.size())
            return BDK_ERR_INVALID_ARGUMENT;
        entries.push_back(Entry{s.substr(pos, static_cast<size_t>(ul)),
                                s.substr(pos + static_cast<size_t>(ul), static_cast<size_t>(tl))});
        pos += static_cast<size_t>(ul + tl) + 1;
    }
    cancel_nav(c, "aborted");
    c->entries = std::move(entries);
    c->current = static_cast<int>(current);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_set_muted(bdk_contents_t h, uint8_t muted, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    c->muted = muted != 0;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_set_zoom(bdk_contents_t h, double zoom, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    if (!(zoom > 0.0))
        return BDK_ERR_INVALID_ARGUMENT;
    c->zoom = zoom;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_set_user_agent(bdk_contents_t h, bdk_string_t ua,
                                                    bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    if (!string_ok(ua))
        return BDK_ERR_INVALID_ARGUMENT;
    c->user_agent = str(ua);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_discard(bdk_contents_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    c->discarded = true; /* the handle stays valid */
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_contents_set_auto_discardable(bdk_contents_t h, uint8_t discardable,
                                                          bdk_error_t* err) {
    bdk_status_t st;
    auto c = open_contents_for(h, err, &st);
    if (!c)
        return st;
    c->auto_discardable = discardable != 0;
    return BDK_OK;
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
    return fail(err, BDK_DOMAIN_PLATFORM, "unsupported", "the stub runtime has no native views");
}

BDK_EXPORT bdk_status_t bdk_contents_focus(bdk_contents_t h) {
    bdk_status_t st;
    auto c = open_contents_for(h, nullptr, &st);
    auto rt = c ? c->rt.lock() : nullptr;
    if (!c || !rt)
        return st != BDK_OK ? st : BDK_ERR_STALE_HANDLE;
    for (auto& o : rt->contents) {
        if (o != c && o->focused) {
            o->focused = false;
            emit_later(o, BDK_CONTENTS_EVENT_FOCUS_CHANGED, "", "{\"focused\":false}", 0, 0);
        }
    }
    if (!c->focused) {
        c->focused = true;
        emit_later(c, BDK_CONTENTS_EVENT_FOCUS_CHANGED, "", "{\"focused\":true}", 0, 0);
    }
    return BDK_OK;
}

BDK_EXPORT void bdk_contents_set_keyboard_handler(bdk_contents_t h, bdk_key_reserve_cb reserve,
                                                  bdk_key_unhandled_cb unhandled, void* ctx,
                                                  bdk_release_cb rel) {
    bdk_status_t st;
    auto c = contents_for(h, nullptr, &st);
    if (!c)
        return;
    release_later(c->key_ctx, c->key_rel);
    c->key_reserve = reserve;
    c->key_unhandled = unhandled;
    c->key_ctx = ctx;
    c->key_rel = rel;
}

BDK_EXPORT bdk_status_t bdk_stub_simulate_key(bdk_contents_t h, bdk_native_handle_t ev,
                                              uint8_t* out_reserved) {
    bdk_status_t st;
    auto c = contents_for(h, nullptr, &st);
    if (!c)
        return st;
    uint8_t reserved = c->key_reserve ? c->key_reserve(c->key_ctx, ev) : 0;
    if (!reserved && c->key_unhandled)
        c->key_unhandled(c->key_ctx, ev);
    if (out_reserved)
        *out_reserved = reserved;
    return BDK_OK;
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
    *out = table_make(T_WINDOW, w);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_window_unregister(bdk_window_t h, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    if (!get<Window>(h, T_WINDOW))
        return BDK_ERR_STALE_HANDLE;
    table_free(h);
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
    if (r->finished)
        return fail(err, BDK_DOMAIN_UI, BDK_CODE_REQUEST_COMPLETED,
                    "the request is already completed");
    if (granting(c) && gesture.kind == 0)
        return fail(err, BDK_DOMAIN_UI, BDK_CODE_GESTURE_REQUIRED,
                    "a granting completion needs the gesture event");
    r->finished = true;
    release_later(r->wctx, r->wrel);
    set_outcome(rt, *r, BDK_STUB_UI_COMPLETED, c, a);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_ui_use_default(bdk_request_t h, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    std::shared_ptr<Runtime> rt;
    auto r = request_of(h, &rt);
    if (!r || !rt)
        return BDK_ERR_STALE_HANDLE;
    if (r->finished)
        return fail(err, BDK_DOMAIN_UI, BDK_CODE_REQUEST_COMPLETED,
                    "the request is already completed");
    r->finished = true;
    release_later(r->wctx, r->wrel);
    set_outcome(rt, *r, BDK_STUB_UI_DEFAULTED, "", "");
    return BDK_OK;
}

BDK_EXPORT void bdk_ui_set_withdrawn_handler(bdk_request_t h, void (*cb)(void*), void* ctx,
                                             bdk_release_cb rel) {
    if (check_thread() != BDK_OK)
        return;
    auto r = get<Request>(h, T_REQUEST);
    if (!r)
        return;
    release_later(r->wctx, r->wrel);
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
    if (!r->finished) {
        r->finished = true;
        release_later(r->wctx, r->wrel);
        set_outcome(rt, *r, BDK_STUB_UI_CANCELLED, "", "");
    }
    table_free(h);
}

/* ---- bdk_stub.h test hooks ---- */

BDK_EXPORT bdk_status_t bdk_stub_script_navigation(bdk_runtime_t h, bdk_string_t url,
                                                   const bdk_stub_navigation_script_t* script) {
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!script || script->struct_size < sizeof *script || !string_ok(url) ||
        !string_ok(script->title))
        return BDK_ERR_INVALID_ARGUMENT;
    rt->scripts[str(url)] = std::make_pair(script->net_error, str(script->title));
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_raise_ui_request(bdk_runtime_t h, uint32_t surface,
                                                  bdk_contents_t contents, bdk_string_t payload) {
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    auto c = contents ? get<Contents>(contents, T_CONTENTS) : nullptr;
    if ((contents && !c) || !string_ok(payload))
        return BDK_ERR_INVALID_ARGUMENT;
    auto r = std::make_shared<Request>();
    r->surface = surface;
    r->contents = contents;
    r->profile = c && c->profile ? c->profile->handle : 0;
    r->payload = str(payload);
    r->handle = table_make(T_REQUEST, r);
    rt->requests.push_back(r);
    set_outcome(rt, *r, BDK_STUB_UI_NONE, "", "");
    post([rt, r] {
        if (rt->shutting || r->finished)
            return;
        auto it = rt->ui_handlers.find(r->surface);
        if (it == rt->ui_handlers.end()) {
            r->finished = true;
            set_outcome(rt, *r, BDK_STUB_UI_DEFAULTED, "", "");
            return;
        }
        bdk_ui_request_info_t info;
        memset(&info, 0, sizeof info);
        info.struct_size = sizeof info;
        info.request = r->handle;
        info.surface = r->surface;
        info.contents = r->contents;
        info.profile = r->profile;
        info.payload_json = view(r->payload);
        bdk_ui_response_t resp = it->second.cb(it->second.ctx, &info);
        r->delivered = true;
        if (resp == BDK_UI_USE_DEFAULT && !r->finished) {
            r->finished = true;
            set_outcome(rt, *r, BDK_STUB_UI_DEFAULTED, "", "");
        } else if (resp == BDK_UI_REJECT && !r->finished) {
            r->finished = true;
            set_outcome(rt, *r, BDK_STUB_UI_REJECTED, "", "");
        }
    });
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_withdraw_ui_requests(bdk_runtime_t h) {
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    for (auto& r : rt->requests) {
        if (r->finished)
            continue;
        r->finished = true;
        set_outcome(rt, *r, BDK_STUB_UI_WITHDRAWN, "", "");
        if (r->withdrawn) {
            void (*cb)(void*) = r->withdrawn;
            void* ctx = r->wctx;
            post([cb, ctx] { cb(ctx); });
            release_later(ctx, r->wrel);
        }
    }
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_last_ui_outcome(bdk_runtime_t h, bdk_stub_ui_outcome_t* out) {
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    *out = rt->outcome;
    return BDK_OK;
}

} /* extern "C" */
