// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/pages/pages_navigation_throttle.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/core/runtime/routes.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/pages/page_routes.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/web_contents.h"
#include "net/base/net_errors.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace bdk {
namespace {

using Kind = bdkcore::RouteNavigation::Kind;

// Starts the branded URL again as a navigation of the browser's, outside the
// throttle's own call, so BrowserURLHandler rewrites it.
void LoadAsBrowser(base::WeakPtr<content::WebContents> contents,
                   const GURL& url) {
  if (!contents) {
    return;
  }
  content::NavigationController::LoadURLParams params(url);
  params.transition_type = ui::PAGE_TRANSITION_LINK;
  contents->GetController().LoadURLWithParams(params);
}

}  // namespace

// static
void PagesNavigationThrottle::MaybeCreateAndAdd(
    content::NavigationThrottleRegistry& registry) {
  content::NavigationHandle& handle = registry.GetNavigationHandle();
  if (!bdkcore::current_runtime() ||
      !EngineContents::FromWebContents(handle.GetWebContents())) {
    return;
  }
  registry.AddThrottle(std::make_unique<PagesNavigationThrottle>(registry));
}

PagesNavigationThrottle::PagesNavigationThrottle(
    content::NavigationThrottleRegistry& registry)
    : content::NavigationThrottle(registry) {}

PagesNavigationThrottle::~PagesNavigationThrottle() = default;

content::NavigationThrottle::ThrottleCheckResult
PagesNavigationThrottle::WillStartRequest() {
  return Check(/*redirect=*/false);
}

content::NavigationThrottle::ThrottleCheckResult
PagesNavigationThrottle::WillRedirectRequest() {
  return Check(/*redirect=*/true);
}

const char* PagesNavigationThrottle::GetNameForLogging() {
  return "BdkPagesNavigationThrottle";
}

content::NavigationThrottle::ThrottleCheckResult PagesNavigationThrottle::Check(
    bool redirect) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt) {
    return PROCEED;
  }
  content::NavigationHandle& handle = *navigation_handle();
  const GURL& url = handle.GetURL();
  // A redirect is the server's choice, never the application's or an internal
  // page's.
  const bool trusted = !redirect && IsTrustedInitiator(handle);
  bdkcore::RouteNavigation nav =
      bdkcore::resolve_navigation(*rt, url.spec(), trusted);
  const bool main_frame = handle.IsInPrimaryMainFrame();
  switch (nav.kind) {
    case Kind::kPass:
      return PROCEED;
    case Kind::kRefused:
      return CANCEL_AND_IGNORE;
    case Kind::kBlocked:
      if (!main_frame) {
        // Chromium's own pages frame chrome:// documents of their own; the
        // route table governs only what a page shows at its top level.
        return url.SchemeIs("chrome") ? PROCEED : CANCEL_AND_IGNORE;
      }
      return {BLOCK_REQUEST, static_cast<net::Error>(nav.net_error)};
    case Kind::kNative: {
      if (!main_frame) {
        return CANCEL_AND_IGNORE;
      }
      EngineContents* page =
          EngineContents::FromWebContents(handle.GetWebContents());
      base::WeakPtr<PagesNavigationThrottle> weak = weak_factory_.GetWeakPtr();
      bdkcore::dispatch_native_page(rt, nav, page ? page->Core() : nullptr,
                                    [weak](bool handled) {
                                      if (weak) {
                                        weak->OnNativeHandled(handled);
                                      }
                                    });
      return DEFER;
    }
    case Kind::kLoad:
      if (nav.url == url.spec()) {
        return PROCEED;  // the internal URL its route is served at
      }
      if (!main_frame) {
        return CANCEL_AND_IGNORE;
      }
      // A branded link in an internal page: the browser starts it again.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&LoadAsBrowser, handle.GetWebContents()->GetWeakPtr(),
                         GURL(nav.display)));
      return CANCEL_AND_IGNORE;
  }
  return PROCEED;
}

void PagesNavigationThrottle::OnNativeHandled(bool handled) {
  if (handled) {
    CancelDeferredNavigation(CANCEL);
    return;
  }
  CancelDeferredNavigation({BLOCK_REQUEST, net::ERR_BLOCKED_BY_CLIENT});
}

}  // namespace bdk
