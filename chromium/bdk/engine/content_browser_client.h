// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_CONTENT_BROWSER_CLIENT_H_
#define BDK_ENGINE_CONTENT_BROWSER_CLIENT_H_

#include <memory>
#include <string>
#include <vector>

#include "chrome/browser/chrome_content_browser_client.h"

namespace bdk {

// Chrome's browser client, plus what the BDK engine adds: the navigation
// interceptor the engine's test hooks script (testing.cc) and HTTP
// authentication as a UI request (ui_auth.h), and the geolocation provider
// the application chose. BDK's internal pages and broker
// hook in here later (M4).
class BdkContentBrowserClient : public ChromeContentBrowserClient {
 public:
  BdkContentBrowserClient();
  ~BdkContentBrowserClient() override;

  std::vector<std::unique_ptr<content::URLLoaderRequestInterceptor>>
  WillCreateURLLoaderRequestInterceptors(
      content::NavigationUIData* navigation_ui_data,
      content::FrameTreeNodeId frame_tree_node_id,
      int64_t navigation_id,
      bool force_no_https_upgrade,
      scoped_refptr<base::SequencedTaskRunner> navigation_response_task_runner)
      override;

  std::unique_ptr<content::LoginDelegate> CreateLoginDelegate(
      const net::AuthChallengeInfo& auth_info,
      content::WebContents* web_contents,
      content::BrowserContext* browser_context,
      const content::GlobalRequestID& request_id,
      bool is_request_for_primary_main_frame_navigation,
      bool is_request_for_navigation,
      const GURL& url,
      scoped_refptr<net::HttpResponseHeaders> response_headers,
      bool first_auth_attempt,
      bool do_not_prompt_for_login,
      content::GuestPageHolder* guest_page_holder,
      content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback)
      override;

  // Providers (T-PRV-04, T-PRV-09): the application's location provider for
  // geolocation = "app", and the Google key only for geolocation = "google".
  std::unique_ptr<device::LocationProvider> OverrideSystemLocationProvider()
      override;
  std::string GetGeolocationApiKey() override;
};

}  // namespace bdk

#endif  // BDK_ENGINE_CONTENT_BROWSER_CLIENT_H_
