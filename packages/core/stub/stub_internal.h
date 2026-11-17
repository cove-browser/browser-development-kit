/* Internal state of the in-memory stub runtime. Platform-neutral C++17; no platform headers. */
#ifndef BDK_STUB_INTERNAL_H_
#define BDK_STUB_INTERNAL_H_

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "bdk_stub.h"

namespace bdkstub {

enum Type : uint8_t {
    T_NONE = 0,
    T_ERROR,
    T_BUFFER,
    T_RUNTIME,
    T_PROFILE,
    T_CONTENTS,
    T_WINDOW,
    T_REQUEST,
    T_CALL,
    T_SUB,
    T_SESSION,
    T_CONTEXT
};

struct Err {
    std::string domain, code, message, details = "{}";
};
struct Buf {
    std::string data;
};

struct Runtime;
struct Contents;

struct Sub {
    bool active = true;
    void* ctx = nullptr;
    bdk_release_cb rel = nullptr;
    bdk_handle_t handle = 0;
    bdk_contents_event_cb ccb = nullptr;
    bdk_runtime_event_cb rcb = nullptr;
    bdk_event_cb ecb = nullptr;
    uint32_t event_id = 0;
    std::string event_name;
};

struct Call {
    bdk_result_cb cb = nullptr;
    bdk_progress_cb prog = nullptr;
    void* ctx = nullptr;
    bdk_release_cb rel = nullptr;
    bdk_handle_t handle = 0;
    bool done = false;
    bool delivered = false;
    bool cancelled = false;
};

struct CallCtx {
    std::shared_ptr<Call> call;
    std::string caller = "{\"caller\":\"host\"}";
};

struct Profile {
    std::string id, name;
    bool ephemeral = false;
    bool open = false;
    bdk_handle_t handle = 0;
};

struct Nav {
    bool cancelled = false;
};
struct Entry {
    std::string url, title;
};

struct Contents {
    bdk_handle_t handle = 0;
    std::weak_ptr<Runtime> rt;
    std::shared_ptr<Profile> profile;
    std::vector<Entry> entries;
    int current = -1;
    std::string url_buf, title_buf, user_agent;
    bool loading = false, muted = false, focused = false, closed = false, dead = false;
    bool auto_discardable = true, discarded = false;
    double progress = 0.0, zoom = 1.0;
    std::shared_ptr<Nav> nav;
    std::vector<std::shared_ptr<Sub>> subs;
    bdk_key_reserve_cb key_reserve = nullptr;
    bdk_key_unhandled_cb key_unhandled = nullptr;
    void* key_ctx = nullptr;
    bdk_release_cb key_rel = nullptr;
};

struct Command {
    bdk_app_command_cb cb;
    void* ctx;
    bdk_release_cb rel;
};
struct Owned {
    void* ctx = nullptr;
    bdk_release_cb rel = nullptr;
};
struct UiHandler {
    bdk_ui_handler_cb cb;
    void* ctx;
    bdk_release_cb rel;
};

struct Request {
    bdk_handle_t handle = 0;
    uint32_t surface = 0;
    bdk_contents_t contents = 0;
    bdk_profile_t profile = 0;
    std::string payload;
    bool delivered = false, finished = false;
    void (*withdrawn)(void*) = nullptr;
    void* wctx = nullptr;
    bdk_release_cb wrel = nullptr;
};

struct Window {
    uint32_t kind = 0;
    bdk_native_handle_t native{};
};
struct Session {
    bool revoked = false;
    std::set<bdk_contents_t> contents;
};

struct Runtime {
    bdk_runtime_t handle = 0;
    bool frozen = false, shutting = false;
    uint64_t next_id = 1;
    std::vector<std::shared_ptr<Profile>> profiles;
    std::vector<std::shared_ptr<Contents>> contents;
    std::vector<std::weak_ptr<Call>> calls;
    std::vector<std::shared_ptr<Sub>> subs;
    std::map<std::string, Command> commands;
    std::map<std::string, uint32_t> events; /* declared application events -> identifier */
    uint32_t next_event_id = 0x80000000u;
    std::vector<std::string> settings;
    std::map<std::string, Owned> page_handlers, providers;
    std::map<std::string, std::string> provider_keys;
    std::map<uint32_t, UiHandler> ui_handlers;
    std::vector<std::shared_ptr<Request>> requests;
    std::map<std::string, std::pair<int32_t, std::string>> scripts;
    bdk_stub_ui_outcome_t outcome{};
    bdk_log_cb log_cb = nullptr;
    void* log_ctx = nullptr;
    bdk_release_cb log_rel = nullptr;
};

/* Infrastructure */
bdk_handle_t table_make(Type, std::shared_ptr<void>);
std::shared_ptr<void> table_get(bdk_handle_t, Type);
Type table_type(bdk_handle_t); /* T_NONE if stale */
bool table_free(bdk_handle_t);
template <class T> std::shared_ptr<T> get(bdk_handle_t h, Type t) {
    return std::static_pointer_cast<T>(table_get(h, t));
}
void post(std::function<void()>);
void release_later(void* ctx, bdk_release_cb rel);
bdk_status_t check_thread();

std::string str(bdk_string_t);
bdk_string_t view(const std::string&);
bool string_ok(bdk_string_t);
bool valid_json(const std::string&, bool* is_object = nullptr);
std::string json_escape(const std::string&);

bdk_error_t make_error(const std::string& domain, const std::string& code,
                       const std::string& message);
bdk_status_t fail(bdk_error_t* out, const char* domain, const char* code,
                  const std::string& message);
void clear_out(bdk_error_t* out);

std::shared_ptr<Runtime> current_runtime();
std::shared_ptr<Runtime> runtime_of(bdk_runtime_t);
std::shared_ptr<Call> new_call(const std::shared_ptr<Runtime>&, bdk_result_cb, bdk_progress_cb,
                               void* ctx, bdk_release_cb);
void finish_call(const std::shared_ptr<Call>&, const char* domain, const char* code,
                 const std::string& message, const std::string& result_json);
void emit_runtime_event(const std::shared_ptr<Runtime>&, uint32_t kind, const std::string& detail,
                        bool to_closing = false);
void log_line(const std::shared_ptr<Runtime>&, uint32_t level, const std::string& message);

/* Subscriptions and teardown shared by the files */
void deactivate(const std::shared_ptr<Sub>&);
void destroy_contents(const std::shared_ptr<Runtime>&, const std::shared_ptr<Contents>&);
void close_profile(const std::shared_ptr<Runtime>&, const std::shared_ptr<Profile>&);
void emit_contents_event_now(const std::shared_ptr<Contents>&, uint32_t kind,
                             const std::string& url, const std::string& detail, int32_t net_error,
                             uint32_t flags);

} // namespace bdkstub

#endif /* BDK_STUB_INTERNAL_H_ */
