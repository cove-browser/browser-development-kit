/* Runtime core: infrastructure, base types, runtime lifecycle, broker. */
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "automation_internal.h"
#include "bdk_abi_hash.h"
#include "broker_internal.h"
#include "core_internal.h"
#include "profile_versions.h"
#include "providers_internal.h"
#include "routes.h"

using namespace bdkcore;

namespace bdkcore {
namespace {

struct Slot {
    uint32_t gen = 1;
    Type type = T_NONE;
    std::shared_ptr<void> obj;
};

/* Everything except the UI-thread identity is touched on the UI thread only. */
struct Globals {
    std::vector<Slot> slots;
    std::vector<uint32_t> free_list;
    std::mutex mutex;
    bool ui_set = false;
    std::thread::id ui;
    std::shared_ptr<Runtime> runtime;
};

Globals& G() {
    static Globals* g = new Globals;
    return *g;
}

constexpr uint64_t kGenMask = 0xFFFFFFu;

} // namespace

bdk_handle_t table_make(Type t, std::shared_ptr<void> obj) {
    Globals& g = G();
    uint32_t index;
    if (!g.free_list.empty()) {
        index = g.free_list.back();
        g.free_list.pop_back();
    } else {
        index = static_cast<uint32_t>(g.slots.size());
        g.slots.emplace_back();
    }
    Slot& s = g.slots[index];
    s.type = t;
    s.obj = std::move(obj);
    return (static_cast<uint64_t>(t) << 56u) | ((static_cast<uint64_t>(s.gen) & kGenMask) << 32u) |
           index;
}

static Slot* find_slot(bdk_handle_t h, bool any_type, Type t) {
    Globals& g = G();
    uint32_t index = static_cast<uint32_t>(h & 0xFFFFFFFFu);
    uint32_t gen = static_cast<uint32_t>((h >> 32u) & kGenMask);
    Type tag = static_cast<Type>(h >> 56u);
    if (tag == T_NONE || index >= g.slots.size())
        return nullptr;
    Slot& s = g.slots[index];
    if (s.type == T_NONE || s.type != tag || (s.gen & kGenMask) != gen)
        return nullptr;
    if (!any_type && tag != t)
        return nullptr;
    return &s;
}

std::shared_ptr<void> table_get(bdk_handle_t h, Type t) {
    Slot* s = find_slot(h, false, t);
    return s ? s->obj : nullptr;
}

Type table_type(bdk_handle_t h) {
    Slot* s = find_slot(h, true, T_NONE);
    return s ? s->type : T_NONE;
}

bool table_free(bdk_handle_t h) {
    Slot* s = find_slot(h, true, T_NONE);
    if (!s)
        return false;
    Globals& g = G();
    uint32_t index = static_cast<uint32_t>(h & 0xFFFFFFFFu);
    s->type = T_NONE;
    s->obj.reset();
    s->gen = (s->gen + 1) & kGenMask;
    if (s->gen == 0)
        s->gen = 1;
    g.free_list.push_back(index);
    return true;
}

void post(std::function<void()> fn) {
    backend().Post(std::move(fn));
}

void release_later(void* ctx, bdk_release_cb rel) {
    if (rel)
        post([ctx, rel] { rel(ctx); });
}

bdk_status_t check_thread() {
    Globals& g = G();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.ui_set && g.ui != std::this_thread::get_id())
        return BDK_ERR_WRONG_THREAD;
    return BDK_OK;
}

std::string str(bdk_string_t s) {
    return s.data ? std::string(s.data, s.length) : std::string();
}
bdk_string_t view(const std::string& s) {
    bdk_string_t v;
    v.data = s.data();
    v.length = s.size();
    return v;
}
bool string_ok(bdk_string_t s) {
    return s.data != nullptr || s.length == 0;
}

