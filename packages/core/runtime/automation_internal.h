/* Automation (M7): sessions, tasks, the wire protocol shared by the script host and the external
   endpoint, user scripts and the core's own contents.* commands. Platform-neutral C++23 like the
   rest of the core. The rules are architecture.md section 9.3, services.md sections 5.15 to 5.17
   and javascript-api.md section 5; the decisions taken are in packages/core/docs/conventions.md
   ("Automation"). */
#ifndef BDK_AUTOMATION_INTERNAL_H_
#define BDK_AUTOMATION_INTERNAL_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "broker_internal.h"

namespace bdkcore {

/* The wire protocol's version, sent in `welcome` and `start`. */
constexpr int kProtocolVersion = 1;
/* The largest frame either side may send. A larger one closes the connection. */
constexpr size_t kMaxFrameBytes = size_t{32} * 1024 * 1024;

/* A persistent user script (services.md 5.16, interface/scripts.json), as the core keeps it. */
struct UserScript {
    std::string id;
    std::vector<std::string> matches, excludes;
    std::string run_at = "documentIdle"; /* documentStart, documentEnd or documentIdle */
    std::string world;                   /* "page", or an isolated world's name (default: the id) */
    bool all_frames = false;
    std::vector<std::string> javascript, css;
    std::string owner; /* the id of the session that registered it; empty for the application */
};

struct AutomationTask;

/* What a backend's automation host sees of one task. The core has already checked the session,
   the configuration and that the script is listed. The backend starts the script, sets to_script
   before StartTask returns, and hands every frame the script sends to FromScript, on the UI
   thread. Everything here runs on the UI thread. */
class TaskPort {
  public:
    /* Out of line, for the engine's style plugin (see CommandControl). */
    explicit TaskPort(std::weak_ptr<AutomationTask> task);
    ~TaskPort();

    /* The task's identifier, as the `start` frame names it. */
    const std::string& id() const;
    /* The script's name: a key of the resolved configuration's automation.scripts. */
    const std::string& script() const;
    /* That entry, as JSON: {"path","digest"} and, once `bdk build automation` ran,
       {"bundle" (a path inside Contents/Resources), "bundle_digest" ("sha256:<hex>")}. */
    const std::string& entry_json() const;
    /* The arguments the application passed, as JSON. */
    const std::string& arguments_json() const;

    /* Frames from BDK to the script, in order. Set by the backend. */
    std::function<void(const std::string& frame)> to_script;
    /* A frame the script sent. */
    void FromScript(const std::string& frame);
    /* The host could not start the script or went away: the task finishes with
       automation.hostGone and this message. */
    void HostGone(const std::string& message);
    /* The script could not be loaded (missing bundle, digest mismatch, a syntax error): the task
       finishes with automation.scriptFailed and this message. */
    void ScriptFailed(const std::string& message);

  private:
    std::weak_ptr<AutomationTask> task_;
    std::string empty_;
};

/* ---- For the backends ---- */

/* The JSON identifier of a page (the decimal handle; JSON numbers cannot carry 64 bits). */
std::string contents_id(bdk_contents_t contents);
/* The page a JSON identifier names, or null. */
std::shared_ptr<Contents> contents_from_id(const std::string& id);

/* The resolved configuration's [devtools] protocol: false refuses debug.attach to every caller
   (devtools.protocolDisabled). */
bool devtools_protocol_enabled(const Runtime& rt);

/* The user scripts registered for a profile, in registration order. */
const std::vector<UserScript>& user_scripts(const Runtime& rt, const Profile& profile);

/* ---- The core's side (automation.cc, contents_commands.cc) ---- */

/* Reads [automation] and [devtools] from the resolved configuration. Called by bdk_runtime_start
   before the backend starts; fails with config.resolvedInvalid on a bad shape. */
bdk_status_t automation_configure(Runtime& rt, const std::string& resolved_config,
                                  bdk_error_t* out_error);
/* bdk_release of a task handle: cancels it if it runs, and frees the handle. */
bdk_status_t task_release(bdk_task_t task);
/* Ends every session, task, connection and the endpoint. Called at the start of shutdown. */
void automation_shutdown(const std::shared_ptr<Runtime>& rt);
/* An event happened (built-in, or an application's `app.` event): deliver it to the sessions
   subscribed to it, by capability and scope; to the one session `only` when it is not null. Runs
   on the UI thread, from the task that delivers it to the host. */
void automation_deliver_event(const std::shared_ptr<Runtime>& rt, const std::string& name,
                              uint32_t id, bdk_profile_t profile, bdk_contents_t contents,
                              const std::string& payload_json, const Caller* only = nullptr);
/* Replaces the endpoint connections' backlog limits (state events are coalesced past the soft
   one, a connection past the hard one is closed); zero keeps the default. A hook for tests, which
   cannot wait for megabytes: applies to the open endpoint and to one opened later. */
void automation_set_endpoint_limits(const std::shared_ptr<Runtime>& rt, size_t soft_bytes,
                                    size_t hard_bytes);
/* A page closed or was destroyed: forget it in every session's scope. */
void automation_forget_contents(const std::shared_ptr<Runtime>& rt, bdk_contents_t contents);
/* A contents event the core emitted: also emitted as the matching contents.* built-in event. */
void automation_contents_event(const std::shared_ptr<Contents>& contents, uint32_t kind,
                               const std::string& url, const std::string& detail, uint32_t flags);
/* A UI request was raised for a page: contents.dialogOpened. */
void automation_request_raised(const std::shared_ptr<Runtime>& rt, const Request& request);

/* The commands the core answers itself, before the backend: contents.* that need no renderer,
   scripts.*, and the core's checks of debug.*. Returns true when it answered (or rejected) the
   call; false hands it to Backend::ExecuteCommand. */
bool core_execute_command(Runtime& rt, const std::shared_ptr<BuiltinCall>& call,
                          const JsonValue& arguments);

/* Emits a built-in event that concerns one page, so that sessions see it only when the page is in
   their scope. Otherwise as emit_builtin_event. */
bool emit_builtin_event_for(const std::shared_ptr<Runtime>&, const std::string& name,
                            bdk_profile_t profile, bdk_contents_t contents,
                            const std::string& payload_json);

/* Emits a built-in event to one caller only: the host's subscriptions when caller is the host,
   else that session's. For events that belong to whoever started something (debug.event and
   debug.detached go to the caller that attached). Returns false for a name that is not a built-in
   event. */
bool emit_builtin_event_to(const std::shared_ptr<Runtime>&, const std::shared_ptr<Caller>& caller,
                           const std::string& name, bdk_profile_t profile, bdk_contents_t contents,
                           const std::string& payload_json);

} // namespace bdkcore

#endif /* BDK_AUTOMATION_INTERNAL_H_ */
