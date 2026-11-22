// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_PAGES_PAGES_NAVIGATION_THROTTLE_H_
#define BDK_ENGINE_PAGES_PAGES_NAVIGATION_THROTTLE_H_

#include "base/memory/weak_ptr.h"
#include "content/public/browser/navigation_throttle.h"

namespace content {
class NavigationThrottleRegistry;
}

namespace bdk {

// Applies the route table (bdkcore::resolve_navigation) to every navigation of
// a BDK page (T-WUI-02, T-WUI-05):
// - a branded URL whose route is disabled or unknown commits the standard
//   error page; one a web page, an extension or a redirect asks for is
//   dropped, as Chromium drops a web page's chrome:// link;
// - a branded URL of a native route is cancelled and delivered to the
//   application's handler, which may show the error page instead;
// - a branded URL an internal page links to is started again by the browser,
//   so the URL handler rewrites it to its internal URL;
// - an internal URL loads only if its route is served there now, so an
//   overridden or disabled built-in page has no other way in.
// Pages Chromium creates by itself (DevTools, print preview, extension
// background pages) are not BDK pages and are left alone.
class PagesNavigationThrottle : public content::NavigationThrottle {
 public:
  static void MaybeCreateAndAdd(content::NavigationThrottleRegistry& registry);

  explicit PagesNavigationThrottle(
      content::NavigationThrottleRegistry& registry);
  PagesNavigationThrottle(const PagesNavigationThrottle&) = delete;
  PagesNavigationThrottle& operator=(const PagesNavigationThrottle&) = delete;
  ~PagesNavigationThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  const char* GetNameForLogging() override;

 private:
  ThrottleCheckResult Check(bool redirect);
  void OnNativeHandled(bool handled);

  base::WeakPtrFactory<PagesNavigationThrottle> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_PAGES_PAGES_NAVIGATION_THROTTLE_H_
