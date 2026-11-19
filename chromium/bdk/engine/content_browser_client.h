// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_CONTENT_BROWSER_CLIENT_H_
#define BDK_ENGINE_CONTENT_BROWSER_CLIENT_H_

#include <memory>
#include <vector>

#include "chrome/browser/chrome_content_browser_client.h"

namespace bdk {

// Chrome's browser client, plus what the BDK engine adds. For now: the
// navigation interceptor the engine's test hooks script (testing.cc). BDK's
// internal pages and broker hook in here later (M4).
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
};

}  // namespace bdk

#endif  // BDK_ENGINE_CONTENT_BROWSER_CLIENT_H_
