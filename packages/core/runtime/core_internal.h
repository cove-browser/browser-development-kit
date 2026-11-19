/* The runtime core: the part of the C ABI that is the same for every runtime. Platform-neutral
   C++23; no platform headers. It keeps the handle table, errors, buffers, asynchronous calls,
   subscriptions, registrations, the broker, UI requests and the argument checks of every ABI
   function, and leaves what a page actually does to a Backend. The stub runtime
   (packages/core/stub) and the Chromium engine (chromium/bdk) are the two backends; the conformance
   suite tests the core through both. */
#ifndef BDK_CORE_INTERNAL_H_
#define BDK_CORE_INTERNAL_H_

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "bdk/bdk.h"

namespace bdkcore {

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
    bool closing = false; /* active at shutdown: owed the closing event, then released */
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

/* What a backend keeps per profile or per page. */
struct BackendData {
    virtual ~BackendData() = default;
};

struct Profile {
    std::string id, name;
    bool ephemeral = false;
    bool open = false;
    bdk_handle_t handle = 0;
    std::unique_ptr<BackendData> data;
};

struct Entry {
    std::string url, title;
};

/* What bdk_contents_state reports, filled by the backend. */
struct ContentsState {
    std::string url = "about:blank", title;
    bool loading = false;
    double progress = 0.0;
    uint32_t security = BDK_SECURITY_UNKNOWN;
    uint32_t audio = 0;
    uint32_t visibility = BDK_VISIBILITY_VISIBLE;
    bool focused = false;
    double zoom = 1.0;
};

struct Contents {
    bdk_handle_t handle = 0;
    std::weak_ptr<Runtime> rt;
    std::shared_ptr<Profile> profile;
    std::string url_buf, title_buf; /* what the last bdk_contents_state lent out */
    bool closed = false, dead = false;
    std::vector<std::shared_ptr<Sub>> subs;
    bdk_key_reserve_cb key_reserve = nullptr;
    bdk_key_unhandled_cb key_unhandled = nullptr;
    void* key_ctx = nullptr;
    bdk_release_cb key_rel = nullptr;
    std::unique_ptr<BackendData> data;
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

/* How a UI request ended, reported to whoever raised it. */
enum class UiOutcome : uint8_t {
    kNone = 0,
    kCompleted = 1,
    kDefaulted = 2,
    kRejected = 3,
    kCancelled = 4,
    kWithdrawn = 5
};

struct Request {
    bdk_handle_t handle = 0;
    uint32_t surface = 0;
    bdk_contents_t contents = 0;
    bdk_profile_t profile = 0;
    bdk_window_t window = 0;
    std::string payload;
    double anchor[4] = {0, 0, 0, 0};
    bdk_native_handle_t secure_view{};
    bool delivered = false, finished = false;
    void (*withdrawn)(void*) = nullptr;
    void* wctx = nullptr;
    bdk_release_cb wrel = nullptr;
    /* Called once, with the outcome and, for kCompleted, the completion and its arguments. */
    std::function<void(UiOutcome, const std::string& completion, const std::string& args)>
        on_finish;
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
    bdk_log_cb log_cb = nullptr;
    void* log_ctx = nullptr;
    bdk_release_cb log_rel = nullptr;
};

/* Done callback of an asynchronous backend operation: an empty code is success. */
using Done = std::function<void(const std::string& code, const std::string& message)>;

/* What differs between runtimes. Every method runs on the UI thread, except Post. The core has
   already checked the thread, the handles and the arguments; a backend checks only what depends on
   its own state. A method that fails with BDK_ERR_FAILED fills out_error through fail(). */
class Backend {
  public:
    virtual ~Backend() = default;

    /* Runs task later on the UI thread, after every task posted before it. Any thread. */
    virtual void Post(std::function<void()> task) = 0;

    /* Called by bdk_runtime_start once the ABI and the resolved configuration are checked. Fills
       rt.profiles with the persistent profiles and sets rt.next_id above every identifier used. */
    virtual bdk_status_t Start(Runtime& rt, const bdk_start_config_t& config,
                               const std::string& resolved_config, bdk_error_t* out_error) = 0;
    /* Called once the core has made every handle stale and queued every release. The backend
       closes what is left and stops; done runs on the UI thread afterwards. */
    virtual void Shutdown(Runtime& rt, std::function<void()> done) = 0;
    /* The manifest of the runtime, as JSON. Any thread. */
    virtual std::string ManifestJson() = 0;

