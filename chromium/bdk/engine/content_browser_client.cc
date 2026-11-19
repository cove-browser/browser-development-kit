// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/content_browser_client.h"

#include <utility>

#include "bdk/engine/testing.h"
#include "content/public/browser/url_loader_request_interceptor.h"

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

}  // namespace bdk