std::string json_escape(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

/* A strict RFC 8259 validator; nothing is built, it only answers whether the text is JSON. */
namespace {
struct JsonParser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    explicit JsonParser(const std::string& text) : s(text) {}
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }
    bool lit(const char* w) {
        size_t n = strlen(w);
        if (s.compare(i, n, w) != 0)
            return false;
        i += n;
        return true;
    }
    bool string() {
        if (i >= s.size() || s[i] != '"')
            return false;
        ++i;
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i++]);
            if (c == '"')
                return true;
            if (c < 0x20)
                return false;
            if (c == '\\') {
                if (i >= s.size())
                    return false;
                char e = s[i++];
                if (e == 'u') {
                    for (int k = 0; k < 4; ++k) {
                        if (i >= s.size())
                            return false;
                        unsigned char hex = static_cast<unsigned char>(s[i]);
                        ++i;
                        if (!isxdigit(hex))
                            return false;
                    }
                } else if (!strchr("\"\\/bfnrt", e)) {
                    return false;
                }
            }
        }
        return false;
    }
    bool number() {
        size_t start = i;
        if (i < s.size() && s[i] == '-')
            ++i;
        if (i >= s.size())
            return false;
        if (s[i] == '0') {
            ++i;
        } else if (s[i] >= '1' && s[i] <= '9') {
            while (i < s.size() && isdigit(static_cast<unsigned char>(s[i])))
                ++i;
        } else {
            return false;
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            size_t d = i;
            while (i < s.size() && isdigit(static_cast<unsigned char>(s[i])))
                ++i;
            if (i == d)
                return false;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                ++i;
            size_t d = i;
            while (i < s.size() && isdigit(static_cast<unsigned char>(s[i])))
                ++i;
            if (i == d)
                return false;
        }
        return i > start;
    }
    bool value() {
        if (++depth > 128)
            return false;
        ws();
        bool ok = false;
        if (i < s.size()) {
            char c = s[i];
            if (c == '{') {
                ++i;
                ws();
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    ok = true;
                } else {
                    for (;;) {
                        ws();
                        if (!string())
                            break;
                        ws();
                        if (i >= s.size() || s[i] != ':')
                            break;
                        ++i;
                        if (!value())
                            break;
                        ws();
                        if (i < s.size() && s[i] == ',') {
                            ++i;
                            continue;
                        }
                        if (i < s.size() && s[i] == '}') {
                            ++i;
                            ok = true;
                        }
                        break;
                    }
                }
            } else if (c == '[') {
                ++i;
                ws();
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    ok = true;
                } else {
                    for (;;) {
                        if (!value())
                            break;
                        ws();
                        if (i < s.size() && s[i] == ',') {
                            ++i;
                            continue;
                        }
                        if (i < s.size() && s[i] == ']') {
                            ++i;
                            ok = true;
                        }
                        break;
                    }
                }
            } else if (c == '"') {
                ok = string();
            } else if (c == 't') {
                ok = lit("true");
            } else if (c == 'f') {
                ok = lit("false");
            } else if (c == 'n') {
                ok = lit("null");
            } else {
                ok = number();
            }
        }
        --depth;
        return ok;
    }
};
} // namespace

bool valid_json(const std::string& text, bool* is_object) {
    JsonParser p(text);
    p.ws();
    bool object = p.i < text.size() && text[p.i] == '{';
    bool ok = p.value();
    p.ws();
    ok = ok && p.i == text.size();
    if (is_object)
        *is_object = ok && object;
    return ok;
}

bdk_error_t make_error(const std::string& domain, const std::string& code,
                       const std::string& message) {
    auto e = std::make_shared<Err>();
    e->domain = domain;
    e->code = code;
    e->message = message;
    return table_make(T_ERROR, e);
}

bdk_buffer_t make_buffer(const std::string& data) {
    auto b = std::make_shared<Buf>();
    b->data = data;
    return table_make(T_BUFFER, b);
}

void clear_out(bdk_error_t* out) {
    if (out)
        *out = 0;
}

bdk_status_t fail(bdk_error_t* out, const char* domain, const char* code,
                  const std::string& message) {
    if (out)
        *out = make_error(domain, code, message);
    return BDK_ERR_FAILED;
}

std::shared_ptr<Runtime> current_runtime() {
    return G().runtime;
}

std::shared_ptr<Runtime> runtime_of(bdk_runtime_t h) {
    auto rt = get<Runtime>(h, T_RUNTIME);
    return (rt && !rt->shutting) ? rt : nullptr;
}

std::shared_ptr<Call> new_call(const std::shared_ptr<Runtime>& rt, bdk_result_cb cb,
                               bdk_progress_cb prog, void* ctx, bdk_release_cb rel) {
    auto c = std::make_shared<Call>();
    c->cb = cb;
    c->prog = prog;
    c->ctx = ctx;
    c->rel = rel;
    c->handle = table_make(T_CALL, c);
    rt->calls.erase(std::remove_if(rt->calls.begin(), rt->calls.end(),
                                   [](const std::weak_ptr<Call>& w) { return w.expired(); }),
                    rt->calls.end());
    rt->calls.push_back(c);
    return c;
}

/* Delivers the result later, on the UI thread: the call handle goes stale, the result callback runs
   exactly once, then the release callback. */