    /* Profiles. The core keeps the list and the names; the backend keeps what is on disk. */
    virtual void ProfilesChanged(Runtime& rt) = 0;
    virtual void OpenProfile(Runtime& rt, const std::shared_ptr<Profile>& profile, Done done) = 0;
    virtual bdk_status_t OpenPrivateProfile(Runtime& rt, Profile& profile,
                                            bdk_error_t* out_error) = 0;
    /* After the profile's contents are destroyed. */
    virtual void CloseProfile(Runtime& rt, Profile& profile) = 0;
    virtual void DeleteProfile(Runtime& rt, const Profile& profile, Done done) = 0;

    /* Pages. Events go through emit_contents_event, which delivers them later. */
    virtual bdk_status_t CreateContents(Runtime& rt, const std::shared_ptr<Contents>& contents,
                                        const std::string& initial_url, bdk_error_t* out_error) = 0;
    /* The page is asked to close; BDK_CONTENTS_EVENT_CLOSED follows. */
    virtual void CloseContents(const std::shared_ptr<Contents>& contents) = 0;
    /* The handle is already stale and the subscriptions released. */
    virtual void DestroyContents(Runtime& rt, Contents& contents) = 0;
    virtual bdk_status_t Load(const std::shared_ptr<Contents>& contents, const std::string& url,
                              bdk_error_t* out_error) = 0;
    virtual bdk_status_t Reload(const std::shared_ptr<Contents>& contents, bool bypass_cache,
                                bdk_error_t* out_error) = 0;
    virtual void Stop(const std::shared_ptr<Contents>& contents) = 0;
    /* index is within the entries. */
    virtual bdk_status_t GoToIndex(const std::shared_ptr<Contents>& contents, int index,
                                   bdk_error_t* out_error) = 0;
    /* current is -1 when there is no entry. */
    virtual void Entries(const Contents& contents, int* current, std::vector<Entry>* entries) = 0;
    virtual void State(const Contents& contents, ContentsState* state) = 0;
    virtual std::string SessionState(const Contents& contents) = 0;
    virtual bdk_status_t Restore(const std::shared_ptr<Contents>& contents, bdk_bytes_t state,
                                 bdk_error_t* out_error) = 0;
    virtual bdk_status_t SetMuted(Contents& contents, bool muted, bdk_error_t* out_error) = 0;
    virtual bdk_status_t SetZoom(Contents& contents, double zoom, bdk_error_t* out_error) = 0;
    virtual bdk_status_t SetUserAgent(Contents& contents, const std::string& user_agent,
                                      bdk_error_t* out_error) = 0;
    virtual bdk_status_t Discard(Contents& contents, bdk_error_t* out_error) = 0;
    virtual bdk_status_t SetAutoDiscardable(Contents& contents, bool discardable,
                                            bdk_error_t* out_error) = 0;

    /* Views and windows. */
    virtual bdk_status_t NativeView(Contents& contents, bdk_native_handle_t* out,
                                    bdk_error_t* out_error) = 0;
    virtual bdk_status_t Focus(Runtime& rt, const std::shared_ptr<Contents>& contents) = 0;
    virtual bdk_status_t RegisterWindow(Runtime& rt, const Window& window,
                                        bdk_error_t* out_error) = 0;
    virtual void UnregisterWindow(const Window& window) = 0;
};

/* The runtime that links the core defines this. */
Backend& backend();

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
bdk_buffer_t make_buffer(const std::string& data);

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
std::shared_ptr<Contents> contents_of(bdk_contents_t);
/* Runs a request's withdrawn-handler release exactly once; later calls do nothing. */
void release_withdrawn(Request&);
/* Withdraws every unfinished request raised for the contents (0: every request). */
void withdraw_contents_requests(const std::shared_ptr<Runtime>&, bdk_contents_t);

/* For backends. */
/* Delivers a contents event later, in order, unless the page is gone by then. */
void emit_contents_event(const std::shared_ptr<Contents>&, uint32_t kind, const std::string& url,
                         const std::string& detail, int32_t net_error, uint32_t flags);
/* Delivers it now. Only from a task the backend already posted. */
void emit_contents_event_now(const std::shared_ptr<Contents>&, uint32_t kind,
                             const std::string& url, const std::string& detail, int32_t net_error,
                             uint32_t flags);
/* Raises a UI request: the handler for the surface is called later; with none, the request is
   defaulted. on_finish gets the outcome exactly once. */
std::shared_ptr<Request> raise_ui_request(const std::shared_ptr<Runtime>&,
                                          std::shared_ptr<Request>);
/* The keyboard handler of the page: the reserve answer, and the unhandled report. */
bool reserve_key(Contents&, bdk_native_handle_t key_event);
void report_unhandled_key(Contents&, bdk_native_handle_t key_event);

} // namespace bdkcore

#endif /* BDK_CORE_INTERNAL_H_ */
