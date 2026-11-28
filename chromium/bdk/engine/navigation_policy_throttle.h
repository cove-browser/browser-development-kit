// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_NAVIGATION_POLICY_THROTTLE_H_
#define BDK_ENGINE_NAVIGATION_POLICY_THROTTLE_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "content/public/browser/navigation_throttle.h"

namespace bdkcore {
struct PolicyDecision;
enum class PolicyAction : uint8_t;
}  // namespace bdkcore

namespace content {
class NavigationThrottleRegistry;
}

namespace bdk {

// The application's navigation policy (T-SWF-10, T-SWF-11; swift-api.md 6.3;
// conventions.md 16) on Chromium. For the outermost main frame of a BDK page
// (the primary page or a prerender), each navigation is deferred where it
// would send its request and again at each server redirect, until the
// application answers (bdkcore::ask_navigation_policy). Allow resumes it;
// cancel ends it with no request sent; redirect ends it the same way and
// loads the application's URL, whose start is not asked again; a prerender
// the application does not allow as it is is discarded. With no handler the
// throttle proceeds at once, so nothing changes.
//
// Chromium runs no throttle for a page activation (a back/forward cache
// restore, a prerender activation): a prerender was asked when it started,
// and EngineContents keeps a page out of the back/forward cache while a
// handler is set, so a history navigation is a navigation that is asked.
class NavigationPolicyThrottle : public content::NavigationThrottle {
 public:
  static void MaybeCreateAndAdd(content::NavigationThrottleRegistry& registry);

  explicit NavigationPolicyThrottle(
      content::NavigationThrottleRegistry& registry);
  NavigationPolicyThrottle(const NavigationPolicyThrottle&) = delete;
  NavigationPolicyThrottle& operator=(const NavigationPolicyThrottle&) = delete;
  ~NavigationPolicyThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  const char* GetNameForLogging() override;

 private:
  ThrottleCheckResult Ask(bool redirect);
  void OnAnswer(bdkcore::PolicyAction action, const std::string& redirect_url);

  // Redirect decisions in a row that led to this navigation (conventions.md
  // 16): from the page when a redirect decision started it, for its server
  // redirects.
  uint32_t policy_redirects_ = 0;
  std::shared_ptr<bdkcore::PolicyDecision> decision_;
  base::WeakPtrFactory<NavigationPolicyThrottle> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_NAVIGATION_POLICY_THROTTLE_H_