void finish_call(const std::shared_ptr<Call>& c, const char* domain, const char* code,
                 const std::string& message, const std::string& result_json) {
    if (c->done)
        return;
    c->done = true;
    std::string d = domain ? domain : "", k = code ? code : "";
    post([c, d, k, message, result_json] {
        c->delivered = true;
        table_free(c->handle);
        bdk_error_t e = d.empty() ? 0 : make_error(d, k, message);
        c->cb(c->ctx, e, view(d.empty() ? result_json : std::string()), 0);
        if (e)
            table_free(e);
        if (c->rel)
            c->rel(c->ctx);
    });
}

void finish_call_binary(const std::shared_ptr<Call>& c, const std::string& result_json,
                        std::string bytes) {
    if (c->done)
        return;
    c->done = true;
    post([c, result_json, bytes = std::move(bytes)] {
        c->delivered = true;
        table_free(c->handle);
        bdk_buffer_t b = make_buffer(bytes);
        c->cb(c->ctx, 0, view(result_json), b);
        table_free(b);
        if (c->rel)
            c->rel(c->ctx);
    });
}

void emit_runtime_event(const std::shared_ptr<Runtime>& rt, uint32_t kind,
                        const std::string& detail, bool to_closing) {
    auto subs = rt->subs; /* the subscribers at the time of the event */
    if (to_closing) {
        /* Only the subscribers active now are owed the closing event; it is delivered before the
           release that the shutdown queues after it. An inactive subscription is never called. */
        subs.erase(std::remove_if(subs.begin(), subs.end(),
                                  [](const std::shared_ptr<Sub>& s) { return !s->active; }),
                   subs.end());
        for (auto& s : subs)
            s->closing = true;
    }
    post([subs, kind, detail, to_closing] {
        for (auto& s : subs) {
            bool callable = to_closing ? s->closing : s->active;
            if (callable && s->rcb)
                s->rcb(s->ctx, kind, view(detail));
        }
    });
}

void log_line(const std::shared_ptr<Runtime>& rt, uint32_t level, const std::string& message) {
    if (!rt->log_cb)
        return;
    bdk_log_cb cb = rt->log_cb;
    void* ctx = rt->log_ctx;
    post([cb, ctx, level, message] { cb(ctx, level, view(message)); });
}

void deactivate(const std::shared_ptr<Sub>& s) {
    if (!s->active)
        return;
    s->active = false;
    if (s->handle)
        table_free(s->handle);
    s->handle = 0;
    release_later(s->ctx, s->rel);
}

} // namespace bdkcore

/* ---- bdk_base.h ---- */

