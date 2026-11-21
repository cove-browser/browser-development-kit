/* The broker: one place that finds a command, checks the caller and starts the service. Platform
   neutral C++23 like the rest of the core. The rules are services.md section 2 and architecture.md
   section 9; the decisions taken are in packages/core/docs/conventions.md ("The broker").

   Commands come from two places. Built-in commands are defined once in packages/core/interface/ and
   reach the core as generated tables (generated/bdk_command_table.inc); a Backend does the work.
   Application commands (`app.`) are registered from the host and run its handler. Both go through
   the same checks, in the order of services.md section 2:

     lookup, then caller identity, capability, scope, gesture or authentication, argument
     validation, and only then the service. */
#ifndef BDK_BROKER_INTERNAL_H_
#define BDK_BROKER_INTERNAL_H_

#include <set>
#include <string>
#include <vector>

#include "core_internal.h"

namespace bdkcore {

namespace table {
struct CommandDef;
}

/* Arguments and results are each capped at this many bytes of JSON (services.md section 2:
   "a few megabytes"). A larger payload is broker.payloadTooLarge. Binary data travels outside the
   JSON value and is not counted. */
constexpr size_t kMaxPayloadBytes = size_t{4} * 1024 * 1024;

/* Who is calling. The host (Swift, through the C ABI) has full authority. A page and a session
   carry a capability set and a scope; their transports arrive in M4 and M7, and until then only the
   stub's test hooks create them. */
enum class CallerKind : uint8_t { kHost, kPage, kSession };

/* What a caller may address. A page is confined to its own profile and addresses no other page. */
struct Scope {
    bool all_profiles = false;
    std::set<bdk_profile_t> profiles;
    bool all_contents = false;
    std::set<bdk_contents_t> contents;
};

struct Caller : std::enable_shared_from_this<Caller> {
    /* Out of line, as for CommandControl: Chromium's style plugin refuses implicit ones here. */
    Caller();
    ~Caller();

    CallerKind kind = CallerKind::kHost;
    std::string name;
    bdk_profile_t profile = 0; /* a page: the profile it is in */
    /* Capability names, and `app.<prefix>.*` patterns. There is no other wildcard. */
    std::vector<std::string> capabilities;
    Scope scope; /* a session */
    bool revoked = false;
    std::vector<std::weak_ptr<Call>> calls; /* outstanding calls, cancelled when revoked */
};

/* The host's caller: full authority. */
std::shared_ptr<Caller> host_caller();

/* Why a capability list may not be granted to a caller of this kind, or empty if it may: a wildcard
   outside `app.`, or a capability on the never-for-pages or never-for-sessions list. */
std::string grant_error(CallerKind kind, const std::vector<std::string>& capabilities);

/* Whether the caller holds the capability: by name, or through an `app.<prefix>.*` pattern. */
bool capability_granted(const Caller& caller, const std::string& capability);

/* How a backend answers a built-in command. Every method runs on the UI thread. The first answer
   wins: after the call is cancelled, or answered, the rest are ignored. */
class CommandControl {
  public:
    /* Out of line: the engine compiles this header with Chromium's style plugin, which refuses an
       inline constructor or implicit destructor on a class with members like these. */
    CommandControl(std::shared_ptr<Call> call, bool progress);
    ~CommandControl();

    /* Reports progress; ignored unless the command is marked P. fraction is clamped to 0..1. */
    void Progress(double fraction, const std::string& message);
    /* Answers with a JSON value (at most kMaxPayloadBytes). */
    void Resolve(const std::string& result_json);
    /* Answers a command marked binary with a JSON value and bytes outside it (not counted against
       kMaxPayloadBytes): the host's result callback gets them as its buffer, a session base64 in
       the result frame. */
    void ResolveBinary(const std::string& result_json, std::string bytes);
    void Reject(const char* domain, const char* code, const std::string& message);
    /* True once the caller cancelled, the caller was revoked or the runtime shut down. */
    bool cancelled() const;
    /* Called once if the call is cancelled or torn down before it is answered: stop working. */
    void OnCancel(std::function<void()> stop);
    /* The core's side of OnCancel: runs the stop function, if there is one. */
    void Stop();

