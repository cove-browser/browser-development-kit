// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/devtools_frontend.h"

#include <memory>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "bdk/engine/identity.h"
#include "bdk/engine/json_util.h"
#include "chrome/browser/devtools/devtools_ui_bindings.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/ui/webui/devtools/devtools_ui.h"
#include "chrome/common/chrome_switches.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "net/base/filename_util.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"

namespace bdk {

namespace {

DevToolsConfig& Config() {
  static base::NoDestructor<DevToolsConfig> config;
  return *config;
}

// Where `bdk build` packages a DevTools directory inside the application.
base::FilePath BundledDevTools(const base::FilePath& bundle_path,
                               const char* leaf) {
  return bundle_path.Append("Contents")
      .Append("Resources")
      .Append("BDK")
      .Append("DevTools")
      .Append(leaf);
}

struct Overlay {
  std::string css;
  std::string js;
  bool found = false;
};

Overlay ReadOverlay(const base::FilePath& dir) {
  Overlay overlay;
  bool css = base::ReadFileToString(dir.Append("overlay.css"), &overlay.css);
  bool js = base::ReadFileToString(dir.Append("overlay.js"), &overlay.js);
  overlay.found = css || js;
  return overlay;
}

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
  void OnLoadCompleted() override {
    if (frontend_) {
      frontend_->SeedInspectedTabId();
    }
  }
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

void ConfigureDevTools(const std::string& resolved,
                       const base::FilePath& bundle_path) {
  DevToolsConfig& config = Config();
  config = DevToolsConfig();
  std::optional<base::DictValue> root = ParseJsonDict(resolved);
  const std::string* frontend =
      root ? root->FindStringByDottedPath("devtools.frontend") : nullptr;
  const std::string* surface =
      root ? root->FindStringByDottedPath("surfaces.devtools") : nullptr;
  if ((frontend && *frontend == "disabled") ||
      (surface && *surface == "disabled")) {
    config.frontend = DevToolsConfig::Frontend::kDisabled;
  } else if (frontend && *frontend != "default") {
    config.frontend = DevToolsConfig::Frontend::kReplaced;
    config.frontend_dir = BundledDevTools(bundle_path, "Frontend");
  }
  SetDevToolsEnabled(config.frontend != DevToolsConfig::Frontend::kDisabled);

  // Chromium serves devtools://devtools/bundled/ from this directory instead
  // of its own resources while the switch is set (DevToolsDataSource reads it
  // on every request), so the replacement needs no patch. It is process-wide:
  // a later runtime in the same process sets or clears it again.
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  command_line->RemoveSwitch(switches::kCustomDevtoolsFrontend);
  if (config.frontend == DevToolsConfig::Frontend::kReplaced) {
    // A directory URL ends with a separator, so the request path appends.
    command_line->AppendSwitchASCII(
        switches::kCustomDevtoolsFrontend,
        net::FilePathToFileURL(config.frontend_dir.AsEndingWithSeparator())
            .spec());
  }

  const std::string* overlay =
      root ? root->FindStringByDottedPath("devtools.overlay") : nullptr;
  if (config.frontend == DevToolsConfig::Frontend::kDefault && overlay) {
    config.has_overlay = true;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(&ReadOverlay, BundledDevTools(bundle_path, "Overlay")),
        base::BindOnce([](Overlay read) {
          DevToolsConfig& c = Config();
          if (!c.has_overlay) {
            return;  // a later start changed the configuration
          }
          if (!read.found) {
            LOG(WARNING) << "BDK: [devtools] overlay is set but the bundle has "
                            "no BDK/DevTools/Overlay/overlay.css or overlay.js";
          }
          c.overlay_css = std::move(read.css);
          c.overlay_js = std::move(read.js);
        }));
  }
}

const DevToolsConfig& CurrentDevToolsConfig() {
  return Config();
}

// static
GURL DevToolsFrontend::FrontendURL() {
  // As DevToolsWindow builds it for an undocked window on a tab target. A
  // replacement is served at the same origin from the application's
  // directory, entered through its index.html.
  const char* entry = Config().frontend == DevToolsConfig::Frontend::kReplaced
                          ? "index.html"
                          : "devtools_app.html";
  return GURL(std::string("devtools://devtools/bundled/") + entry +
              "?remoteBase=" + DevToolsUI::GetRemoteBaseURL().spec() +
              "&targetType=tab");
}

DevToolsFrontend::FrontendObserver::FrontendObserver(
    content::WebContents* frontend,
    DevToolsFrontend* owner)
    : content::WebContentsObserver(frontend), owner_(owner) {}

void DevToolsFrontend::FrontendObserver::DidFinishNavigation(
    content::NavigationHandle* handle) {
  owner_->Attach();
}

void DevToolsFrontend::FrontendObserver::
    DocumentOnLoadCompletedInPrimaryMainFrame() {
  owner_->ApplyOverlay();
}

void DevToolsFrontend::ApplyOverlay() {
  const DevToolsConfig& config = Config();
  if (!frontend_ || (config.overlay_css.empty() && config.overlay_js.empty())) {
    return;
  }
  // The overlay runs in the front end's own context with DevTools' powers
  // over the inspected page and nothing more (javascript-api.md section 6).
  // RenderFrameHost::ExecuteJavaScript is allowed on devtools:// documents;
  // the style sheet is added by script so the front end's CSP, which
  // governs script sources only, does not apply to it.
  std::string css;
  base::JSONWriter::Write(base::Value(config.overlay_css), &css);
  std::string script =
      "(() => { const s = document.createElement('style');"
      " s.dataset.bdkOverlay = '';"
      " s.textContent = " +
      css +
      ";"
      " (document.head || document.documentElement).appendChild(s); })();\n" +
      config.overlay_js;
  frontend_->GetPrimaryMainFrame()->ExecuteJavaScript(base::UTF8ToUTF16(script),
                                                      base::NullCallback());
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

void DevToolsFrontend::SetFrontend(content::WebContents* frontend) {
  frontend_ = frontend;
  frontend_observer_.Follow(frontend);
  attached_bindings_ = 0;
  Attach();
}

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

void DevToolsFrontend::SeedInspectedTabId() {
  DevToolsUIBindings* bindings = Bindings();
  if (bindings && inspected_) {
    bindings->CallClientMethod(
        "DevToolsAPI", "setInspectedTabId",
        base::Value(extensions::ExtensionTabUtil::GetTabId(inspected_)));
  }
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