extern "C" {

BDK_EXPORT bdk_status_t bdk_release(bdk_handle_t h) {
    if (bdk_status_t s = check_thread())
        return s;
    Type t = table_type(h);
    switch (t) {
    case T_NONE:
        return BDK_ERR_STALE_HANDLE;
    case T_ERROR:
    case T_BUFFER:
        table_free(h);
        return BDK_OK;
    case T_CONTENTS:
        return bdk_contents_destroy(h);
    case T_PROFILE:
        return bdk_profile_close(h, nullptr);
    case T_SUB:
        bdk_unsubscribe(h);
        return BDK_OK;
    case T_SESSION:
        return bdk_session_revoke(h, nullptr);
    case T_TASK:
        return task_release(h);
    case T_WINDOW:
        return bdk_window_unregister(h, nullptr);
    case T_REQUEST:
        bdk_ui_request_release(h);
        return BDK_OK;
    default:
        return BDK_ERR_INVALID_ARGUMENT;
    }
}

BDK_EXPORT bdk_string_t bdk_error_domain(bdk_error_t e) {
    auto p = get<Err>(e, T_ERROR);
    return p ? view(p->domain) : bdk_string_t{nullptr, 0};
}
BDK_EXPORT bdk_string_t bdk_error_code(bdk_error_t e) {
    auto p = get<Err>(e, T_ERROR);
    return p ? view(p->code) : bdk_string_t{nullptr, 0};
}
BDK_EXPORT bdk_string_t bdk_error_message(bdk_error_t e) {
    auto p = get<Err>(e, T_ERROR);
    return p ? view(p->message) : bdk_string_t{nullptr, 0};
}
BDK_EXPORT bdk_string_t bdk_error_details_json(bdk_error_t e) {
    auto p = get<Err>(e, T_ERROR);
    return p ? view(p->details) : bdk_string_t{nullptr, 0};
}
BDK_EXPORT void bdk_error_release(bdk_error_t e) {
    if (table_type(e) == T_ERROR)
        table_free(e);
}

BDK_EXPORT bdk_bytes_t bdk_buffer_bytes(bdk_buffer_t b) {
    auto p = get<Buf>(b, T_BUFFER);
    if (!p)
        return bdk_bytes_t{nullptr, 0};
    return bdk_bytes_t{reinterpret_cast<const uint8_t*>(p->data.data()), p->data.size()};
}

BDK_EXPORT bdk_status_t bdk_call_cancel(bdk_call_t h) {
    if (bdk_status_t s = check_thread())
        return s;
    auto c = get<Call>(h, T_CALL);
    if (!c)
        return BDK_ERR_STALE_HANDLE;
    if (!c->done) {
        c->cancelled = true;
        run_on_stop(c);
        if (c->on_cancel) {
            auto on_cancel = std::move(c->on_cancel);
            c->on_cancel = nullptr;
            on_cancel();
        }
        finish_call(c, BDK_DOMAIN_BROKER, BDK_CODE_CANCELLED, "the call was cancelled", "");
    }
    return BDK_OK;
}

BDK_EXPORT BDK_ANY_THREAD uint32_t bdk_abi_version(void) {
    return BDK_ABI_VERSION;
}

BDK_EXPORT BDK_ANY_THREAD bdk_string_t bdk_abi_hash(void) {
    return bdk_string_t{BDK_ABI_HASH, sizeof(BDK_ABI_HASH) - 1};
}

BDK_EXPORT BDK_ANY_THREAD bdk_string_t bdk_runtime_manifest_json(void) {
    static const std::string* manifest = new std::string(backend().ManifestJson());
    return view(*manifest);
}

/* ---- bdk_runtime.h ---- */

BDK_EXPORT bdk_status_t bdk_runtime_start(const bdk_start_config_t* config, bdk_runtime_t* out,
                                          bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    if (!config || !out || config->struct_size < sizeof(size_t))
        return BDK_ERR_INVALID_ARGUMENT;
    *out = 0;
    Globals& g = G();
    if (g.runtime)
        return fail(out_error, BDK_DOMAIN_RUNTIME, BDK_CODE_ALREADY_STARTED,
                    "a runtime is already started");

    /* The callee reads only the bytes the caller declared; the rest reads as zero. */
    bdk_start_config_t c;
    memset(&c, 0, sizeof c);
    memcpy(&c, config, config->struct_size < sizeof c ? config->struct_size : sizeof c);
    if (!string_ok(c.resolved_config_json) || !string_ok(c.host_abi_hash))
        return BDK_ERR_INVALID_ARGUMENT;

    if (c.host_abi_version != BDK_ABI_VERSION) {
        return fail(out_error, BDK_DOMAIN_ABI, BDK_CODE_VERSION_MISMATCH,
                    "the host was built against ABI version " + std::to_string(c.host_abi_version) +
                        " and the runtime implements version " + std::to_string(BDK_ABI_VERSION) +
                        (c.host_abi_version < BDK_ABI_VERSION ? "; the host is older"
                                                              : "; the runtime is older"));
    }
    if (str(c.host_abi_hash) != BDK_ABI_HASH) {
        return fail(out_error, BDK_DOMAIN_ABI, BDK_CODE_HASH_MISMATCH,
                    "the host and the runtime were built from different headers");
    }
    std::string resolved = str(c.resolved_config_json);
    if (resolved.empty()) {
        return fail(out_error, BDK_DOMAIN_CONFIG, "resolvedMissing",
                    "bdk.resolved.json was not provided");
    }
    bool is_object = false;
    if (!valid_json(resolved, &is_object) || !is_object) {
        return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                    "the resolved configuration is not a JSON object");
    }

    auto rt = std::make_shared<Runtime>();
    if (bdk_status_t s = ui_configure(*rt, resolved, out_error))
        return s;
    if (bdk_status_t s = ui_configure_extensions(*rt, resolved, out_error))
        return s;
    if (bdk_status_t s = extension_configure(*rt, resolved, out_error))
        return s;
    if (bdk_status_t s = automation_configure(*rt, resolved, out_error))
        return s;
    if (bdk_status_t s = providers_configure(*rt, resolved, c, out_error))
        return s;
    if (bdk_status_t s = routes_configure(*rt, resolved, out_error))
        return s;
    ProfileVersionPolicy profile_policy;
    if (bdk_status_t s = profile_policy_configure(resolved, &profile_policy, out_error))
        return s;
    const std::string data_dir = backend().ProfileDataDirectory(c, resolved);
    const std::string engine_version = backend().EngineVersion();
    if (!data_dir.empty() && !engine_version.empty()) {
        if (bdk_status_t s =
                profile_versions_apply(data_dir, engine_version, profile_policy, out_error))
            return s;
    }
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.ui_set = true;
        g.ui = std::this_thread::get_id();
    }
    if (bdk_status_t s = backend().Start(*rt, c, resolved, out_error)) {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.ui_set = false;
        return s;
    }
    rt->handle = table_make(T_RUNTIME, rt);
    g.runtime = rt;
    *out = rt->handle;
    return BDK_OK;
}

