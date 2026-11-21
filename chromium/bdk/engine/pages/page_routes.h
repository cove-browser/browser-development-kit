// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_PAGES_PAGE_ROUTES_H_
#define BDK_ENGINE_PAGES_PAGE_ROUTES_H_

#include <string>

#include "base/files/file_path.h"

class GURL;

namespace base {
class CommandLine;
}

namespace content {
class BrowserURLHandler;
class NavigationHandle;
}  // namespace content

namespace bdk {

// The engine's side of the route table (packages/core/runtime/routes.h): the
// core decides; these apply its decisions where Chromium asks.

// The switch the scheme patch reads in every process
// (chrome/common/chrome_content_client.cc).
inline constexpr char kInternalSchemeSwitch[] = "bdk-internal-scheme";

// The branded scheme of the resolved configuration, or empty.
std::string InternalSchemeOf(const std::string& resolved_config);

// Copies the scheme switch from the browser's command line to a child's.
void CopyInternalSchemeSwitch(base::CommandLine* child);

// The application bundle the pages are read from (bdk_start_config_t's
// bundle_path), set at start.
void SetPagesBundlePath(const base::FilePath& bundle);
const base::FilePath& PagesBundlePath();

// The URL the application sees for one the engine reports: the branded form of
// an internal page, anything else as it is (bdkcore::display_url).
std::string DisplayUrl(const std::string& url);
std::string DisplayUrl(const GURL& url);

// Adds the handler pair that rewrites a branded URL to the internal URL its
// route is served at, and back for display.
void AddPageUrlHandlers(content::BrowserURLHandler* handler);

// Whether a navigation may open a branded URL: one the application started,
// or one an internal page started.
bool IsTrustedInitiator(content::NavigationHandle& handle);

}  // namespace bdk

#endif  // BDK_ENGINE_PAGES_PAGE_ROUTES_H_
