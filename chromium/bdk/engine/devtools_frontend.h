// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_DEVTOOLS_FRONTEND_H_
#define BDK_ENGINE_DEVTOOLS_FRONTEND_H_

#include <cstdint>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/web_contents_observer.h"
#include "url/gurl.h"

namespace content {
class WebContents;
}  // namespace content

class DevToolsUIBindings;

namespace bdk {

// Undocked DevTools for one page, hosted in a BDK page of its own instead of a
// Chrome window (T-ADP-05): Chrome's front end and its DevToolsUIBindings. The
// application places the front-end page like any other new window. Owned by
// the front-end page.
//
// DevToolsUIBindings owns its delegate and is replaced when the front end's
// navigation commits, so the bindings get a small forwarding delegate of their
// own (devtools_frontend.cc) that reaches this object through a weak pointer.
class DevToolsFrontend : public content::WebContentsObserver {
 public:
  // The URL of Chrome's bundled front end, for the front-end page to load.
  static GURL FrontendURL();

  // `frontend` has started loading FrontendURL(); `inspected` is the page to
  // inspect. `close` asks the engine to close the front-end page.
  DevToolsFrontend(content::WebContents* frontend,
                   content::WebContents* inspected,
                   base::RepeatingClosure close);
  ~DevToolsFrontend() override;

  content::WebContents* inspected() { return inspected_; }
  void Close() { close_.Run(); }
  void OpenInNewTab(const std::string& url);

  // content::WebContentsObserver, on the inspected page:
  void WebContentsDestroyed() override;

 private:
  // Watches the front-end page for the commits that replace its bindings.
  class FrontendObserver : public content::WebContentsObserver {
   public:
    FrontendObserver(content::WebContents* frontend, DevToolsFrontend* owner);
    void DidFinishNavigation(content::NavigationHandle* handle) override;

   private:
    raw_ptr<DevToolsFrontend> owner_;
  };

  // The front end's current bindings; never kept, because they are replaced.
  DevToolsUIBindings* Bindings();
  // Gives the current bindings a delegate and attaches them to the inspected
  // page, once per bindings object.
  void Attach();

  raw_ptr<content::WebContents> frontend_;
  raw_ptr<content::WebContents> inspected_;
  FrontendObserver frontend_observer_;
  // Identifies the bindings last attached; compared, never dereferenced.
  uintptr_t attached_bindings_ = 0;
  base::RepeatingClosure close_;
  base::WeakPtrFactory<DevToolsFrontend> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_DEVTOOLS_FRONTEND_H_