BDK_EXPORT bdk_call_t bdk_runtime_shutdown(bdk_runtime_t h, bdk_result_cb cb, void* ctx,
                                           bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb)
        return 0;
    auto rt = runtime_of(h);
    if (!rt)
        return 0;
    emit_runtime_event(rt, BDK_RUNTIME_EVENT_SHUTDOWN_STARTED, "{}", true);
    rt->shutting = true;
    automation_shutdown(rt);
    routes_shutdown(*rt);
    auto call = new_call(rt, cb, nullptr, ctx, rel);
    call->done = true; /* completed by the task below; a cancel has no effect */

    /* Pending calls complete with broker.cancelled before the shutdown result. */
    for (auto& w : rt->calls) {
        auto c = w.lock();
        if (c && c != call && !c->done) {
            c->cancelled = true;
            run_on_stop(c);
            finish_call(c, BDK_DOMAIN_BROKER, BDK_CODE_CANCELLED, "the runtime shut down", "");
        }
    }
    for (auto& c : rt->contents) {
        c->dead = true;
        for (auto& s : c->subs)
            deactivate(s);
        release_later(c->key_ctx, c->key_rel);
        c->key_rel = nullptr;
    }
    for (auto& s : rt->subs)
        deactivate(s);
    for (auto& kv : rt->commands)
        release_later(kv.second.ctx, kv.second.rel);
    for (auto& kv : rt->page_handlers)
        release_later(kv.second.ctx, kv.second.rel);
    providers_shutdown(*rt); /* the providers hear of their cancelled calls before release */
    for (auto& kv : rt->providers)
        release_later(kv.second.ctx, kv.second.rel);
    for (auto& kv : rt->ui_handlers)
        release_later(kv.second.ctx, kv.second.rel);
    for (auto& r : rt->requests) {
        r->finished = true;
        release_withdrawn(*r);
        release_update(*r);
        r->on_update = nullptr;
    }
    /* As bdk_window_unregister does: the backend lets go of a window before it is freed (the
       engine parks its pages and closes its contexts, which point at it). */
    for (auto& w : rt->windows) {
        release_window_handlers(*w);
        backend().UnregisterWindow(*w);
    }
    rt->windows.clear();
    release_later(rt->log_ctx, rt->log_rel);
    rt->log_cb = nullptr;

    /* Every handle derived from the runtime is stale from here on. */
    for (auto& c : rt->contents)
        table_free(c->handle);
    for (auto& p : rt->profiles) {
        if (p->open)
            table_free(p->handle);
    }
    for (auto& r : rt->requests)
        table_free(r->handle);
    table_free(rt->handle);
    Globals& g = G();
    for (uint32_t i = 0; i < g.slots.size(); ++i) {
        Type t = g.slots[i].type;
        if (t == T_WINDOW || t == T_SESSION || t == T_CONTEXT || t == T_PROFILE || t == T_SUB ||
            t == T_TASK || t == T_HELLO || t == T_PROVIDER_CALL) {
            bdk_handle_t handle = (static_cast<uint64_t>(t) << 56u) |
                                  ((static_cast<uint64_t>(g.slots[i].gen) & kGenMask) << 32u) | i;
            table_free(handle);
        }
    }

    /* The backend stops after every release queued above has run. */
    post([rt, call] {
        backend().Shutdown(*rt, [rt, call] {
            Globals& g2 = G();
            g2.runtime.reset();
            {
                std::lock_guard<std::mutex> lock(g2.mutex);
                g2.ui_set = false;
            }
            table_free(call->handle);
            call->done = true;
            call->cb(call->ctx, 0, view(std::string("{}")), 0);
            if (call->rel)
                call->rel(call->ctx);
        });
    });
    return call->handle;
}

BDK_EXPORT bdk_status_t bdk_runtime_freeze_registrations(bdk_runtime_t h, bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (rt->frozen)
        return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_REGISTRATION_AFTER_START,
                    "the registrations are already frozen");
    if (bdk_status_t s = ui_check_handlers(*rt, out_error))
        return s;
    if (bdk_status_t s = providers_check_registrations(*rt, out_error))
        return s;
    if (bdk_status_t s = routes_check_handlers(*rt, out_error))
        return s;
    rt->frozen = true;
    emit_runtime_event(rt, BDK_RUNTIME_EVENT_REGISTRATIONS_FROZEN, "{}");
    log_line(rt, 1, "registrations frozen");
    return BDK_OK;
}

