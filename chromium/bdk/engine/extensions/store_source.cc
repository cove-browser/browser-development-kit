// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/store_source.h"

#include <memory>

#include "base/functional/callback_helpers.h"
#include "base/strings/utf_string_conversions.h"
#include "bdk/core/runtime/broker_internal.h"
#include "bdk/core/runtime/extension_policy.h"
#include "bdk/engine/extensions/extension_support.h"
#include "bdk/engine/extensions/management_policy.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "content/public/common/isolated_world_ids.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {

namespace {

// The first embedder world: Chrome's own (translate, extensions) start after
// it.
constexpr int32_t kRelabelWorld = content::ISOLATED_WORLD_ID_CONTENT_END;

class StoreRelabeler : public content::WebContentsObserver,
                       public content::WebContentsUserData<StoreRelabeler> {
 public:
  ~StoreRelabeler() override = default;

  // content::WebContentsObserver:
  void DOMContentLoaded(content::RenderFrameHost* frame) override {
    if (!frame->IsInPrimaryMainFrame() ||
        !url::IsSameOriginWith(frame->GetLastCommittedURL(),
                               GURL(kStoreOrigin))) {
      return;
    }
    std::shared_ptr<bdkcore::Runtime> runtime = CurrentRuntime();
    if (!runtime || !StoreEnabled(runtime->extensions, "chrome-web-store")) {
      return;
    }
    frame->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(StoreRelabelScript(
            bdkcore::ext::store_install_label(runtime->browser_name))),
        base::NullCallback(), kRelabelWorld);
  }

 private:
  friend class content::WebContentsUserData<StoreRelabeler>;
  explicit StoreRelabeler(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<StoreRelabeler>(*contents) {}

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(StoreRelabeler);

}  // namespace

std::string StoreRelabelScript(const std::string& label) {
  // The label goes in as a JSON string, so a browser name with quotes or
  // backslashes cannot end the literal.
  return "(function(){var label=\"" + bdkcore::json_escape(label) +
         "\";var from=/^\\s*Add to Chrome\\s*$/;"
         "function walk(root){var w=document.createTreeWalker(root,"
         "NodeFilter.SHOW_TEXT);var n;while((n=w.nextNode())){"
         "if(from.test(n.nodeValue)&&n.parentElement&&"
         "n.parentElement.closest('button,[role=button],a')){"
         "n.nodeValue=label;}}}"
         "function run(){try{walk(document.body||document.documentElement);}"
         "catch(e){}}"
         "run();"
         "try{new MutationObserver(run).observe(document.documentElement,"
         "{childList:true,subtree:true,characterData:true});}catch(e){}"
         "})();";
}

void InstallStoreRelabeler(content::WebContents* contents) {
  StoreRelabeler::CreateForWebContents(contents);
}

}  // namespace bdk
