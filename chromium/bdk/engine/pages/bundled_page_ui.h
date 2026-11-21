// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_PAGES_BUNDLED_PAGE_UI_H_
#define BDK_ENGINE_PAGES_BUNDLED_PAGE_UI_H_

#include <memory>
#include <string>
#include <string_view>

#include "bdk/engine/pages/page_broker.mojom-forward.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

class GURL;

namespace bdkcore {
struct Runtime;
}

namespace bdk {

class PageBrokerImpl;

// The WebUI controller of every bundled page (chrome://bdk-<route>) and of
// BDK's own new-tab page (T-WUI-03, T-WUI-04). Its own controller type, so
// the per-WebUI interface broker binds the page broker for it and for nothing
// else; a frame of any other page that asks has its renderer terminated. It
// adds the page's data source to the profile, and owns the page's broker
// connection, so the connection goes with the document.
class BundledPageUI : public ui::MojoWebUIController {
 public:
  BundledPageUI(content::WebUI* web_ui, const GURL& url);
  BundledPageUI(const BundledPageUI&) = delete;
  BundledPageUI& operator=(const BundledPageUI&) = delete;
  ~BundledPageUI() override;

  // Called by the interface broker (BdkContentBrowserClient). Checks the
  // frame (BindPageCaller) and terminates its renderer when it may not have
  // the broker.
  void BindInterface(mojo::PendingReceiver<mojom::PageBroker> receiver);

 private:
  std::unique_ptr<PageBrokerImpl> broker_;

  WEB_UI_CONTROLLER_TYPE_DECL();
};

// One bundled host. Enabled only while the route table serves the host.
class BundledPageUIConfig : public content::WebUIConfig {
 public:
  explicit BundledPageUIConfig(std::string_view host);
  ~BundledPageUIConfig() override;

  // content::WebUIConfig:
  bool IsWebUIEnabled(content::BrowserContext* browser_context) override;
  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

// Adds a config for every host the route table can serve a bundled page or
// BDK's new-tab page at. A host is added once per process: Chromium outlives
// a runtime when BDK_ENGINE_KEEP_CHROMIUM is set, and the map refuses a
// second config for a host.
void RegisterBundledPageUIs(const bdkcore::Runtime& rt);

}  // namespace bdk

#endif  // BDK_ENGINE_PAGES_BUNDLED_PAGE_UI_H_