BDK_EXPORT bdk_subscription_t bdk_runtime_subscribe(bdk_runtime_t h, bdk_runtime_event_cb cb,
                                                    void* ctx, bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb)
        return 0;
    auto rt = runtime_of(h);
    if (!rt)
        return 0;
    auto s = std::make_shared<Sub>();
    s->rcb = cb;
    s->ctx = ctx;
    s->rel = rel;
    s->handle = table_make(T_SUB, s);
    rt->subs.push_back(s);
    return s->handle;
}

BDK_EXPORT void bdk_unsubscribe(bdk_subscription_t h) {
    if (check_thread() != BDK_OK)
        return;
    auto s = get<Sub>(h, T_SUB);
    if (s) {
        s->closing = false; /* an unsubscribed subscription is not called again */
        deactivate(s);
    }
}

BDK_EXPORT BDK_ANY_THREAD void bdk_post_to_ui_thread(void (*fn)(void*), void* ctx) {
    if (fn)
        post([fn, ctx] { fn(ctx); });
}

/* ---- bdk_broker.h ---- */

BDK_EXPORT bdk_call_t bdk_invoke(bdk_runtime_t h, uint32_t command_id, bdk_string_t args,
                                 const bdk_invoke_options_t* o, bdk_result_cb cb,
                                 bdk_progress_cb prog, void* ctx, bdk_release_cb rel) {
    return broker_invoke_abi(h, nullptr, command_id, args, o, cb, prog, ctx, rel);
}

BDK_EXPORT bdk_call_t bdk_invoke_named(bdk_runtime_t h, bdk_string_t name, bdk_string_t args,
                                       const bdk_invoke_options_t* o, bdk_result_cb cb,
                                       bdk_progress_cb prog, void* ctx, bdk_release_cb rel) {
    if (!string_ok(name))
        return 0;
    std::string n = str(name);
    return broker_invoke_abi(h, &n, 0, args, o, cb, prog, ctx, rel);
}

static bdk_subscription_t subscribe_impl(bdk_runtime_t h, uint32_t id, const std::string& name,
                                         bdk_string_t filter, const bdk_invoke_options_t* o,
                                         bdk_event_cb cb, void* ctx, bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb || !string_ok(filter))
        return 0;
    auto rt = runtime_of(h);
    if (!rt || !invoke_options_ok(o))
        return 0;
    std::string f = str(filter);
    if (!f.empty() && !valid_json(f))
        return 0;
    auto s = std::make_shared<Sub>();
    s->ecb = cb;
    s->ctx = ctx;
    s->rel = rel;
    s->event_id = id;
    s->event_name = name;
    /* o may declare fewer bytes than profile and contents need */
    const bdk_invoke_options_t options = read_invoke_options(o);
    s->profile = options.profile;
    s->contents = options.contents;
    s->handle = table_make(T_SUB, s);
    rt->subs.push_back(s);
    return s->handle;
}

BDK_EXPORT bdk_subscription_t bdk_subscribe(bdk_runtime_t h, uint32_t event_id, bdk_string_t filter,
                                            const bdk_invoke_options_t* o, bdk_event_cb cb,
                                            void* ctx, bdk_release_cb rel) {
    if (event_id == 0)
        return 0;
    return subscribe_impl(h, event_id, std::string(), filter, o, cb, ctx, rel);
}

BDK_EXPORT bdk_subscription_t bdk_subscribe_named(bdk_runtime_t h, bdk_string_t name,
                                                  bdk_string_t filter,
                                                  const bdk_invoke_options_t* o, bdk_event_cb cb,
                                                  void* ctx, bdk_release_cb rel) {
    if (!string_ok(name) || name.length == 0)
        return 0;
    return subscribe_impl(h, 0, str(name), filter, o, cb, ctx, rel);
}

BDK_EXPORT void bdk_app_command_progress(bdk_context_t h, double fraction, bdk_string_t message) {
    if (check_thread() != BDK_OK)
        return;
    auto cc = get<CallCtx>(h, T_CONTEXT);
    if (!cc || cc->call->cancelled || cc->call->delivered || !cc->call->prog)
        return;
    auto call = cc->call;
    std::string m = str(message);
    post([call, fraction, m] {
        if (!call->delivered && !call->cancelled)
            call->prog(call->ctx, fraction, view(m));
    });
}

