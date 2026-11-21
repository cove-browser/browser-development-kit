/* The runtime core: the part of the C ABI that is the same for every runtime. Platform-neutral
   C++23; no platform headers. It keeps the handle table, errors, buffers, asynchronous calls,
   subscriptions, registrations, the broker, UI requests and the argument checks of every ABI
   function, and leaves what a page actually does to a Backend. The stub runtime
   (packages/core/stub) and the Chromium engine (chromium/bdk) are the two backends; the conformance
   suite tests the core through both. */
#ifndef BDK_CORE_INTERNAL_H_
#define BDK_CORE_INTERNAL_H_

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bdk/bdk.h"

namespace bdkcore {

struct Request;

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
struct BuiltinCall; /* broker_internal.h */
struct RouteTable;  /* routes.h */

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
    bdk_profile_t profile = 0; /* events of this profile only; 0 for every profile */
    bool page = false; /* subscribed by a page: also sees application-wide events (profile 0), but
                          not settings.changed for a setting that is not exposed to pages */
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
    /* Runs once, from bdk_call_cancel, before the cancelled result is delivered: undoes what the
       call already handed out. */
    std::function<void()> on_cancel;
    /* Runs once when the call is cancelled, or torn down with its caller or the runtime, before
       the cancelled result is delivered: tells the service to stop. */
    std::function<void()> on_stop;
};

/* Runs the call's on_stop, if it has one and has not run it yet. */
inline void run_on_stop(const std::shared_ptr<Call>& c) {
    if (c->on_stop) {
        auto stop = std::move(c->on_stop);
        c->on_stop = nullptr;
        stop();
    }
}

struct Profile;

struct CallCtx {
    std::shared_ptr<Call> call;
    std::string caller = "{\"caller\":\"host\"}";
    std::weak_ptr<Profile> profile; /* the profile the call addresses, if any */
    /* A native page request (routes.h): its answer, and the page whose navigation asked. */
    std::function<void(uint32_t result)> native_done;
    bdk_contents_t source = 0;
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
    kWithdrawn = 5,
    kDisabled =
        6 /* the surface is disabled in the configuration: deny, report the API unavailable */
};

/* The configured mode of a surface (ui-requests.md section 4, configuration.md 3.6). */
enum class UiMode : uint8_t { kDefault = 0, kCustom = 1, kDisabled = 2 };

/* The requests of one kind for one page: Chromium queues them and the handler sees one at a time.
 */
