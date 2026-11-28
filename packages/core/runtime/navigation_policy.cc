/* Navigation policy. See navigation_policy.h and bdk_contents.h. */
#include "navigation_policy.h"

#include <algorithm>
#include <utility>

using namespace bdkcore;

namespace bdkcore {
namespace {

PolicyTable& table_of(Runtime& rt) {
    if (!rt.policy)
        rt.policy = std::make_shared<PolicyTable>();
    return *rt.policy;
}

std::string navigation_json(const PolicyNavigation& nav) {
    std::string json = "{\"url\":\"" + json_escape(nav.url) + "\",\"type\":\"" + nav.type +
                       "\",\"method\":\"" + json_escape(nav.method) +
                       "\",\"isUserInitiated\":" + (nav.user_initiated ? "true" : "false") +
                       ",\"isRedirect\":" + (nav.redirected_from.empty() ? "false" : "true");
    if (!nav.redirected_from.empty())
        json += ",\"redirectedFrom\":\"" + json_escape(nav.redirected_from) + "\"";
    json += std::string(",\"initiator\":{\"kind\":\"") + nav.initiator + "\"";
    if (!nav.initiator_origin.empty())
        json += ",\"origin\":\"" + json_escape(nav.initiator_origin) + "\"";
    json += std::string("},\"isPrerender\":") + (nav.prerender ? "true" : "false") + "}";
    return json;
}

/* Ends a decision: its handle goes stale and it leaves the pending list. Returns its done, which
   the caller runs or drops. */
std::function<void(PolicyAction, const std::string&)> take(Runtime* rt, PolicyDecision& d) {
    if (d.handle)
        table_free(d.handle);
    d.handle = 0;
    auto done = std::move(d.done);
    d.done = nullptr;
    if (rt && rt->policy) {
        auto& pending = rt->policy->pending;
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&d](const std::weak_ptr<PolicyDecision>& w) {
                                         auto p = w.lock();
                                         return !p || p.get() == &d;
                                     }),
                      pending.end());
    }
    return done;
}

} // namespace

bool navigation_policy_active(const Runtime& rt) {
    return rt.policy && rt.policy->cb && !rt.shutting;
}

std::shared_ptr<PolicyDecision>
ask_navigation_policy(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c,
                      const PolicyNavigation& nav,
                      std::function<void(PolicyAction, const std::string&)> done) {
    PolicyTable& t = table_of(*rt);
    if (!t.cb || rt->shutting)
        return nullptr;
    /* The application chose the URL of a redirect decision: its start is not asked again. */
    if (nav.policy_redirects > 0 && nav.redirected_from.empty())
        return nullptr;
    auto d = std::make_shared<PolicyDecision>();
    d->contents = c->handle;
    d->policy_redirects = nav.policy_redirects;
    d->done = std::move(done);
    d->handle = table_make(T_DECISION, d);
    t.pending.push_back(d);
    std::string json = navigation_json(nav);
    bdk_context_t handle = d->handle;
    bdk_contents_t contents = c->handle;
    std::weak_ptr<Runtime> weak = rt;
    post([weak, handle, contents, json] {
        auto r = weak.lock();
        auto decision = get<PolicyDecision>(handle, T_DECISION);
        if (!r || r->shutting || !decision)
            return;
        PolicyTable& now = table_of(*r);
        if (!now.cb) {
            /* Removed since it was asked: the navigation goes on, as with no handler. */
            auto finish = take(r.get(), *decision);
            if (finish)
                post([finish] { finish(PolicyAction::kAllow, std::string()); });
            return;
        }
        now.cb(now.ctx, handle, contents, view(json));
    });
    return d;
}

void withdraw_navigation_policy(const std::shared_ptr<PolicyDecision>& d) {
    if (!d || !d->handle)
        return;
    auto rt = current_runtime();
    take(rt.get(), *d);
}

void navigation_policy_forget_contents(Runtime& rt, bdk_contents_t contents) {
    if (!rt.policy)
        return;
    std::vector<std::shared_ptr<PolicyDecision>> gone;
    for (auto& w : rt.policy->pending) {
        auto d = w.lock();
        if (d && d->contents == contents)
            gone.push_back(d);
    }
    for (auto& d : gone)
        take(&rt, *d);
}

std::string policy_failure_detail(PolicyAction action, const std::string& redirect_url) {
    if (action == PolicyAction::kRedirect)
        return "{\"code\":\"redirectedByPolicy\",\"redirectUrl\":\"" + json_escape(redirect_url) +
               "\"}";
    return "{\"code\":\"cancelledByPolicy\"}";
}

void navigation_policy_shutdown(Runtime& rt) {
    if (!rt.policy)
        return;
    PolicyTable& t = *rt.policy;
    auto pending = std::move(t.pending);
    t.pending.clear();
    for (auto& w : pending) {
        if (auto d = w.lock())
            take(nullptr, *d);
    }
    release_later(t.ctx, t.rel);
    t.cb = nullptr;
    t.ctx = nullptr;
    t.rel = nullptr;
}

} // namespace bdkcore

/* ---- bdk_contents.h: navigation policy ---- */

extern "C" {

BDK_EXPORT bdk_status_t bdk_navigation_policy_set_handler(bdk_runtime_t h,
                                                          bdk_navigation_policy_cb cb, void* ctx,
                                                          bdk_release_cb rel,
                                                          bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt || rt->shutting)
        return BDK_ERR_STALE_HANDLE;
    PolicyTable& t = table_of(*rt);
    release_later(t.ctx, t.rel);
    t.cb = cb;
    t.ctx = cb ? ctx : nullptr;
    t.rel = cb ? rel : nullptr;
    if (!cb)
        release_later(ctx, rel);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_navigation_policy_decide(bdk_context_t h, uint32_t action,
                                                     bdk_string_t redirect_url,
                                                     bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto d = get<PolicyDecision>(h, T_DECISION);
    if (!d || !d->done)
        return BDK_ERR_STALE_HANDLE;
    PolicyAction outcome;
    std::string url;
    switch (action) {
    case BDK_NAVIGATION_ALLOW:
        outcome = PolicyAction::kAllow;
        break;
    case BDK_NAVIGATION_CANCEL:
        outcome = PolicyAction::kCancel;
        break;
    case BDK_NAVIGATION_REDIRECT:
        if (!string_ok(redirect_url) || !plausible_url(str(redirect_url)))
            return BDK_ERR_INVALID_ARGUMENT;
        url = str(redirect_url);
        outcome = d->policy_redirects >= kMaxPolicyRedirects ? PolicyAction::kTooManyRedirects
                                                             : PolicyAction::kRedirect;
        break;
    default:
        return BDK_ERR_INVALID_ARGUMENT;
    }
    auto rt = current_runtime();
    auto finish = take(rt.get(), *d);
    post([finish, outcome, url] { finish(outcome, url); });
    return BDK_OK;
}

} /* extern "C" */