BDK_EXPORT void bdk_app_command_resolve(bdk_context_t h, bdk_string_t output) {
    if (check_thread() != BDK_OK)
        return;
    auto cc = get<CallCtx>(h, T_CONTEXT);
    if (!cc)
        return;
    table_free(h);
    resolve_call(cc->call, str(output));
}

BDK_EXPORT void bdk_app_command_reject(bdk_context_t h, bdk_string_t domain, bdk_string_t code,
                                       bdk_string_t message) {
    if (check_thread() != BDK_OK)
        return;
    auto cc = get<CallCtx>(h, T_CONTEXT);
    if (!cc)
        return;
    table_free(h);
    std::string d = str(domain), c = str(code);
    if (d.empty())
        d = "broker";
    finish_call(cc->call, d.c_str(), c.c_str(), str(message), "");
}

BDK_EXPORT bdk_string_t bdk_app_command_caller_json(bdk_context_t h) {
    auto cc = get<CallCtx>(h, T_CONTEXT);
    return cc ? view(cc->caller) : bdk_string_t{nullptr, 0};
}

BDK_EXPORT bdk_profile_t bdk_app_command_profile(bdk_context_t h) {
    auto cc = get<CallCtx>(h, T_CONTEXT);
    auto p = cc ? cc->profile.lock() : nullptr;
    return p && p->open ? p->handle : 0;
}

BDK_EXPORT uint8_t bdk_app_command_is_cancelled(bdk_context_t h) {
    auto cc = get<CallCtx>(h, T_CONTEXT);
    return cc && cc->call->cancelled ? 1 : 0;
}

/* Registration functions: allowed between start and the freeze. On failure the caller keeps ctx and
   no release callback is called; on success BDK owns ctx and releases it once, when the runtime
   shuts down. */
static std::shared_ptr<Runtime> registration_runtime(bdk_runtime_t h, bdk_error_t* out_error,
                                                     bdk_status_t* status) {
    clear_out(out_error);
    *status = check_thread();
    if (*status != BDK_OK)
        return nullptr;
    auto rt = runtime_of(h);
    if (!rt) {
        *status = BDK_ERR_STALE_HANDLE;
        return nullptr;
    }
    if (rt->frozen) {
        *status = fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_REGISTRATION_AFTER_START,
                       "registrations are not allowed after bdk_runtime_freeze_registrations");
        return nullptr;
    }
    return rt;
}

static bool app_name(const std::string& n) {
    return n.size() > 4 && n.compare(0, 4, "app.") == 0;
}