  private:
    std::shared_ptr<Call> call_;
    bool progress_;
    std::function<void()> stop_;
};

/* One built-in command call, as handed to Backend::ExecuteCommand. */
struct BuiltinCall {
    const table::CommandDef* def = nullptr;
    uint32_t id = 0;
    std::string name;
    std::string arguments; /* valid JSON; an object that matches the command's argument types */
    std::shared_ptr<Profile> profile;   /* null for a command with profile "none" */
    std::shared_ptr<Contents> contents; /* null unless the caller named a page */
    std::shared_ptr<Caller> caller;
    std::shared_ptr<CommandControl> control;
};

/* A command is named or numbered. */
struct CommandRef {
    std::string name; /* used when non-empty */
    uint32_t id = 0;
};

/* Whether the options name live objects. */
bool invoke_options_ok(const bdk_invoke_options_t*);
/* The options as this version reads them: only the struct_size bytes the caller declared, the
   rest zero. All zero for null. Call after invoke_options_ok. */
bdk_invoke_options_t read_invoke_options(const bdk_invoke_options_t*);

/* bdk_invoke and bdk_invoke_named: the call of the host, with full authority. name is null when the
   command is named by id. Returns 0 for a synchronous rejection (stale handle, wrong thread). */
bdk_call_t broker_invoke_abi(bdk_runtime_t h, const std::string* name, uint32_t id,
                             bdk_string_t arguments, const bdk_invoke_options_t* options,
                             bdk_result_cb cb, bdk_progress_cb prog, void* ctx, bdk_release_cb rel);

/* Answers a call with a JSON value: checks that it is JSON and within kMaxPayloadBytes first. */
void resolve_call(const std::shared_ptr<Call>& call, const std::string& result_json);

/* Starts a call for the caller. Returns the call handle, or 0 when the call is rejected
   synchronously (nothing is called back). gesture is whether the call comes from a user gesture
   in the caller's page. The call is dispatched later, on the UI thread, in the order submitted. */
bdk_call_t broker_submit(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Caller>& caller,
                         const CommandRef& command, const std::string& arguments_json,
                         const bdk_invoke_options_t* options, bool gesture, bdk_result_cb cb,
                         bdk_progress_cb prog, void* ctx, bdk_release_cb rel);

/* A caller ends (its page navigated or closed, its session was revoked): its outstanding calls are
   cancelled with broker.cancelled and their services told to stop. Later calls fail with
   capability.denied. */
void broker_revoke_caller(const std::shared_ptr<Caller>& caller);

/* Emits a built-in event (an event of the interface definitions) to the subscriptions for it, by
   name or by identifier. A subscription made for a profile gets only that profile's events;
   `profile` is the handle of the profile the event happened in. Delivered later, on the UI thread,
   in order. For backends. Returns false for a name that is not a built-in event. */
bool emit_builtin_event(const std::shared_ptr<Runtime>&, const std::string& name,
                        bdk_profile_t profile, const std::string& payload_json);

/* ---- Argument checking (broker_args.cc) ---- */

/* An object member. Not std::pair: a pair of an incomplete JsonValue is ill-formed, and
   libstdc++ rejects it. */
struct JsonMember;

/* A parsed JSON value. Only what argument checking needs. */
struct JsonValue {
    enum class Type : uint8_t { kNull, kBool, kNumber, kString, kArray, kObject };
    Type type = Type::kNull;
    bool boolean = false;
    double number = 0;
    bool integral = false;           /* a number written without fraction or exponent */
    std::string text;                /* a string's value */
    std::vector<JsonValue> items;    /* an array's */
    std::vector<JsonMember> members; /* an object's, in order */

    const JsonValue* find(const std::string& key) const;
};

struct JsonMember {
    std::string first;
    JsonValue second;
};

/* Parses text that valid_json() accepted. */
bool json_parse(const std::string& text, JsonValue* out);

/* Writes a parsed value back as JSON text. Numbers are written in their shortest exact form. */
std::string json_serialize(const JsonValue& value);

/* Checks parsed arguments against a built-in command's argument types. Empty when they match;
   otherwise a message naming the first problem and where it is. */
std::string check_arguments(const table::CommandDef& def, const JsonValue& arguments);

/* ---- Application settings (app_settings.cc) ---- */

/* Declares an application setting from bdk_app_setting_register's JSON:
   {"key": "app.…", "default": <json>, "scope": "profile" | "application", "exposedToPages": bool}.
   Fails with config.nameNotApp and config.duplicateName; the type is that of the default. */
bdk_status_t app_setting_declare(Runtime& rt, const std::string& declaration,
                                 bdk_error_t* out_error);

/* Whether a non-host caller may make this settings.* call: false (with a message) when it names an
   application setting that is not exposed to pages. A key that is not registered is not refused
   here; the call answers null or an error like any unknown key. */
bool app_settings_exposed(const Runtime& rt, const JsonValue& arguments, std::string* message);

/* Answers a settings.* call that names application settings (`app.` keys), and returns true.
   Returns false for any other call, which the backend answers. */
bool app_settings_execute(Runtime& rt, const std::shared_ptr<BuiltinCall>& call,
                          const JsonValue& arguments);

/* Forgets the cached values of a profile that closed (they are on disk). */
void app_settings_forget(Runtime& rt, const std::string& profile_id);

} // namespace bdkcore

#endif /* BDK_BROKER_INTERNAL_H_ */
