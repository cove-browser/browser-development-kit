// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/pages/page_routes.h"

#include <memory>
#include <optional>

#include "base/command_line.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "bdk/core/runtime/routes.h"
#include "bdk/engine/json_util.h"
#include "content/public/browser/browser_url_handler.h"
#include "content/public/browser/navigation_handle.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {
namespace {

base::FilePath& BundlePath() {
  static base::NoDestructor<base::FilePath> path;
  return *path;
}

// Forward: a branded URL whose route is served by a page becomes the internal
// URL. A disabled, native or unknown route is left as it is; the navigation
// throttle (pages_navigation_throttle.h) blocks it or hands it to Swift.
bool RewriteBrandedUrl(GURL* url, content::BrowserContext*) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt) {
    return false;
  }
  bdkcore::RouteNavigation nav =
      bdkcore::resolve_navigation(*rt, url->spec(), /*trusted=*/true);
  if (nav.kind != bdkcore::RouteNavigation::Kind::kLoad ||
      nav.url == url->spec()) {
    return false;
  }
  *url = GURL(nav.url);
  return true;
}

// Reverse: the internal URL of a route is shown as its branded form (the
// navigation entry's virtual URL).
bool ReverseInternalUrl(GURL* url, content::BrowserContext*) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt) {
    return false;
  }
  std::string shown = bdkcore::display_url(*rt, url->spec());
  if (shown == url->spec()) {
    return false;
  }
  *url = GURL(shown);
  return true;
}

}  // namespace

std::string InternalSchemeOf(const std::string& resolved_config) {
  std::optional<base::DictValue> root = ParseJsonDict(resolved_config);
  if (!root) {
    return std::string();
  }
  const std::string* scheme =
      root->FindStringByDottedPath("browser.internal_scheme");
  return scheme ? *scheme : std::string();
}

void CopyInternalSchemeSwitch(base::CommandLine* child) {
  const base::CommandLine& browser = *base::CommandLine::ForCurrentProcess();
  if (browser.HasSwitch(kInternalSchemeSwitch) &&
      !child->HasSwitch(kInternalSchemeSwitch)) {
    child->AppendSwitchASCII(kInternalSchemeSwitch, browser.GetSwitchValueASCII(
                                                        kInternalSchemeSwitch));
  }
}

void SetPagesBundlePath(const base::FilePath& bundle) {
  BundlePath() = bundle;
}

const base::FilePath& PagesBundlePath() {
  return BundlePath();
}

std::string DisplayUrl(const std::string& url) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  return rt ? bdkcore::display_url(*rt, url) : url;
}

std::string DisplayUrl(const GURL& url) {
  return DisplayUrl(url.possibly_invalid_spec());
}

void AddPageUrlHandlers(content::BrowserURLHandler* handler) {
  handler->AddHandlerPair(&RewriteBrandedUrl, &ReverseInternalUrl);
}

bool IsTrustedInitiator(content::NavigationHandle& handle) {
  if (!handle.IsRendererInitiated()) {
    return true;
  }
  const std::optional<url::Origin>& initiator = handle.GetInitiatorOrigin();
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  return rt && initiator && !initiator->opaque() &&
         bdkcore::is_internal_page(*rt, initiator->GetURL().spec());
}

}  // namespace bdk
