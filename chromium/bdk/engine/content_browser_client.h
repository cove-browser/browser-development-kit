// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_CONTENT_BROWSER_CLIENT_H_
#define BDK_ENGINE_CONTENT_BROWSER_CLIENT_H_

#include <memory>
#include <vector>

#include "chrome/browser/chrome_content_browser_client.h"

namespace bdk {

// Chrome's browser client, plus what the BDK engine adds: the navigation
// interceptor the engine's test hooks script (testing.cc), HTTP
// authentication as a UI request (ui_auth.h), and internal pages
// (engine/pages): the branded scheme in every child process, the URL handler
// pair, the navigation throttle and the page broker's interface broker.
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

  void AppendExtraCommandLineSwitches(base::CommandLine* command_line,
                                      int child_process_id) override;
  void BrowserURLHandlerCreated(content::BrowserURLHandler* handler) override;
  void CreateThrottlesForNavigation(
      content::NavigationThrottleRegistry& registry) override;
  void RegisterTrustedWebUIInterfaceBrokers(
      content::WebUIBrowserInterfaceBrokerRegistry& registry) override;

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
};

}  // namespace bdk

#endif  // BDK_ENGINE_CONTENT_BROWSER_CLIENT_H_
