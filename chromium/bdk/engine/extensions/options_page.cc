// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/options_page.h"

#include <memory>
#include <optional>
#include <string>

#include "base/functional/bind.h"
#include "base/strings/stringprintf.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "chrome/browser/extensions/embedder_options_page_hook.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/common/webui_url_constants.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest_handlers/options_page_info.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace bdk {

namespace {

// Where the options page loads: the page itself, or, for an extension that
// wants it embedded in its management page (open_in_tab false), that page
// pointed at the extension, which an application with its own management page
// handles.
GURL OptionsUrl(const extensions::Extension& extension) {
  if (extensions::OptionsPageInfo::ShouldOpenInTab(&extension)) {
    return extensions::OptionsPageInfo::GetOptionsPage(&extension);
  }
  return GURL(base::StringPrintf(
      "%s?options=%s", chrome::kChromeUIExtensionsURL, extension.id().c_str()));
}

// A page of `profile` to be the opener of the request: `preferred` if it is
// one, else any.
std::shared_ptr<bdkcore::Contents> OpenerIn(
    Profile* profile,
    const std::shared_ptr<bdkcore::Contents>& preferred) {
  if (preferred) {
    return preferred;
  }
  for (EngineContents* page : EngineContents::All()) {
    std::shared_ptr<bdkcore::Contents> core = page->Core();
    if (core && !core->dead && ProfileOf(*core) == profile) {
      return core;
    }
  }
  return nullptr;
}

// Opens the options page: a new page in the profile, and the request for the
// application to place it. False when it could not be asked for.
bool Open(Profile* profile,
          const extensions::Extension& extension,
          const GURL& url,
          const std::shared_ptr<bdkcore::Contents>& preferred_opener) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt || rt->shutting) {
    return false;
  }
  std::shared_ptr<bdkcore::Profile> bdk_profile = BdkProfileOf(*rt, profile);
  if (!bdk_profile) {
    return false;
  }
  std::unique_ptr<content::WebContents> created =
      content::WebContents::Create(content::WebContents::CreateParams(profile));
  created->GetController().LoadURLWithParams(
      content::NavigationController::LoadURLParams(url));
  return EngineContents::RequestNewPage(
             bdk_profile, OpenerIn(profile, preferred_opener),
             std::move(created), "newPage",
             WindowOpenDisposition::kNewForegroundTab, url, nullptr,
             /*user_gesture=*/true, extension.id()) != nullptr;
}

void OpenOptionsCommand(const std::shared_ptr<ServiceCall>& call) {
  if (!bdkcore::ui_surface_available(call->runtime(),
                                     BDK_SURFACE_EXTENSION_OPTIONS_PAGES)) {
    call->Reject(BDK_DOMAIN_EXTENSION, BDK_CODE_SURFACE_DISABLED,
                 "options pages cannot be opened: [extensions.ui] "
                 "options_pages is disabled");
    return;
  }
  const std::string id = call->StringOr("id", std::string());
  const extensions::Extension* extension =
      EnabledExtension(call->profile(), id);
  if (!extension) {
    call->Reject(BDK_DOMAIN_EXTENSION, BDK_CODE_NOT_FOUND,
                 "no extension \"" + id + "\" is installed and enabled");
    return;
  }
  if (!extensions::OptionsPageInfo::HasOptionsPage(extension)) {
    call->Reject(BDK_DOMAIN_EXTENSION, BDK_CODE_NOT_FOUND,
                 "the extension has no options page");
    return;
  }
  if (!Open(call->profile(), *extension, OptionsUrl(*extension),
            call->builtin().contents)) {
    call->Reject(BDK_DOMAIN_EXTENSION, BDK_CODE_NOT_FOUND,
                 "the options page could not be opened in this profile");
    return;
  }
  call->ResolveNull();
}

std::optional<bool> OnOpenOptionsPage(BrowserWindowInterface* browser,
                                      const extensions::Extension* extension,
                                      const GURL& url,
                                      bool open_in_tab) {
  Profile* profile = browser->GetProfile();
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt || !BdkProfileOf(*rt, profile)) {
    return std::nullopt;  // Not a page of BDK's: Chromium's own path.
  }
  if (!bdkcore::ui_surface_available(*rt,
                                     BDK_SURFACE_EXTENSION_OPTIONS_PAGES)) {
    return false;
  }
  // The opener is the page the window shows.
  std::shared_ptr<bdkcore::Contents> opener;
  if (TabListInterface* tabs = TabListInterface::From(browser)) {
    if (tabs::TabInterface* tab = tabs->GetActiveTab()) {
      if (EngineContents* page =
              EngineContents::FromWebContents(tab->GetContents())) {
        opener = page->Core();
      }
    }
  }
  return Open(profile, *extension, url, opener);
}

}  // namespace

void InstallExtensionOptionsPageHook() {
  embedder_options_page_hook::SetHook(base::BindRepeating(&OnOpenOptionsPage));
}

void AddOptionsPageHandlers(HandlerTable& table) {
  table["extensions.openOptions"] = &OpenOptionsCommand;
}

}  // namespace bdk
