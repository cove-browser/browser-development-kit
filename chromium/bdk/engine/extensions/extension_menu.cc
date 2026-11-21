// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_menu.h"

#include <optional>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/action_service.h"
#include "bdk/engine/extensions/extension_ui_platform.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/host_context.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/ui_presenters.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace bdk {

namespace {

constexpr char kManageId[] = "manage";
constexpr char kManageTitle[] = "Manage Extensions";

std::string StringOf(const base::DictValue& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

// Runs what the person chose, for the page the menu was opened for.
void Choose(base::WeakPtr<EngineContents> weak_page, const std::string& id) {
  EngineContents* page = weak_page.get();
  std::shared_ptr<bdkcore::Contents> core = page ? page->Core() : nullptr;
  if (!core) {
    return;
  }
  if (id == kManageId) {
    OpenManageExtensions(*page);
    return;
  }
  // A choice in BDK's own menu is the user's click: the menu is the proof.
  if (const extensions::Extension* extension =
          EnabledExtension(ProfileOf(*core), id)) {
    RunExtensionAction(*page, *extension, PopupTrigger::kUserClick);
  }
}

void Show(base::WeakPtr<EngineContents> weak_page,
          std::weak_ptr<bdkcore::Window> weak_window,
          bdk_native_handle_t anchor_view) {
  EngineContents* page = weak_page.get();
  std::shared_ptr<bdkcore::Window> window = weak_window.lock();
  std::shared_ptr<bdkcore::Contents> core = page ? page->Core() : nullptr;
  if (!core || !window) {
    return;
  }
  base::ListValue states =
      ExtensionActionStates(ProfileOf(*core), page->web_contents());
  std::vector<ExtensionMenuEntry> entries;
  base::ListValue items;
  for (const base::Value& value : states) {
    const base::DictValue* state = value.GetIfDict();
    if (!state) {
      continue;
    }
    ExtensionMenuEntry entry;
    entry.id = StringOf(*state, "id");
    entry.title = StringOf(*state, "title");
    entry.badge = StringOf(*state, "badgeText");
    entry.icon = StringOf(*state, "icon");
    entry.enabled = state->FindBool("isEnabled").value_or(false) ||
                    state->FindBool("wantsAccess").value_or(false);
    items.Append(state->Clone());
    entries.push_back(std::move(entry));
  }
  if (!entries.empty()) {
    entries.push_back(ExtensionMenuEntry());  // A separator.
  }
  ExtensionMenuEntry manage;
  manage.id = kManageId;
  manage.title = kManageTitle;
  entries.push_back(std::move(manage));

  base::DictValue payload;
  payload.Set("items", std::move(items));
  if (std::optional<Dismisser> dismiss = ScriptedPresent(
          "extension_menu", payload,
          base::BindOnce(
              [](base::WeakPtr<EngineContents> page,
                 const base::DictValue& answer) {
                if (const std::string* id = answer.FindString("choose")) {
                  Choose(page, *id);
                }
              },
              weak_page))) {
    return;  // A test scripted the menu: nothing is shown.
  }
  RecordPresented("extension_menu", payload);
  std::optional<WindowRect> anchor;
  WindowRect rect;
  if (bdkcore::window_anchor(*window, BDK_ANCHOR_EXTENSIONS, rect.data())) {
    anchor = rect;
  }
  ShowExtensionsMenu(
      window->native, anchor, anchor_view, entries,
      base::BindOnce([](base::WeakPtr<EngineContents> page,
                        const std::string& id) { Choose(page, id); },
                     weak_page));
}

}  // namespace

EngineContents* FocusedPageOf(const bdkcore::Window& window) {
  EngineContents* active = nullptr;
  for (EngineContents* page : EngineContents::All()) {
    if (!page->IsInWindow(&window)) {
      continue;
    }
    if (page->focused()) {
      return page;
    }
    HostContext* context = HostContexts::Get().ContextOf(page->web_contents());
    if (!active && context && context->IsActive(page->web_contents())) {
      active = page;
    }
  }
  return active;
}

bool PresentExtensionsMenu(bdkcore::Runtime& rt,
                           const std::shared_ptr<bdkcore::Window>& window,
                           bdk_native_handle_t anchor_view,
                           std::string* error) {
  EngineContents* page = FocusedPageOf(*window);
  if (!page) {
    *error = "the window shows no page";
    return false;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&Show, page->GetWeakPtr(),
                     std::weak_ptr<bdkcore::Window>(window), anchor_view));
  return true;
}

void OpenManageExtensions(EngineContents& page) {
  std::shared_ptr<bdkcore::Contents> core = page.Core();
  if (!core) {
    return;
  }
  const GURL url("chrome://extensions");
  std::unique_ptr<content::WebContents> created =
      content::WebContents::Create(content::WebContents::CreateParams(
          page.web_contents()->GetBrowserContext()));
  created->GetController().LoadURLWithParams(
      content::NavigationController::LoadURLParams(url));
  EngineContents::RequestNewPage(
      core->profile, core, std::move(created), "newPage",
      WindowOpenDisposition::kNewForegroundTab, url, nullptr,
      /*user_gesture=*/true, std::string());
}

}  // namespace bdk
