// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_ENGINE_CONTENTS_H_
#define BDK_ENGINE_ENGINE_CONTENTS_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/services/events.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"

class Profile;

namespace bdk {

class DevToolsFrontend;
class HostContext;
class HostView;
class PageDialogs;
class PageFind;

// What the engine keeps for a profile the application opened.
struct EngineProfile : bdkcore::BackendData {
  EngineProfile();
  ~EngineProfile() override;

  raw_ptr<Profile> profile = nullptr;
  // Observes the profile's history, bookmarks and downloads for the service
  // events.
  std::unique_ptr<ServiceEvents> events;
};

Profile* ProfileOf(const bdkcore::Contents& contents);

// One page: its WebContents, its host view, and the translation of
// Chromium's observer and delegate calls into BDK events and UI requests.
// Owned by the core's Contents (as its backend data); it refers back to it
// weakly. The WebContents is a tab of a host context (host_context.h), and its
// tab model owns it. BDK's delegate serves the page in every context: after a
// move the engine sets it again, because the Browser takes the delegate
// when it takes the tab.
class EngineContents : public bdkcore::BackendData,
                       public content::WebContentsDelegate,
                       public content::WebContentsObserver {
 public:
  // Takes `web_contents` (created by the engine or by Chromium for a popup) as
  // a tab of the parking context of the page's profile, which must be able to
  // host one (HostContexts::CanHost).
  EngineContents(const std::shared_ptr<bdkcore::Contents>& contents,
                 std::unique_ptr<content::WebContents> web_contents);
  ~EngineContents() override;

  static EngineContents* From(const bdkcore::Contents& contents);
  // The page whose WebContents is `contents`, or null.
  static EngineContents* FromWebContents(content::WebContents* contents);

  // The core's side of the page, null once the application destroyed it.
  std::shared_ptr<bdkcore::Contents> Core() const { return core_.lock(); }
  base::WeakPtr<EngineContents> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  // Find in page (ui_find.h).
  PageFind& find() { return *find_; }

  content::WebContents* web_contents() { return web_contents_; }
  HostView& host_view() { return *host_view_; }

  // Every page of the engine, in creation order.
  static const std::vector<EngineContents*>& All();

  // Moves the page's tab into `context`. A page with a focus keeps being the
  // active tab of the context it moves into.
  void MoveTo(HostContext& context);
  // Moves the page to the parking context of its profile.
  void Park();
  // Moves the page into the context of the registered window its host view is
  // in, if there is one. A page in no registered window stays where it is.
  void Reconcile();
  // Whether the page is a tab of a context of `window`.
  bool IsInWindow(const bdkcore::Window* window);
  // Focuses the page and makes its tab the active tab of its context.
  void Focus();

  void Load(const std::string& url);
  // Replaces the WebContents with one restored from `state`, keeping the
  // handle and the host view.
  void ReplaceWebContents(std::unique_ptr<content::WebContents> web_contents);
  // Replaces the page with an unloaded copy of its history; it loads again
  // when it is shown or focused.
  void Discard();
  void Close();
  void SetZoom(double zoom);
  double Zoom();
  void SetUserAgent(const std::string& user_agent);
  // Mutes the page, including the WebContents a discard or restore puts in its
  // place.
  void SetMuted(bool muted);
  // Whether the page has focus, as the last focus event said.
  bool focused() const { return focused_; }
  // Between Close() and Chromium's CloseContents().
  bool closing() const { return closing_; }
  void LoadIfDiscarded();
  bool auto_discardable() const { return auto_discardable_; }
  void set_auto_discardable(bool value) { auto_discardable_ = value; }

  // content::WebContentsDelegate:
  content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params,
      base::OnceCallback<void(content::NavigationHandle&)>
          navigation_handle_callback) override;
  content::WebContents* AddNewContents(
      content::WebContents* source,
      std::unique_ptr<content::WebContents> new_contents,
      const GURL& target_url,
      WindowOpenDisposition disposition,
      const blink::mojom::WindowFeatures& window_features,
      bool user_gesture,
      bool* was_blocked) override;
  void CloseContents(content::WebContents* source) override;
  bool HandleContextMenu(content::RenderFrameHost& render_frame_host,
                         const content::ContextMenuParams& params) override;
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      content::WebContents* source,
      const input::NativeWebKeyboardEvent& event) override;
  bool HandleKeyboardEvent(content::WebContents* source,
                           const input::NativeWebKeyboardEvent& event) override;
  bool CanEnterFullscreenModeForTab(
      content::RenderFrameHost* requesting_frame) override;
  void EnterFullscreenModeForTab(
      content::RenderFrameHost* requesting_frame,
      const blink::mojom::FullscreenOptions& options) override;
  void ExitFullscreenModeForTab(content::WebContents* source) override;
  bool IsFullscreenForTabOrPending(
      const content::WebContents* web_contents) override;
  content::JavaScriptDialogManager* GetJavaScriptDialogManager(
      content::WebContents* source) override;
  void RunFileChooser(content::RenderFrameHost* render_frame_host,
                      scoped_refptr<content::FileSelectListener> listener,
                      const blink::mojom::FileChooserParams& params) override;
  void EnumerateDirectory(content::WebContents* web_contents,
                          scoped_refptr<content::FileSelectListener> listener,
                          const base::FilePath& path) override;
  void RequestMediaAccessPermission(
      content::WebContents* web_contents,
      const content::MediaStreamRequest& request,
      content::MediaResponseCallback callback) override;
  bool CheckMediaAccessPermission(content::RenderFrameHost* render_frame_host,
                                  const url::Origin& security_origin,
                                  blink::mojom::MediaStreamType type) override;

  // content::WebContentsObserver:
  void DidStartNavigation(content::NavigationHandle* handle) override;
  void DidFinishNavigation(content::NavigationHandle* handle) override;
  void DidStopLoading() override;
  void TitleWasSet(content::NavigationEntry* entry) override;
  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override;
  void OnWebContentsFocused(
      content::RenderWidgetHost* render_widget_host) override;
  void OnWebContentsLostFocus(
      content::RenderWidgetHost* render_widget_host) override;
  void OnVisibilityChanged(content::Visibility visibility) override;

 private:
  std::shared_ptr<bdkcore::Contents> core() const { return Core(); }
  void Emit(uint32_t kind,
            const std::string& url,
            const std::string& detail,
            int32_t net_error,
            uint32_t flags);
  // Makes `web_contents` the page's WebContents: its first, as a tab of the
  // parking context, or a replacement in the same tab.
  void Attach(std::unique_ptr<content::WebContents> web_contents);
  // The host view moved into or out of a window; the page follows later.
  void OnHostViewWindowChanged();
  void ActivateTab();
  // Opens undocked DevTools for this page in a page of its own, through a
  // new-window request of kind "devTools".
  void OpenDevTools();
  // Asks the application to close this page, as window.close() does.
  void RequestClose();
  // Wraps `new_contents` as a page in the same profile and asks the
  // application where it goes (a new-window request). Returns the new page.
  EngineContents* RequestNewWindow(
      std::unique_ptr<content::WebContents> new_contents,
      const std::string& kind,
      WindowOpenDisposition disposition,
      const GURL& target_url,
      const blink::mojom::WindowFeatures* features,
      bool user_gesture);
  void PerformMenuItem(const content::ContextMenuParams& params,
                       const std::string& id);
  void SetFullscreen(bool fullscreen);

  std::weak_ptr<bdkcore::Contents> core_;
  std::unique_ptr<HostView> host_view_;
  // JavaScript dialogs and file pickers; find in page (ui_dialogs.h,
  // ui_find.h).
  std::unique_ptr<PageDialogs> dialogs_;
  std::unique_ptr<PageFind> find_;
  // Owned by its tab model, in a host context.
  raw_ptr<content::WebContents> web_contents_ = nullptr;
  // For a DevTools front-end page: the bindings to the page it inspects.
  std::unique_ptr<DevToolsFrontend> devtools_;
  std::string user_agent_;
  bool muted_ = false;
  double zoom_ = 1.0;
  bool auto_discardable_ = true;
  bool discarded_ = false;
  bool fullscreen_ = false;
  bool close_reported_ = false;
  bool focused_ = false;
  bool closing_ = false;
  bool reconcile_pending_ = false;
  // The last main-frame, cross-document navigation failed or showed an error.
  bool last_navigation_failed_ = false;
  base::WeakPtrFactory<EngineContents> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_ENGINE_CONTENTS_H_
