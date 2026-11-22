// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/content_browser_client.h"

#include <utility>

#include "base/command_line.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/pages/bundled_page_ui.h"
#include "bdk/engine/pages/page_broker.mojom.h"
#include "bdk/engine/pages/page_routes.h"
#include "bdk/engine/pages/pages_navigation_throttle.h"
#include "bdk/engine/providers/geolocation_provider.h"
#include "bdk/engine/providers/provider_settings.h"
#include "bdk/engine/services/user_scripts.h"
#include "bdk/engine/startup_completion.h"
#include "bdk/engine/testing.h"
#include "bdk/engine/ui_auth.h"
#include "chrome/browser/chrome_browser_main.h"
#include "chrome/browser/chrome_browser_main_extra_parts.h"
#include "content/public/browser/browser_url_handler.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/url_loader_request_interceptor.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "extensions/common/constants.h"
#include "services/device/public/cpp/geolocation/location_provider.h"

namespace bdk {

namespace {

// The DevTools front end loads an extension's `devtools_page` in a frame of
// its own and the extension adds its panels from there, so a front end whose
// frames cannot load extension pages shows no extension panel. Chromium adds
// the extensions to the front end itself and offers no switch to stop it.
class DevToolsExtensionThrottle : public content::NavigationThrottle {
 public:
  explicit DevToolsExtensionThrottle(
      content::NavigationThrottleRegistry& registry)
      : content::NavigationThrottle(registry) {}

  ThrottleCheckResult WillStartRequest() override {
    content::NavigationHandle* handle = navigation_handle();
    if (handle->IsInMainFrame() ||
        !handle->GetURL().SchemeIs(extensions::kExtensionScheme)) {
      return PROCEED;
    }
    content::RenderFrameHost* parent = handle->GetParentFrame();
    if (!parent || !parent->GetLastCommittedURL().SchemeIs("devtools")) {
      return PROCEED;
    }
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    if (rt &&
        !bdkcore::ui_surface_available(*rt, BDK_SURFACE_EXTENSION_DEVTOOLS)) {
      return BLOCK_REQUEST_AND_COLLAPSE;
    }
    return PROCEED;
  }

  const char* GetNameForLogging() override {
    return "BdkDevToolsExtensionThrottle";
  }
};

// What BDK does at the stages of Chrome's browser start.
class BdkBrowserMainExtraParts : public ChromeBrowserMainExtraParts {
 public:
  // After the threads exist and before Chrome's own platform setup, which
  // installs the operating system's location permission unless one is.
  void PostCreateThreads() override {
    InstallGeolocationPermission();
    HoldStartupUntilRuntimeStarts();
  }
};

}  // namespace

BdkContentBrowserClient::BdkContentBrowserClient() = default;
BdkContentBrowserClient::~BdkContentBrowserClient() = default;

std::unique_ptr<content::BrowserMainParts>
BdkContentBrowserClient::CreateBrowserMainParts(bool is_integration_test) {
  std::unique_ptr<content::BrowserMainParts> parts =
      ChromeContentBrowserClient::CreateBrowserMainParts(is_integration_test);
  // Chrome's client makes ChromeBrowserMainParts (its platform subclass).
  static_cast<ChromeBrowserMainParts*>(parts.get())
      ->AddParts(std::make_unique<BdkBrowserMainExtraParts>());
  return parts;
}

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
  registry.AddThrottle(std::make_unique<DevToolsExtensionThrottle>(registry));
  MaybeAddUserScriptsThrottle(registry);
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

std::unique_ptr<device::LocationProvider>
BdkContentBrowserClient::OverrideSystemLocationProvider() {
  return CreateApplicationLocationProvider();
}

std::string BdkContentBrowserClient::GetGeolocationApiKey() {
  // The network provider gets a key only when the application chose Google:
  // the key may be present for another Google provider (architecture.md 16,
  // invariant 9).
  if (CurrentProviderSettings().geolocation != "google") {
    return std::string();
  }
  return ChromeContentBrowserClient::GetGeolocationApiKey();
}

}  // namespace bdk
