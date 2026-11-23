// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_HOST_CONTEXT_H_
#define BDK_ENGINE_HOST_CONTEXT_H_

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "bdk/core/include/bdk/bdk_base.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"

class Browser;
class BrowserWindowInterface;
class Profile;

namespace bdkcore {
struct Window;
}  // namespace bdkcore

namespace content {
class WebContents;
}  // namespace content

namespace tabs {
class TabModel;
}  // namespace tabs

namespace bdk {

class BdkBrowserWindow;
class HostContexts;

// A host context (architecture.md section 4.1): BDK's wrapper around one
// Chromium `Browser`, in which BDK's pages are tabs. It is either the parking
// context of a profile (no native window, where a page is when no registered
// window shows it) or the context of one registered window for one profile
// (a `Browser` belongs to a profile, so a window showing pages of two profiles
// has two). The application never sees one.
//
// A page's WebContents is owned by the tab model of its context. It moves
// between contexts only through the tab model (`Release` then `Add`), so its
// tab and the services attached to it survive, and the tab's session
// identifier, which the extension tab identifier derives from, stays.
//
// A tab Chromium puts in the context itself (a link in an extension popup goes
// through Browser::OpenURL to the popup's Browser, which is a host context) is
// no page of BDK's. The context hands it to EngineContents::AdoptForeignTab,
// which makes it a page and asks the application where it goes, as a
// new-window request.
class HostContext : public TabStripModelObserver {
 public:
  ~HostContext() override;

  // Null once Chromium has destroyed the Browser on its own.
  Browser* browser() const;
  Profile* profile() const { return profile_; }
  // The registered window this context belongs to, or null for a parking
  // context.
  const bdkcore::Window* window() const { return window_; }
  bool is_parking() const { return window_ == nullptr; }
  BdkBrowserWindow* browser_window() const;

  int tab_count() const;
  bool Contains(content::WebContents* contents) const;
  bool IsActive(content::WebContents* contents) const;

  // Takes a page's WebContents as a new background tab.
  void Add(std::unique_ptr<content::WebContents> contents);
  // Takes a tab that another context released.
  void Add(std::unique_ptr<tabs::TabModel> tab);
  // Detaches the page's tab for insertion into another context.
  std::unique_ptr<tabs::TabModel> Release(content::WebContents* contents);
  // Detaches the tab and takes its WebContents out of it: for a tab that
  // Chromium inserted, which becomes a page of its own.
  std::unique_ptr<content::WebContents> Take(content::WebContents* contents);
  // Replaces the page's WebContents in its tab (restore, discard) and returns
  // the old one.
  std::unique_ptr<content::WebContents> Replace(
      content::WebContents* old_contents,
      std::unique_ptr<content::WebContents> new_contents);
  // Closes the page's tab, destroying its WebContents.
  void Destroy(content::WebContents* contents);
  // Makes the page's tab the active tab of this context. A context of a
  // window becomes the most recently active one.
  void Activate(content::WebContents* contents);

 private:
  friend class HostContexts;

  HostContext(Profile* profile, const bdkcore::Window* window);

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;

  raw_ptr<Profile> profile_;
  raw_ptr<const bdkcore::Window> window_;
  // True while a page swaps its own WebContents (Replace), so the observer
  // does not take the swap for Chromium's discard.
  bool replacing_ = false;
  base::WeakPtr<BrowserWindowInterface> browser_;
};

// The host contexts of the runtime, and the windows the application has
// registered.
class HostContexts {
 public:
  static HostContexts& Get();

  // Whether a context can be created for `profile` (Chromium refuses to
  // create a window for a profile that is being deleted).
  static bool CanHost(Profile* profile);

  // The parking context of `profile`, created on first use.
  HostContext& Parking(Profile* profile);
  // The context of `window` for `profile`, created on first use.
  HostContext& ForWindow(const bdkcore::Window* window,
                         bdk_native_handle_t native,
                         Profile* profile);
  // The context a page is in, or null if it is in none.
  HostContext* ContextOf(content::WebContents* contents);

  // Moves the page's tab to `to` through the tab model. Does nothing if it is
  // there already.
  void Move(content::WebContents* contents, HostContext& to);

  // The application registered `window`; a page whose view is already in it is
  // moved by the engine (EngineContents).
  void RegisterWindow(const bdkcore::Window* window,
                      bdk_native_handle_t native);
  // The registered window for a native window, or null.
  const bdkcore::Window* FindWindow(bdk_native_handle_t native) const;
  // Forgets `window` and destroys its contexts. Their pages must have moved
  // out (to parking) already.
  void UnregisterWindow(const bdkcore::Window* window);

  // The contexts, for pages to move out of before a window or profile goes.
  std::vector<HostContext*> ContextsOfWindow(const bdkcore::Window* window);
  std::vector<HostContext*> ContextsOfProfile(Profile* profile);
  // Destroys the contexts of `profile`, which must hold no page.
  void DestroyProfileContexts(Profile* profile);
  // Destroys every context; none may hold a page.
  void DestroyAll();

  // Whether `browser` is a host context's, and whether a parking context's.
  bool IsHostContext(const BrowserWindowInterface* browser) const;
  bool IsParkingContext(const BrowserWindowInterface* browser) const;
  size_t ContextCount() const { return contexts_.size(); }

 private:
  friend class base::NoDestructor<HostContexts>;
  HostContexts();
  ~HostContexts();

  HostContext& Create(Profile* profile,
                      const bdkcore::Window* window,
                      bdk_native_handle_t native);
  void Destroy(HostContext* context);

  std::vector<std::unique_ptr<HostContext>> contexts_;
  std::map<const bdkcore::Window*, bdk_native_handle_t> windows_;
};

}  // namespace bdk

#endif  // BDK_ENGINE_HOST_CONTEXT_H_
