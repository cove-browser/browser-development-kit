// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/pages/bundled_page_ui.h"

#include <set>
#include <utility>

#include "base/logging.h"
#include "base/no_destructor.h"
#include "bdk/core/runtime/routes.h"
#include "bdk/engine/pages/bundled_page_source.h"
#include "bdk/engine/pages/page_broker.mojom.h"
#include "bdk/engine/pages/page_broker_impl.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/webui_config_map.h"
#include "content/public/common/url_constants.h"
#include "url/gurl.h"

namespace bdk {

BundledPageUI::BundledPageUI(content::WebUI* web_ui, const GURL& url)
    : ui::MojoWebUIController(web_ui) {
  content::URLDataSource::Add(web_ui->GetWebContents()->GetBrowserContext(),
                              std::make_unique<BundledPageSource>(url.host()));
}

BundledPageUI::~BundledPageUI() = default;

void BundledPageUI::BindInterface(
    mojo::PendingReceiver<mojom::PageBroker> receiver) {
  content::RenderFrameHost* frame = web_ui()->GetRenderFrameHost();
  std::string why;
  std::shared_ptr<bdkcore::Caller> caller = BindPageCaller(frame, &why);
  if (!caller) {
    // As for any interface a page may not have: the renderer goes.
    LOG(ERROR) << "BDK page broker refused: " << why;
    if (frame) {
      frame->GetProcess()->ShutdownForBadMessage(
          content::RenderProcessHost::CrashReportMode::NO_CRASH_DUMP);
    }
    return;
  }
  // A second request replaces the first connection, whose caller is revoked.
  broker_ = std::make_unique<PageBrokerImpl>(frame, std::move(caller),
                                             std::move(receiver));
}

WEB_UI_CONTROLLER_TYPE_IMPL(BundledPageUI)

BundledPageUIConfig::BundledPageUIConfig(std::string_view host)
    : content::WebUIConfig(content::kChromeUIScheme, host) {}

BundledPageUIConfig::~BundledPageUIConfig() = default;

bool BundledPageUIConfig::IsWebUIEnabled(
    content::BrowserContext* browser_context) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  bdkcore::BundledSource source;
  return rt && bdkcore::bundled_source(*rt, host(), &source);
}

std::unique_ptr<content::WebUIController>
BundledPageUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                           const GURL& url) {
  return std::make_unique<BundledPageUI>(web_ui, url);
}

void RegisterBundledPageUIs(const bdkcore::Runtime& rt) {
  static base::NoDestructor<std::set<std::string>> added;
  for (const std::string& host : bdkcore::bundled_hosts(rt)) {
    if (added->insert(host).second) {
      content::WebUIConfigMap::GetInstance().AddWebUIConfig(
          std::make_unique<BundledPageUIConfig>(host));
    }
  }
}

}  // namespace bdk
