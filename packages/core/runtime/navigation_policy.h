/* Navigation policy (T-SWF-10, swift-api.md 6.3): the application's handler, the decisions it has
   not answered yet, and the rules every backend follows (bdk_contents.h). Platform neutral C++23,
   like the rest of the core; the stub calls these from its navigation steps and the engine from a
   navigation throttle (T-SWF-11), so what is asked, the loop limit and the stale handles are the
   same on both. */
#ifndef BDK_NAVIGATION_POLICY_H_
#define BDK_NAVIGATION_POLICY_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core_internal.h"

namespace bdkcore {

/* Redirect decisions in a row without a commit, after which the next is refused. Chromium's own
   limit for server redirects (net/url_request/redirect_util.h, kMaxRedirects). */
constexpr uint32_t kMaxPolicyRedirects = 20;
constexpr int32_t kNetErrorTooManyRedirects = -310;

/* A navigation the policy is asked about. URLs are as the application sees them (display_url). */
struct PolicyNavigation {
    std::string url;
    const char* type = "load"; /* "load", "reload" or "history" */
    std::string method = "GET";
    const char* initiator = "application"; /* "application", "page" or "browser" */
    std::string initiator_origin;          /* a page's origin, when known */
    std::string redirected_from;           /* a server redirect: the URL it came from */
    bool user_initiated = false;
    bool prerender = false;
    /* Redirect decisions in a row that led to this navigation: 0 for one no decision began. The
       backend carries it from the decision to the load it starts and to that load's server
       redirects. */
    uint32_t policy_redirects = 0;
};

enum class PolicyAction : uint8_t {
    kAllow,
    kCancel,           /* end it with no request sent: fail it with policy_failure_detail */
    kRedirect,         /* end it the same way, then load the URL with policy_redirects + 1 */
    kTooManyRedirects, /* a redirect over kMaxPolicyRedirects: the error page, -310 */
};

/* One decision the handler has not answered. */
struct PolicyDecision {
    bdk_context_t handle = 0;
    bdk_contents_t contents = 0;
    uint32_t policy_redirects = 0;
    std::function<void(PolicyAction, const std::string& redirect_url)> done;
};

struct PolicyTable {
    bdk_navigation_policy_cb cb = nullptr;
    void* ctx = nullptr;
    bdk_release_cb rel = nullptr;
    std::vector<std::weak_ptr<PolicyDecision>> pending;
};

/* Asks the application's handler about a navigation, later. Returns null when nothing is asked (no
   handler, or the start of a navigation a redirect decision began): the backend goes on at once,
   exactly as with no policy. Otherwise the backend waits; `done` runs once, on a later task, with
   the answer (kAllow if the handler was removed before it was called), unless the navigation is
   withdrawn first. */
std::shared_ptr<PolicyDecision>
ask_navigation_policy(const std::shared_ptr<Runtime>&, const std::shared_ptr<Contents>&,
                      const PolicyNavigation&,
                      std::function<void(PolicyAction, const std::string& redirect_url)> done);

/* The navigation ended before the answer (stopped, superseded, the page closed): the decision
   handle goes stale and `done` is never called. Does nothing for null or a finished decision. */
void withdraw_navigation_policy(const std::shared_ptr<PolicyDecision>&);

/* destroy_contents: withdraws every decision asked for the page. */
void navigation_policy_forget_contents(Runtime&, bdk_contents_t);

/* The detail of BDK_CONTENTS_EVENT_NAVIGATION_FAILED for a cancel or a redirect. */
std::string policy_failure_detail(PolicyAction, const std::string& redirect_url);

/* At shutdown: releases the handler and makes every pending decision stale. */
void navigation_policy_shutdown(Runtime&);

} // namespace bdkcore

#endif /* BDK_NAVIGATION_POLICY_H_ */
