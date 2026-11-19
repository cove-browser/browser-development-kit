// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_ENGINE_CONTENTS_H_
#define BDK_ENGINE_ENGINE_CONTENTS_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "bdk/core/runtime/core_internal.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"

class Profile;
class ScopedProfileKeepAlive;

namespace bdk {

class HostView;

// What the engine keeps for a profile the application opened.
struct EngineProfile : bdkcore::BackendData {
  EngineProfile();
  ~EngineProfile() override;

  raw_ptr<Profile> profile = nullptr;
  // Keeps a persistent profile loaded while the application has it open.
  std::unique_ptr<ScopedProfileKeepAlive> keep_alive;
};

Profile* ProfileOf(const bdkcore::Contents& contents);

// One page: its WebContents, its host view, and the translation of
// Chromium's observer and delegate calls into BDK events and UI requests.
// Owned by the core's Contents (as its backend data); it refers back to it
// weakly.
class EngineContents : public bdkcore::BackendData,
                       public content::WebContentsDelegate,
                       public content::WebContentsObserver {
 public:
  // Takes `web_contents` (created by the engine or by Chromium for a popup).
  EngineContents(const std::shared_ptr<bdkcore::Contents>& contents,
                 std::unique_ptr<content::WebContents> web_contents);
  ~EngineContents() override;

  static EngineContents* From(const bdkcore::Contents& contents);

  content::WebContents* web_contents() { return web_contents_.get(); }
  HostView& host_view() { return *host_view_; }

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
  std::shared_ptr<bdkcore::Contents> core() const { return core_.lock(); }
  void Emit(uint32_t kind,
            const std::string& url,
            const std::string& detail,
            int32_t net_error,
            uint32_t flags);
  void Attach(std::unique_ptr<content::WebContents> web_contents);
  // Wraps `new_contents` as a page in the same profile and asks the
  // application where it goes (a new-window request).
  content::WebContents* RequestNewWindow(
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
  std::unique_ptr<content::WebContents> web_contents_;
  std::string user_agent_;
  double zoom_ = 1.0;
  bool auto_discardable_ = true;
  bool discarded_ = false;
  bool fullscreen_ = false;
  bool close_reported_ = false;
  base::WeakPtrFactory<EngineContents> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_ENGINE_CONTENTS_H_
