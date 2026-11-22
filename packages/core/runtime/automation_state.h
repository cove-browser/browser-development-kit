/* What automation.cc and contents_commands.cc share: the per-runtime automation state. Private to
   the runtime core; backends use automation_internal.h. */
#ifndef BDK_AUTOMATION_STATE_H_
#define BDK_AUTOMATION_STATE_H_

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "automation_internal.h"
#include "local_socket.h"

namespace bdkcore {

class Connection;

/* A session (bdk_session_t): a broker caller with a scope, and what it started. */
struct Session {
    enum class Pages : uint8_t { kAll, kCreated, kExplicit };
    bdk_session_t handle = 0;
    /* Unique for the runtime's life, unlike the name: what owns its user scripts. */
    std::string id;
    std::weak_ptr<Runtime> rt;
    std::shared_ptr<Caller> caller;
    Pages pages = Pages::kCreated;
    bool revoked = false;
    std::vector<std::weak_ptr<AutomationTask>> tasks;
    std::vector<std::weak_ptr<Connection>> connections;
};

/* One running script. */
struct AutomationTask : std::enable_shared_from_this<AutomationTask> {
    bdk_task_t handle = 0;
    std::string id, script, entry_json, arguments_json;
    uint32_t cpu_budget_ms = 0; /* the CPU time its script may use */
    std::weak_ptr<Runtime> rt;
    std::shared_ptr<Session> session;
    std::shared_ptr<Connection> connection; /* its protocol connection to the script */
    std::shared_ptr<TaskPort> port;
    bool started = false, finished = false;
    std::string finish_detail; /* the FINISHED event's JSON */
    std::vector<std::shared_ptr<Sub>> subs;
};

/* A page operation waiting for the page (contents.navigate, contents.waitFor). on_event returns
   true when the wait is over; on_gone runs if the page closes or goes away first. */
struct ContentsWaiter {
    bdk_contents_t contents = 0;
    bool done = false;
    std::function<bool(uint32_t kind, const std::string& url, const std::string& detail,
                       uint32_t flags)>
        on_event;
    std::function<void()> on_gone;
};

struct PageFacts {
    bool crashed = false;
    std::string created_by; /* the session's name, empty when the application created it */
};

/* The endpoint's pending hello (a bdk_context_t of type T_HELLO). */
struct Hello {
    std::weak_ptr<Connection> connection;
};

struct AutomationState {
    bool enabled = false;                       /* [automation] enabled */
    bool external_enabled = false;              /* [automation.external] enabled */
    uint32_t cpu_budget_ms = 60000;             /* [automation] cpu_budget_ms */
    bool protocol = true;                       /* [devtools] protocol */
    bool frontend_disabled = false;             /* [devtools] frontend = "disabled" */
    std::map<std::string, std::string> scripts; /* name -> resolved entry, as JSON */
    std::vector<std::shared_ptr<Session>> sessions;
    std::map<bdk_contents_t, PageFacts> pages;
    std::map<std::string, std::vector<UserScript>> user_scripts; /* by profile id */
    std::vector<std::shared_ptr<ContentsWaiter>> waiters;
    /* The endpoint. */
    std::unique_ptr<LocalServer> server;
    size_t endpoint_soft_limit = 0, endpoint_hard_limit = 0; /* test hook; 0: the default */
    std::map<uint64_t, std::shared_ptr<Connection>> external;
    bdk_automation_hello_cb hello_cb = nullptr;
    void* hello_ctx = nullptr;
    bdk_release_cb hello_rel = nullptr;
    uint64_t next_task = 1;
    uint64_t next_session = 1;
};

/* The state of a running runtime; never null for one that started. */
AutomationState& automation_state(const Runtime& rt);

/* Whether a session (or the host) may address the page. */
bool caller_sees_contents(const Caller& caller, const Contents& contents);
/* The session whose caller this is, or null (the host, a page, a stub caller). */
std::shared_ptr<Session> session_of(const Runtime& rt, const Caller& caller);
/* A page the caller created through contents.create: in its scope, and announced. */
void automation_page_created(const std::shared_ptr<Runtime>& rt,
                             const std::shared_ptr<Contents>& contents, const Caller& caller);
/* The JSON of ContentsInfo (interface/contents.json). */
std::string contents_info_json(const Runtime& rt, const Contents& contents);
/* Waits for page events; the waiter ends when on_event says so, or the page goes. */
void add_contents_waiter(const Runtime& rt, std::shared_ptr<ContentsWaiter> waiter);
/* The configuration name of a UI surface (ui_router.cc), "unknown" for none. */
const char* ui_surface_name(uint32_t surface);
/* Tells the backend a profile's user scripts changed. */
void user_scripts_changed(Runtime& rt, Profile& profile);

} // namespace bdkcore

#endif /* BDK_AUTOMATION_STATE_H_ */