struct UiQueue {
    bool busy = false;
    std::deque<std::shared_ptr<Request>> waiting;
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
    /* The gesture event of the completion being applied, for on_finish to check further (a
       recent mouse-up or key-down of this application). Borrowed: set only while on_finish runs
       for a completion, empty otherwise. */
    bdk_native_handle_t gesture{};
    bool delivered = false, finished = false;
    bool holds_slot = false; /* it is the one request of its kind and page the handler sees */
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

/* An application setting (`app.`), declared with bdk_app_setting_register. */
struct AppSetting {
    std::string key;
    std::string type;         /* boolean, number, string, array, object or json (any value) */
    std::string default_json; /* normalised JSON */
    bool application = false; /* scope: the whole application, not one profile */
    bool exposed = false;     /* exposedToPages: reachable by pages and sessions */
};

/* The values of one scope, kept by the core so that every backend behaves the same. The backend
   only persists them (Backend::LoadAppSettings, SaveAppSettings). */
struct AppSettingValues {
    bool loaded = false;
    std::map<std::string, std::string> values; /* key -> JSON */
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
    std::map<std::string, AppSetting> app_settings;
    std::map<std::string, AppSettingValues> app_values; /* by profile id; "" for the application */
    std::map<std::string, Owned> page_handlers, providers;
    std::shared_ptr<RouteTable> routes;                  /* routes.h */
    std::vector<std::weak_ptr<CallCtx>> native_contexts; /* native page requests not answered */
    std::map<std::string, std::string> provider_keys;
    std::map<uint32_t, UiHandler> ui_handlers;
    std::map<uint32_t, UiMode> ui_modes; /* by surface; a surface not listed is kDefault */
    std::map<std::pair<bdk_contents_t, uint32_t>, UiQueue> ui_queues;
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
    virtual bdk_status_t OpenPrivateProfile(Runtime& rt, const std::shared_ptr<Profile>& profile,
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
    /* scope is BDK_ZOOM_SCOPE_PAGE or BDK_ZOOM_SCOPE_SITE. */
    virtual bdk_status_t SetZoom(Contents& contents, double zoom, uint32_t scope,
                                 bdk_error_t* out_error) = 0;
    virtual bdk_status_t SetUserAgent(Contents& contents, const std::string& user_agent,
                                      bdk_error_t* out_error) = 0;
    virtual bdk_status_t Discard(Contents& contents, bdk_error_t* out_error) = 0;
    virtual bdk_status_t SetAutoDiscardable(Contents& contents, bool discardable,
                                            bdk_error_t* out_error) = 0;
    /* Find in page and printing, started by the application. Results are
       BDK_CONTENTS_EVENT_FIND_RESULT events. */
    virtual bdk_status_t FindStart(const std::shared_ptr<Contents>& contents,
                                   const std::string& text, bool match_case,
                                   bdk_error_t* out_error) = 0;
    virtual bdk_status_t FindNext(const std::shared_ptr<Contents>& contents, bool forward,
                                  bdk_error_t* out_error) = 0;
    virtual bdk_status_t FindStop(const std::shared_ptr<Contents>& contents, bool clear_selection,
                                  bdk_error_t* out_error) = 0;
    virtual bdk_status_t FindPresent(const std::shared_ptr<Contents>& contents,
                                     bdk_error_t* out_error) = 0;
    virtual bdk_status_t Print(const std::shared_ptr<Contents>& contents,
                               bdk_error_t* out_error) = 0;
    /* Page commands (BDK_PAGE_COMMAND_*, already checked to be one). CanPerformCommand answers as
       Chromium would enable the command now; the core refuses PerformCommand when it is false. */
    virtual bool CanPerformCommand(const std::shared_ptr<Contents>& contents, uint32_t command) = 0;
    virtual bdk_status_t PerformCommand(const std::shared_ptr<Contents>& contents, uint32_t command,
                                        bdk_error_t* out_error) = 0;
    virtual bdk_status_t ExitPictureInPicture(const std::shared_ptr<Contents>& contents,
                                              bdk_error_t* out_error) = 0;

    /* Views and windows. */
    virtual bdk_status_t NativeView(Contents& contents, bdk_native_handle_t* out,
                                    bdk_error_t* out_error) = 0;
    virtual bdk_status_t Focus(Runtime& rt, const std::shared_ptr<Contents>& contents) = 0;
    virtual bdk_status_t RegisterWindow(Runtime& rt, const Window& window,
                                        bdk_error_t* out_error) = 0;
    virtual void UnregisterWindow(const Window& window) = 0;

    /* Built-in commands (interface/<namespace>.json). The core has already found the command and
       checked the caller, the scope, the gesture and the arguments (broker_internal.h); the backend
       only does the work and answers through call->control, now or later, on the UI thread, exactly
       once. The default answers broker.notImplemented, so a backend implements commands one by one.
     */
    virtual void ExecuteCommand(Runtime& rt, const std::shared_ptr<BuiltinCall>& call);
    /* Application settings (`app.`) are answered by the core (app_settings.cc); the backend only
       keeps them. LoadAppSettings returns the JSON object last saved for the profile, or for the
       application when profile is null, and an empty string for none. SaveAppSettings stores the
       whole object. Neither is called for a private profile. The defaults keep nothing. */
    virtual std::string LoadAppSettings(Runtime& rt, const Profile* profile);
    virtual void SaveAppSettings(Runtime& rt, const Profile* profile, const std::string& json);
    /* Asks the user to authenticate with the operating system (a command marked A). done(true) if
       they did. Runs on the UI thread, later. The default refuses. */
    virtual void Authenticate(Runtime& rt, const std::string& reason,
                              std::function<void(bool)> done);
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
/* Withdraws one request if it is unfinished: the application's withdrawn-handler fires and the
   raiser learns kWithdrawn. For a backend whose own UI went away (a login delegate destroyed). */
void withdraw_ui_request(Request&);

/* For backends. */
/* Delivers a contents event later, in order, unless the page is gone by then. */
void emit_contents_event(const std::shared_ptr<Contents>&, uint32_t kind, const std::string& url,
                         const std::string& detail, int32_t net_error, uint32_t flags);
/* Delivers it now. Only from a task the backend already posted. */
void emit_contents_event_now(const std::shared_ptr<Contents>&, uint32_t kind,
                             const std::string& url, const std::string& detail, int32_t net_error,
                             uint32_t flags);
/* Raises a UI request (ui_router.cc). The router decides by the surface's mode: disabled finishes
   it as kDisabled with no handler called; otherwise the handler for the surface is called later,
   one request of a kind per page at a time, and with no handler the request is defaulted (the
   backend shows its default presenter). on_finish gets the outcome exactly once, always on a later
   task. A request with no on_finish is a notice: it is delivered at once and never queued. */
std::shared_ptr<Request> raise_ui_request(const std::shared_ptr<Runtime>&,
                                          std::shared_ptr<Request>);
/* Whether `completion` grants on `surface` (ui-requests.md section 5), and so needs a gesture. */
bool completion_grants(uint32_t surface, const std::string& completion);
/* Reads [ui.surfaces] from the resolved configuration; fails with ui.surfaceUnknown or
   ui.surfaceMode. Called by bdk_runtime_start before the backend starts. */
bdk_status_t ui_configure(Runtime&, const std::string& resolved_config, bdk_error_t* out_error);
/* Fails with config.handlerMissing when a custom surface has no handler. Called at the freeze. */
bdk_status_t ui_check_handlers(const Runtime&, bdk_error_t* out_error);
/* False when the configuration disables the surface: the web or extension API behind it must
   report itself unavailable instead of raising a request. */
bool ui_surface_available(const Runtime&, uint32_t surface);
/* bdk_ui_complete, bdk_ui_use_default and bdk_ui_request_release, after the handle checks. */
bdk_status_t ui_complete(Request&, const std::string& completion, const std::string& arguments,
                         bdk_native_handle_t gesture, bdk_error_t* out_error);
bdk_status_t ui_use_default(Request&, bdk_error_t* out_error);
void ui_release(Request&);
/* The keyboard handler of the page: the reserve answer, and the unhandled report. */
bool reserve_key(Contents&, bdk_native_handle_t key_event);
void report_unhandled_key(Contents&, bdk_native_handle_t key_event);

} // namespace bdkcore

#endif /* BDK_CORE_INTERNAL_H_ */
