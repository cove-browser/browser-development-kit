// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/devtools_frontend.h"

#include <memory>
#include <utility>

#include "chrome/browser/devtools/devtools_ui_bindings.h"
#include "chrome/browser/ui/webui/devtools/devtools_ui.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"

namespace bdk {

namespace {

// What the bindings own and call. Everything an undocked front end in a BDK
// page has no use for (docking, bounds, eye dropper, infobars) does nothing.
class BindingsDelegate : public DevToolsUIBindings::Delegate {
 public:
  explicit BindingsDelegate(base::WeakPtr<DevToolsFrontend> frontend)
      : frontend_(std::move(frontend)) {}

  content::WebContents* GetInspectedWebContents() override {
    return frontend_ ? frontend_->inspected() : nullptr;
  }
  void ActivateWindow() override {}
  void CloseWindow() override {
    if (frontend_) {
      frontend_->Close();
    }
  }
  void Inspect(scoped_refptr<content::DevToolsAgentHost> host) override {}
  void SetInspectedPageBounds(const gfx::Rect& rect) override {}
  void InspectElementCompleted() override {}
  void SetIsDocked(bool is_docked) override {}
  void OpenInNewTab(const std::string& url) override {
    if (frontend_) {
      frontend_->OpenInNewTab(url);
    }
  }
  void OpenSearchResultsInNewTab(const std::string& query) override {}
  void SetWhitelistedShortcuts(const std::string& message) override {}
  void SetEyeDropperActive(bool active) override {}
  void OpenNodeFrontend() override {}
  void InspectedContentsClosing() override { CloseWindow(); }
  void OnLoadCompleted() override {}
  void ReadyForTest() override {}
  void ConnectionReady() override {}
  void SetOpenNewWindowForPopups(bool value) override {}
  infobars::ContentInfoBarManager* GetInfoBarManager() override {
    return nullptr;
  }
  void RenderProcessGone(bool crashed) override {}
  void ShowCertificateViewer(const std::string& cert_chain) override {}
  int GetDockStateForLogging() override { return 0; }
  int GetOpenedByForLogging() override { return 0; }
  int GetClosedByForLogging() override { return 0; }

 private:
  base::WeakPtr<DevToolsFrontend> frontend_;
};

}  // namespace

// static
GURL DevToolsFrontend::FrontendURL() {
  // As DevToolsWindow builds it for an undocked window on a tab target.
  return GURL("devtools://devtools/bundled/devtools_app.html?remoteBase=" +
              DevToolsUI::GetRemoteBaseURL().spec() + "&targetType=tab");
}

DevToolsFrontend::FrontendObserver::FrontendObserver(
    content::WebContents* frontend,
    DevToolsFrontend* owner)
    : content::WebContentsObserver(frontend), owner_(owner) {}

void DevToolsFrontend::FrontendObserver::DidFinishNavigation(
    content::NavigationHandle* handle) {
  owner_->Attach();
}

DevToolsFrontend::DevToolsFrontend(content::WebContents* frontend,
                                   content::WebContents* inspected,
                                   base::RepeatingClosure close)
    : content::WebContentsObserver(inspected),
      frontend_(frontend),
      inspected_(inspected),
      frontend_observer_(frontend, this),
      close_(std::move(close)) {
  Attach();
}

DevToolsFrontend::~DevToolsFrontend() = default;

DevToolsUIBindings* DevToolsFrontend::Bindings() {
  return DevToolsUIBindings::ForWebContents(frontend_);
}

void DevToolsFrontend::Attach() {
  DevToolsUIBindings* bindings = Bindings();
  if (!bindings || !inspected_ ||
      reinterpret_cast<uintptr_t>(bindings) == attached_bindings_) {
    return;
  }
  attached_bindings_ = reinterpret_cast<uintptr_t>(bindings);
  bindings->SetDelegate(new BindingsDelegate(weak_factory_.GetWeakPtr()));
  bindings->AttachTo(content::DevToolsAgentHost::GetOrCreateForTab(inspected_));
}

void DevToolsFrontend::OpenInNewTab(const std::string& url) {
  if (!inspected_) {
    return;
  }
  content::OpenURLParams params(
      GURL(url), content::Referrer(), WindowOpenDisposition::kNewForegroundTab,
      ui::PAGE_TRANSITION_LINK, /*is_renderer_initiated=*/false);
  inspected_->OpenURL(params, {});
}

void DevToolsFrontend::WebContentsDestroyed() {
  inspected_ = nullptr;
  Observe(nullptr);
  if (DevToolsUIBindings* bindings = Bindings()) {
    bindings->Detach();
  }
}

}  // namespace bdk
