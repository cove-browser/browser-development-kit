// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/host_context.h"

#include <algorithm>

#include "base/check.h"
#include "base/functional/bind.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/bdk_browser_window.h"
#include "bdk/engine/host_window.h"
#include "bdk/engine/window_registry.h"
#include "chrome/browser/extensions/embedder_extension_window_filter.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_active_state_manager/browser_active_state_manager.h"
#include "chrome/browser/ui/browser_manager_service.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/browser_window/public/embedder_browser_window_factory.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/unload_controller.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/base_window.h"

namespace bdk {

namespace {

// The window the factory made last, for the context being created.
BdkBrowserWindow* g_created_window = nullptr;
// Whether the context being created is a parking context, and its window then:
// extensions must not see it (see HiddenFromExtensions).
bool g_creating_parking = false;
const ui::BaseWindow* g_hidden_window = nullptr;

std::unique_ptr<BrowserWindow, BrowserWindowDeleter> CreateWindow(
    BrowserWindowInterface* browser,
    bool user_gesture,
    bool in_tab_dragging) {
  auto window = std::make_unique<BdkBrowserWindow>(browser);
  g_created_window = window.get();
  if (g_creating_parking) {
    g_hidden_window = window.get();
  }
  return std::unique_ptr<BrowserWindow, BrowserWindowDeleter>(window.release());
}

// The extension windows API lists every Browser. A parking context is where
// pages wait that no window shows; it is not a window of the application, so
// windows.getAll, tabs.query and windows.onCreated must not report it
// (T-EXT-01 assertion 10). The extension window controller asks this as it is
// made, which is before the context exists.
bool HiddenFromExtensions(BrowserWindowInterface* browser) {
  return g_hidden_window && browser->GetWindow() == g_hidden_window;
}

// Chromium closes a Browser whose tab strip empties: `UnloadController`'s
// `TabStripEmpty` starts a window close, and a task it posts when the last tab
// detaches deletes the Browser. A context's strip empties whenever its last
// page moves to another context or is destroyed, which is not a request to
// close it (BDK destroys a context itself, with its window or profile), so the
// close is cancelled before the posted task runs. Without this the context
// pointed at a deleted Browser, and the next page to enter it hit
// `CHECK(browser_)`.
void KeepOpenWhenEmpty(BrowserWindowInterface* browser) {
  if (!browser->GetTabStripModel()->empty()) {
    return;
  }
  UnloadController* unload = UnloadController::From(browser);
  if (unload && unload->is_attempting_to_close_browser()) {
    unload->CancelWindowClose();
  }
}

}  // namespace

HostContext::HostContext(Profile* profile, const bdkcore::Window* window)
    : profile_(profile), window_(window) {}

HostContext::~HostContext() {
  WindowRegistry::Get().ForgetContext(this);
  if (browser_) {
    // A context is destroyed after its pages moved out. The Browser closes
    // the way Chromium closes any other.
    CHECK_EQ(0, tab_count());
    BdkBrowserWindow::ScopedTabOperation operation(browser_window());
    BrowserManagerService::SynchronouslyDestroyBrowser(browser_.get());
  }
}

Browser* HostContext::browser() const {
  // The factory makes a host context's window and Browser, and nothing else
  // makes either, so the interface is a Browser.
  return static_cast<Browser*>(browser_.get());
}

BdkBrowserWindow* HostContext::browser_window() const {
  return browser_ ? static_cast<BdkBrowserWindow*>(
                        BrowserWindow::FromBrowser(browser_.get()))
                  : nullptr;
}

int HostContext::tab_count() const {
  return browser_ ? browser_->GetTabStripModel()->count() : 0;
}

bool HostContext::Contains(content::WebContents* contents) const {
  return browser_ && browser_->GetTabStripModel()->GetIndexOfWebContents(
                         contents) != TabStripModel::kNoTab;
}

bool HostContext::IsActive(content::WebContents* contents) const {
  return browser_ &&
         browser_->GetTabStripModel()->GetActiveWebContents() == contents;
}

void HostContext::Add(std::unique_ptr<content::WebContents> contents) {
  CHECK(browser_);
  TabStripModel* strip = browser_->GetTabStripModel();
  strip->InsertWebContentsAt(strip->count(), std::move(contents),
                             AddTabTypes::ADD_NONE);
}

void HostContext::Add(std::unique_ptr<tabs::TabModel> tab) {
  CHECK(browser_);
  TabStripModel* strip = browser_->GetTabStripModel();
  strip->InsertDetachedTabAt(strip->count(), std::move(tab),
                             AddTabTypes::ADD_NONE);
}

std::unique_ptr<tabs::TabModel> HostContext::Release(
    content::WebContents* contents) {
  TabStripModel* strip = browser_->GetTabStripModel();
  const int index = strip->GetIndexOfWebContents(contents);
  CHECK_NE(index, TabStripModel::kNoTab);
  // The strip may empty, which makes Chromium close the window: not a request
  // for the application.
  BdkBrowserWindow::ScopedTabOperation operation(browser_window());
  std::unique_ptr<tabs::TabModel> tab = strip->DetachTabAtForInsertion(index);
  KeepOpenWhenEmpty(browser_.get());
  return tab;
}

std::unique_ptr<content::WebContents> HostContext::Replace(
    content::WebContents* old_contents,
    std::unique_ptr<content::WebContents> new_contents) {
  return browser_->GetTabStripModel()->DiscardWebContents(
      old_contents, std::move(new_contents));
}

void HostContext::Destroy(content::WebContents* contents) {
  if (!browser_) {
    return;
  }
  TabStripModel* strip = browser_->GetTabStripModel();
  const int index = strip->GetIndexOfWebContents(contents);
  if (index != TabStripModel::kNoTab) {
    BdkBrowserWindow::ScopedTabOperation operation(browser_window());
    strip->DetachAndDeleteWebContentsAt(index);
    KeepOpenWhenEmpty(browser_.get());
  }
}

void HostContext::Activate(content::WebContents* contents) {
  TabStripModel* strip = browser_->GetTabStripModel();
  const int index = strip->GetIndexOfWebContents(contents);
  if (index == TabStripModel::kNoTab) {
    return;
  }
  strip->ActivateTabAt(index);
  if (!is_parking()) {
    BrowserActiveStateManager::From(browser_.get())->DidBecomeActive();
    // This context holds the window's active page now.
    WindowRegistry::Get().NoteActive(window_, this);
  }
}

// static
HostContexts& HostContexts::Get() {
  static base::NoDestructor<HostContexts> contexts;
  return *contexts;
}

HostContexts::HostContexts() {
  embedder_browser_window_factory::SetFactory(
      base::BindRepeating(&CreateWindow));
  extensions::embedder_extension_window_filter::SetFilter(
      base::BindRepeating(&HiddenFromExtensions));
}

HostContexts::~HostContexts() = default;

// static
bool HostContexts::CanHost(Profile* profile) {
  return GetBrowserWindowCreationStatusForProfile(*profile) ==
         BrowserWindowInterface::CreationStatus::kOk;
}

HostContext& HostContexts::Create(Profile* profile,
                                  const bdkcore::Window* window,
                                  bdk_native_handle_t native) {
  // A popup window (bdk_window_register's kind) is a popup to extensions: it
  // is what windows.get reports. Everything else is a normal window.
  const bool popup = window && window->kind == BDK_WINDOW_KIND_POPUP;
  BrowserWindowCreateParams params(popup ? BrowserWindowInterface::TYPE_POPUP
                                         : BrowserWindowInterface::TYPE_NORMAL,
                                   profile, /*from_user_gesture=*/false);
  // The application owns what is open and restores it itself.
  params.omit_from_session_restore = true;
  params.should_trigger_session_restore = false;
  g_created_window = nullptr;
  g_creating_parking = window == nullptr;
  g_hidden_window = nullptr;
  BrowserWindowInterface* browser = CreateBrowserWindow(std::move(params));
  g_creating_parking = false;
  g_hidden_window = nullptr;
  CHECK(browser);
  CHECK(g_created_window);
  g_created_window->InstallAutofillBubbleHandler();
  g_created_window->InstallSidePanelUI();
  if (window) {
    g_created_window->SetRegisteredWindow(window->handle);
  }
  std::unique_ptr<HostContext> context(new HostContext(profile, window));
  context->browser_ = browser->GetWeakPtr();
  if (window) {
    BdkBrowserWindow* browser_window = g_created_window;
    browser_window->SetHostWindow(HostWindow::Wrap(
        native, base::BindRepeating(&BdkBrowserWindow::OnActiveChanged,
                                    base::Unretained(browser_window))));
    browser_window->InstallExtensionsContainer(native);
    // The window may be the key window already.
    if (browser_window->IsActive()) {
      browser_window->OnActiveChanged(true);
    }
  }
  g_created_window = nullptr;
  contexts_.push_back(std::move(context));
  return *contexts_.back();
}

HostContext& HostContexts::Parking(Profile* profile) {
  for (const auto& context : contexts_) {
    if (context->is_parking() && context->profile() == profile) {
      return *context;
    }
  }
  return Create(profile, nullptr, bdk_native_handle_t{});
}

HostContext& HostContexts::ForWindow(const bdkcore::Window* window,
                                     bdk_native_handle_t native,
                                     Profile* profile) {
  for (const auto& context : contexts_) {
    if (context->window() == window && context->profile() == profile) {
      return *context;
    }
  }
  return Create(profile, window, native);
}

HostContext* HostContexts::ContextOf(content::WebContents* contents) {
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (!tab) {
    return nullptr;
  }
  BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
  for (const auto& context : contexts_) {
    if (context->browser_.get() == browser) {
      return context.get();
    }
  }
  return nullptr;
}

void HostContexts::Move(content::WebContents* contents, HostContext& to) {
  HostContext* from = ContextOf(contents);
  CHECK(from);
  if (from == &to) {
    return;
  }
  // The tab model moves; the WebContents is never reparented behind it.
  to.Add(from->Release(contents));
}

void HostContexts::RegisterWindow(const bdkcore::Window* window,
                                  bdk_native_handle_t native) {
  windows_[window] = native;
}

const bdkcore::Window* HostContexts::FindWindow(
    bdk_native_handle_t native) const {
  if (!native.pointer) {
    return nullptr;
  }
  for (const auto& [window, handle] : windows_) {
    if (handle.kind == native.kind && handle.pointer == native.pointer) {
      return window;
    }
  }
  return nullptr;
}

void HostContexts::UnregisterWindow(const bdkcore::Window* window) {
  windows_.erase(window);
  for (HostContext* context : ContextsOfWindow(window)) {
    Destroy(context);
  }
  WindowRegistry::Get().ForgetWindow(window);
}

std::vector<HostContext*> HostContexts::ContextsOfWindow(
    const bdkcore::Window* window) {
  std::vector<HostContext*> result;
  for (const auto& context : contexts_) {
    if (context->window() == window) {
      result.push_back(context.get());
    }
  }
  return result;
}

std::vector<HostContext*> HostContexts::ContextsOfProfile(Profile* profile) {
  std::vector<HostContext*> result;
  for (const auto& context : contexts_) {
    if (context->profile() == profile) {
      result.push_back(context.get());
    }
  }
  return result;
}

void HostContexts::DestroyProfileContexts(Profile* profile) {
  for (HostContext* context : ContextsOfProfile(profile)) {
    Destroy(context);
  }
}

void HostContexts::DestroyAll() {
  while (!contexts_.empty()) {
    Destroy(contexts_.back().get());
  }
  windows_.clear();
}

void HostContexts::Destroy(HostContext* context) {
  auto it = std::ranges::find_if(
      contexts_, [context](const auto& c) { return c.get() == context; });
  CHECK(it != contexts_.end());
  // Out of the list first: the Browser's teardown must not find it.
  std::unique_ptr<HostContext> owned = std::move(*it);
  contexts_.erase(it);
}

bool HostContexts::IsHostContext(const BrowserWindowInterface* browser) const {
  return std::ranges::any_of(contexts_, [browser](const auto& context) {
    return context->browser_.get() == browser;
  });
}

}  // namespace bdk
