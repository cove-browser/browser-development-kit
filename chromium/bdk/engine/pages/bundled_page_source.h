// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_PAGES_BUNDLED_PAGE_SOURCE_H_
#define BDK_ENGINE_PAGES_BUNDLED_PAGE_SOURCE_H_

#include <string>
#include <string_view>

#include "content/public/browser/url_data_source.h"

namespace bdk {

// Files above this size are refused, as `bdk build pages` refuses them.
inline constexpr int64_t kMaxPageFileBytes = int64_t{16} * 1024 * 1024;

// Serves one bundled host from the signed application bundle (architecture.md
// 8.4, T-WUI-03), under BDK's content security policy, which a page cannot
// relax: scripts and styles only from the page and chrome://resources, no
// inline script or eval, no remote frames, Trusted Types with no policy.
//
// - `/<path>` is the file under Contents/Resources/<route path>; `/` is
//   index.html, and a path with no file extension and no file serves
//   index.html, so client-side routing works. A path that leaves the page's
//   directory, through `..` or a link, is refused.
// - `/@bdk/internal/<module>.js` is the SDK, from
//   Contents/Resources/BDK/js/internal (written by `bdk build pages`).
// - `/@bdk/page_broker.mojom-webui.js` is the page broker's Mojo bindings,
//   compiled into the runtime so they always match it.
// - BDK's own new-tab page (route new-tab in chromium mode) is a page in the
//   runtime, with no script.
//
// The route table decides what the host serves at the time of each request
// (bdkcore::bundled_source), so a host whose route is not bundled now serves
// nothing.
class BundledPageSource : public content::URLDataSource {
 public:
  explicit BundledPageSource(std::string_view host);
  BundledPageSource(const BundledPageSource&) = delete;
  BundledPageSource& operator=(const BundledPageSource&) = delete;
  ~BundledPageSource() override;

  // content::URLDataSource:
  std::string GetSource() override;
  void StartDataRequest(const GURL& url,
                        const content::WebContents::Getter& wc_getter,
                        GotDataCallback callback) override;
  std::string GetMimeType(const GURL& url) override;
  bool AllowCaching() override;
  std::string GetContentSecurityPolicy(
      network::mojom::CSPDirectiveName directive) override;
  bool ShouldServeMimeTypeAsContentTypeHeader() override;

 private:
  const std::string host_;
};

}  // namespace bdk

#endif  // BDK_ENGINE_PAGES_BUNDLED_PAGE_SOURCE_H_
