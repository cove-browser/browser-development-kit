// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/navigation_policy_throttle.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/core/runtime/navigation_policy.h"
#include "bdk/core/runtime/routes.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/pages/page_routes.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "net/base/net_errors.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {
namespace {

using Kind = bdkcore::RouteNavigation::Kind;
using bdkcore::PolicyAction;

// Loads the application's URL of a redirect decision, outside the throttle's
// own call, as bdk_contents_load would.
void LoadRedirectTarget(base::WeakPtr<content::WebContents> contents,
                        std::string url,
                        uint32_t policy_redirects) {
  if (!contents) {
    return;
  }
  if (EngineContents* page = EngineContents::FromWebContents(contents.get())) {
    page->LoadForPolicy(url, policy_redirects);
  }
}

const char* TypeOf(content::NavigationHandle& handle) {
  if (handle.GetReloadType() != content::ReloadType::NONE) {
    return "reload";
  }
  if (handle.GetPageTransition() & ui::PAGE_TRANSITION_FORWARD_BACK) {
    return "history";
  }
  return "load";
}

// The page's own navigations, those the application asked for through the
// page's API (loads, reloads, history), and the rest Chromium starts itself
// (an extension's, a branded link an internal page restarts).
const char* InitiatorOf(content::NavigationHandle& handle) {
  if (handle.IsRendererInitiated()) {
    return "page";
  }
  const ui::PageTransition t = handle.GetPageTransition();
  if (ui::PageTransitionCoreTypeIs(t, ui::PAGE_TRANSITION_TYPED) ||
      handle.GetReloadType() != content::ReloadType::NONE ||
      (t & ui::PAGE_TRANSITION_FORWARD_BACK)) {
    return "application";
  }
  return "browser";
}

}  // namespace

// static
void NavigationPolicyThrottle::MaybeCreateAndAdd(
    content::NavigationThrottleRegistry& registry) {
  content::NavigationHandle& handle = registry.GetNavigationHandle();
  if (!handle.IsInOutermostMainFrame() || !bdkcore::current_runtime() ||
      !EngineContents::FromWebContents(handle.GetWebContents())) {
    return;
  }
  registry.AddThrottle(std::make_unique<NavigationPolicyThrottle>(registry));
}

NavigationPolicyThrottle::NavigationPolicyThrottle(
    content::NavigationThrottleRegistry& registry)
    : content::NavigationThrottle(registry) {}

NavigationPolicyThrottle::~NavigationPolicyThrottle() {
  // Stopped, superseded or the page closed before the answer.
  bdkcore::withdraw_navigation_policy(decision_);
}

content::NavigationThrottle::ThrottleCheckResult
NavigationPolicyThrottle::WillStartRequest() {
  content::NavigationHandle& handle = *navigation_handle();
  if (!handle.IsRendererInitiated()) {
    if (EngineContents* page =
            EngineContents::FromWebContents(handle.GetWebContents())) {
      policy_redirects_ = page->TakePolicyRedirects();
    }
  }
  return Ask(/*redirect=*/false);
}

content::NavigationThrottle::ThrottleCheckResult
NavigationPolicyThrottle::WillRedirectRequest() {
  return Ask(/*redirect=*/true);
}

const char* NavigationPolicyThrottle::GetNameForLogging() {
  return "BdkNavigationPolicyThrottle";
}

content::NavigationThrottle::ThrottleCheckResult NavigationPolicyThrottle::Ask(
    bool redirect) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt || !bdkcore::navigation_policy_active(*rt)) {
    return PROCEED;
  }
  content::NavigationHandle& handle = *navigation_handle();
  EngineContents* page =
      EngineContents::FromWebContents(handle.GetWebContents());
  std::shared_ptr<bdkcore::Contents> core = page ? page->Core() : nullptr;
  if (!core) {
    return PROCEED;
  }
  const GURL& url = handle.GetURL();
  // What the route table drops, or starts again as the browser's own
  // navigation, never reaches the page (PagesNavigationThrottle): the
  // navigation that does is the one asked.
  if (!redirect) {
    const bdkcore::RouteNavigation route = bdkcore::resolve_navigation(
        *rt, url.spec(), IsTrustedInitiator(handle));
    if (route.kind == Kind::kRefused ||
        (route.kind == Kind::kLoad && route.url != url.spec())) {
      return PROCEED;
    }
  }
  bdkcore::PolicyNavigation nav;
  nav.url = bdkcore::display_url(*rt, url.spec());
  nav.type = TypeOf(handle);
  nav.method = handle.IsPost() ? "POST" : "GET";
  nav.initiator = InitiatorOf(handle);
  if (const std::optional<url::Origin>& origin = handle.GetInitiatorOrigin();
      handle.IsRendererInitiated() && origin && !origin->opaque()) {
    nav.initiator_origin = origin->Serialize();
  }
  if (redirect) {
    const std::vector<GURL>& chain = handle.GetRedirectChain();
    if (chain.size() >= 2) {
      nav.redirected_from =
          bdkcore::display_url(*rt, chain[chain.size() - 2].spec());
    }
  }
  nav.user_initiated = !handle.IsRendererInitiated() || handle.HasUserGesture();
  nav.prerender = handle.IsInPrerenderedMainFrame();
  nav.policy_redirects = policy_redirects_;
  base::WeakPtr<NavigationPolicyThrottle> weak = weak_factory_.GetWeakPtr();
  decision_ = bdkcore::ask_navigation_policy(
      rt, core, nav,
      [weak](PolicyAction action, const std::string& redirect_url) {
        if (weak) {
          weak->OnAnswer(action, redirect_url);
        }
      });
  return decision_ ? DEFER : PROCEED;
}

void NavigationPolicyThrottle::OnAnswer(PolicyAction action,
                                        const std::string& redirect_url) {
  decision_.reset();
  content::NavigationHandle& handle = *navigation_handle();
  if (action == PolicyAction::kAllow) {
    Resume();
    return;
  }
  // A prerender is the page's guess at a later navigation: one the
  // application would not let start as it is, is not kept.
  if (handle.IsInPrerenderedMainFrame()) {
    CancelDeferredNavigation(CANCEL_AND_IGNORE);
    return;
  }
  if (action == PolicyAction::kTooManyRedirects) {
    CancelDeferredNavigation({CANCEL, net::ERR_TOO_MANY_REDIRECTS});
    return;
  }
  EngineContents* page =
      EngineContents::FromWebContents(handle.GetWebContents());
  if (page) {
    page->NotePolicyEnd(handle.GetNavigationId(),
                        bdkcore::policy_failure_detail(action, redirect_url));
  }
  if (action == PolicyAction::kRedirect) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&LoadRedirectTarget,
                                  handle.GetWebContents()->GetWeakPtr(),
                                  redirect_url, policy_redirects_ + 1));
  }
  // The navigation sends no request and the page stays where it is.
  CancelDeferredNavigation(CANCEL_AND_IGNORE);
}

}  // namespace bdk
