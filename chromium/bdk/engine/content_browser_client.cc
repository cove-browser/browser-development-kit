// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/content_browser_client.h"

#include <utility>

#include "base/command_line.h"
#include "bdk/engine/pages/bundled_page_ui.h"
#include "bdk/engine/pages/page_broker.mojom.h"
#include "bdk/engine/pages/page_routes.h"
#include "bdk/engine/pages/pages_navigation_throttle.h"
#include "bdk/engine/testing.h"
#include "bdk/engine/ui_auth.h"
#include "content/public/browser/browser_url_handler.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/url_loader_request_interceptor.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"

namespace bdk {

BdkContentBrowserClient::BdkContentBrowserClient() = default;
BdkContentBrowserClient::~BdkContentBrowserClient() = default;

std::vector<std::unique_ptr<content::URLLoaderRequestInterceptor>>
BdkContentBrowserClient::WillCreateURLLoaderRequestInterceptors(
    content::NavigationUIData* navigation_ui_data,
    content::FrameTreeNodeId frame_tree_node_id,
    int64_t navigation_id,
    bool force_no_https_upgrade,
    scoped_refptr<base::SequencedTaskRunner> navigation_response_task_runner) {
  std::vector<std::unique_ptr<content::URLLoaderRequestInterceptor>>
      interceptors =
          ChromeContentBrowserClient::WillCreateURLLoaderRequestInterceptors(
              navigation_ui_data, frame_tree_node_id, navigation_id,
              force_no_https_upgrade, navigation_response_task_runner);
  if (std::unique_ptr<content::URLLoaderRequestInterceptor> scripted =
          CreateScriptedNavigationInterceptor()) {
    // First, so a scripted URL never reaches the network.
    interceptors.insert(interceptors.begin(), std::move(scripted));
  }
  return interceptors;
}

void BdkContentBrowserClient::AppendExtraCommandLineSwitches(
    base::CommandLine* command_line,
    int child_process_id) {
  ChromeContentBrowserClient::AppendExtraCommandLineSwitches(command_line,
                                                             child_process_id);
  // Every process registers the branded scheme the same way (the scheme
  // patch in chrome/common/chrome_content_client.cc reads it).
  CopyInternalSchemeSwitch(command_line);
}

void BdkContentBrowserClient::BrowserURLHandlerCreated(
    content::BrowserURLHandler* handler) {
  ChromeContentBrowserClient::BrowserURLHandlerCreated(handler);
  AddPageUrlHandlers(handler);
}

void BdkContentBrowserClient::CreateThrottlesForNavigation(
    content::NavigationThrottleRegistry& registry) {
  // First, so a navigation the route table refuses runs no other throttle.
  PagesNavigationThrottle::MaybeCreateAndAdd(registry);
  ChromeContentBrowserClient::CreateThrottlesForNavigation(registry);
}

void BdkContentBrowserClient::RegisterTrustedWebUIInterfaceBrokers(
    content::WebUIBrowserInterfaceBrokerRegistry& registry) {
  ChromeContentBrowserClient::RegisterTrustedWebUIInterfaceBrokers(registry);
  // Only BundledPageUI gets the page broker; every other WebUI that asks for
  // it has its renderer terminated by the per-WebUI broker.
  registry.ForWebUI<BundledPageUI>().Add<mojom::PageBroker>();
}

std::unique_ptr<content::LoginDelegate>
BdkContentBrowserClient::CreateLoginDelegate(
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
    content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback) {
  // Chrome's own prompt is a Views dialog on a Browser window; BDK's is a UI
  // request of the `http_auth` surface.
  return CreateBdkLoginDelegate(auth_info, web_contents,
                                do_not_prompt_for_login,
                                std::move(auth_required_callback));
}

}  // namespace bdk