BDK_EXPORT bdk_status_t bdk_app_command_register(bdk_runtime_t h, bdk_string_t name,
                                                 bdk_string_t decl, bdk_app_command_cb cb,
                                                 void* ctx, bdk_release_cb rel,
                                                 bdk_error_t* out_error) {
    bdk_status_t st;
    auto rt = registration_runtime(h, out_error, &st);
    if (!rt)
        return st;
    if (!cb || !string_ok(name) || !string_ok(decl) || !valid_json(str(decl)))
        return BDK_ERR_INVALID_ARGUMENT;
    std::string n = str(name);
    if (!app_name(n)) {
        return fail(out_error, BDK_DOMAIN_CONFIG, "nameNotApp",
                    "application command names start with 'app.'");
    }
    if (rt->commands.count(n))
        return fail(out_error, BDK_DOMAIN_CONFIG, "duplicateName",
                    "'" + n + "' is already registered");
    rt->commands[n] = Command{cb, ctx, rel};
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_app_event_declare(bdk_runtime_t h, bdk_string_t name, bdk_string_t decl,
                                              bdk_error_t* out_error) {
    bdk_status_t st;
    auto rt = registration_runtime(h, out_error, &st);
    if (!rt)
        return st;
    if (!string_ok(name) || !string_ok(decl) || !valid_json(str(decl)))
        return BDK_ERR_INVALID_ARGUMENT;
    std::string n = str(name);
    if (!app_name(n))
        return fail(out_error, BDK_DOMAIN_CONFIG, "nameNotApp",
                    "application event names start with 'app.'");
    if (rt->events.count(n))
        return fail(out_error, BDK_DOMAIN_CONFIG, "duplicateName",
                    "'" + n + "' is already declared");
    rt->events[n] = rt->next_event_id++;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_app_event_emit(bdk_runtime_t h, bdk_string_t name, bdk_string_t payload,
                                           bdk_string_t target, bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(name) || !string_ok(payload) || !string_ok(target))
        return BDK_ERR_INVALID_ARGUMENT;
    std::string n = str(name), p = str(payload), t = str(target);
    if (!valid_json(p) || (!t.empty() && !valid_json(t)))
        return BDK_ERR_INVALID_ARGUMENT;
    auto it = rt->events.find(n);
    if (it == rt->events.end())
        return fail(out_error, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                    "event '" + n + "' is not declared");
    uint32_t id = it->second;
    post([rt, n, id, p] {
        auto subs = rt->subs;
        for (auto& s : subs) {
            if (!s->active || !s->ecb)
                continue;
            if ((!s->event_name.empty() && s->event_name == n) ||
                (s->event_id != 0 && s->event_id == id)) {
                s->ecb(s->ctx, id, view(n), view(p));
            }
        }
        automation_deliver_event(rt, n, id, 0, 0, p);
    });
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_app_setting_register(bdk_runtime_t h, bdk_string_t decl,
                                                 bdk_error_t* out_error) {
    bdk_status_t st;
    auto rt = registration_runtime(h, out_error, &st);
    if (!rt)
        return st;
    bool is_object = false;
    if (!string_ok(decl) || !valid_json(str(decl), &is_object) || !is_object)
        return BDK_ERR_INVALID_ARGUMENT;
    return app_setting_declare(*rt, str(decl), out_error);
}

BDK_EXPORT bdk_status_t bdk_page_handler_register(bdk_runtime_t h, bdk_string_t route,
                                                  uint32_t mode, bdk_native_page_cb cb, void* ctx,
                                                  bdk_release_cb rel, bdk_error_t* out_error) {
    bdk_status_t st;
    auto rt = registration_runtime(h, out_error, &st);
    if (!rt)
        return st;
    if (!string_ok(route) || route.length == 0)
        return BDK_ERR_INVALID_ARGUMENT;
    std::string r = str(route);
    if (rt->page_handlers.count(r))
        return fail(out_error, BDK_DOMAIN_CONFIG, "duplicateName",
                    "route '" + r + "' is already registered");
    if (bdk_status_t s = routes_register(*rt, r, mode, cb, ctx, out_error))
        return s;
    rt->page_handlers[r] = Owned{ctx, rel};
    return BDK_OK;
}

BDK_EXPORT void bdk_native_page_complete(bdk_context_t h, uint32_t result) {
    if (check_thread() != BDK_OK)
        return;
    auto cc = get<CallCtx>(h, T_CONTEXT);
    if (!cc || !cc->native_done ||
        (result != BDK_NATIVE_PAGE_HANDLED && result != BDK_NATIVE_PAGE_BLOCKED))
        return;
    table_free(h);
    auto finish = std::move(cc->native_done);
    cc->native_done = nullptr;
    post([finish, result] { finish(result); });
}

BDK_EXPORT bdk_contents_t bdk_native_page_source(bdk_context_t h) {
    auto cc = get<CallCtx>(h, T_CONTEXT);
    return cc && contents_of(cc->source) ? cc->source : 0;
}

/* ---- bdk_provider.h, bdk_automation.h, bdk_diagnostics.h ---- */

BDK_EXPORT bdk_status_t bdk_provider_register(bdk_runtime_t h, bdk_string_t service,
                                              const bdk_provider_vtable_t* vtable, void* ctx,
                                              bdk_release_cb rel, bdk_error_t* out_error) {
    bdk_status_t st;
    auto rt = registration_runtime(h, out_error, &st);
    if (!rt)
        return st;
    if (!vtable || vtable->struct_size < sizeof(size_t) || !string_ok(service) ||
        service.length == 0) {
        return BDK_ERR_INVALID_ARGUMENT;
    }
    return provider_register(*rt, str(service), *vtable, ctx, rel, out_error);
}

BDK_EXPORT bdk_status_t bdk_provider_set_key(bdk_runtime_t h, bdk_string_t service,
                                             bdk_string_t key, bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(service) || !string_ok(key))
        return BDK_ERR_INVALID_ARGUMENT;
    /* No named provider in this runtime takes a key this way (T-PRV-08); the Google key is part
       of the start configuration. */
    return fail(out_error, BDK_DOMAIN_CONFIG, "keyUnused",
                "no provider of '" + str(service) +
                    "' in this runtime takes a key; the Google key is google_api_key in the start "
                    "configuration");
}

BDK_EXPORT bdk_status_t bdk_log_set_handler(bdk_runtime_t h, bdk_log_cb cb, void* ctx,
                                            bdk_release_cb rel, bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    release_later(rt->log_ctx, rt->log_rel);
    rt->log_cb = cb;
    rt->log_ctx = cb ? ctx : nullptr;
    rt->log_rel = cb ? rel : nullptr;
    if (!cb)
        release_later(ctx, rel);
    return BDK_OK;
}

} /* extern "C" */
