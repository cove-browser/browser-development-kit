// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_ENGINE_CONTENTS_H_
#define BDK_ENGINE_ENGINE_CONTENTS_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/services/events.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"

class Profile;

namespace bdk {

class DevToolsFrontend;
class ExtensionMenuItems;
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
  // The page's handle: valid for the life of the page, which Core() is not.
  bdk_contents_t handle() const { return handle_; }
  base::WeakPtr<EngineContents> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  // Reports an event to the application's subscription, later.
  void Emit(uint32_t kind,
            const std::string& url,
            const std::string& detail,
            int32_t net_error,
            uint32_t flags);
  // Find in page (ui_find.h).
  PageFind& find() { return *find_; }

  content::WebContents* web_contents() { return web_contents_; }
  HostView& host_view() { return *host_view_; }
  // Creates a page of `profile` loading `url` as the installed app `app_id`
  // (web-apps.md 4.2) and raises its new-window request of kind webApp with
  // `app` (WebAppInfo). Null when the profile cannot host a page.
  static EngineContents* LaunchWebApp(
      const std::shared_ptr<bdkcore::Profile>& profile,
      const std::string& app_id,
      const GURL& url,
      base::DictValue app);
  // The app this page was launched as, or empty.
  const std::string& web_app_id() const { return web_app_id_; }
  // What the page captures changed (ui_screen_capture.h): emits
  // BDK_CONTENTS_EVENT_CAPTURE_CHANGED when its BDK_CAPTURE_* flags did.
  void NoteCaptureChanged();
  // An app's page committed `url`: reports leaving or re-entering the app's
  // scope (BDK_CONTENTS_EVENT_WEB_APP).
  void NoteAppScope(const GURL& url);
  // The page's JavaScript dialogs and file pickers.
  PageDialogs& dialogs() { return *dialogs_; }

  // Every page of the engine, in creation order.
  static const std::vector<EngineContents*>& All();

  // Wraps `new_contents` as a page of `profile` and asks the application where
  // it goes (a new-window request of `kind`). `opener` is the page that asked,
  // or null when none did (an extension's options page opened by a command);
  // `initiator_extension_id` names the extension that asked, or is empty.
  // Returns the new page, or null when the profile cannot host one.
  // The navigation `navigation_id` went to a native route's handler, which
  // showed the page: it ends as handledNatively rather than aborted.
  void NoteHandledNatively(int64_t navigation_id) {
    handled_natively_ = navigation_id;
  }
  // The navigation `navigation_id` was cancelled or redirected by the
  // application's navigation policy: it ends with `detail`
  // (bdkcore::policy_failure_detail) rather than as aborted.
  void NotePolicyEnd(int64_t navigation_id, std::string detail) {
    policy_ended_ = navigation_id;
    policy_detail_ = std::move(detail);
  }
  // A tab Chromium inserted in a host context by itself (a link in an
  // extension popup, which Chromium opens in the popup's Browser): unless a
  // page holds it by now, it becomes a page, and a new-window request asks
  // the application where it goes (T-EXT-12).
  static void AdoptForeignTab(base::WeakPtr<content::WebContents> contents);
  static EngineContents* RequestNewPage(
      const std::shared_ptr<bdkcore::Profile>& profile,
      const std::shared_ptr<bdkcore::Contents>& opener,
      std::unique_ptr<content::WebContents> new_contents,
      const std::string& kind,
      WindowOpenDisposition disposition,
      const GURL& target_url,
      const blink::mojom::WindowFeatures* features,
      bool user_gesture,
      const std::string& initiator_extension_id,
      const base::DictValue* web_app = nullptr);

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
  // Loads the URL of the application's redirect decision, the
  // `policy_redirects`th in a row (NavigationPolicyThrottle).
  void LoadForPolicy(const std::string& url, uint32_t policy_redirects);
  // The count LoadForPolicy left for the navigation it started, once; 0 for
  // any other.
  uint32_t TakePolicyRedirects() { return std::exchange(policy_redirects_, 0); }
  // Replaces the WebContents with one restored from `state`, keeping the
  // handle and the host view.
  void ReplaceWebContents(std::unique_ptr<content::WebContents> web_contents);
  // Replaces the page with an unloaded copy of its history; it loads again
  // when it is shown or focused.
  void Discard();
  void Close();
  // DevTools for this page (T-AUT-05): opens the configured front end in a
  // page of its own and raises a `devtools` request for it (ui-requests.md
  // 5.15); does nothing when one is open. CloseDevTools closes it.
  void OpenDevTools();
  void CloseDevTools();
  bool DevToolsOpen() const { return !!devtools_page_; }
  // The JSON id of the open front end's page, or empty.
  std::string DevToolsFrontendId() const;
  void SetZoom(double zoom);
  double Zoom();
  void SetUserAgent(const std::string& user_agent);
  // Mutes the page, including the WebContents a discard or restore puts in its
  // place.
  void SetMuted(bool muted);
  // Closes this page's DevTools when they are open, and opens them otherwise.
  void ToggleDevTools();
  // Whether DevTools may open: not when [ui.surfaces] devtools is disabled.
  bool CanOpenDevTools();
  // Opens view-source: of the page in a page of its own, through a
  // new-window request of kind "newPage", as Chrome opens a tab.
  void OpenViewSource();
  // Whether the page has focus, as the last focus event said.
  bool focused() const { return focused_; }
  // Between Close() and Chromium's CloseContents().
  bool closing() const { return closing_; }
  void LoadIfDiscarded();
  // Chromium discarded the page's tab itself and put `fresh`, with no
  // renderer, in its place (host_context.cc sees the swap).
  void AdoptDiscardReplacement(content::WebContents* fresh);
  bool auto_discardable() const { return auto_discardable_; }
  void set_auto_discardable(bool value) { auto_discardable_ = value; }

  // content::WebContentsDelegate:
  blink::mojom::DisplayMode GetDisplayMode(
      const content::WebContents* web_contents) override;
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
  // window.moveTo and resizeTo of a page in a popup window.
  void SetContentsBounds(content::WebContents* source,
                         const gfx::Rect& bounds) override;
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
  // Pointer and keyboard lock (ui-requests.md 5.12, T-UIR-15), with
  // Chrome's rules: a pointer lock needs a gesture unless the page unlocked
  // it itself, and is refused just after the person pressed Escape; a
  // keyboard lock only in fullscreen. Each raises the exclusive_access notice
  // (ui_exclusive_access.h); Escape unlocks the pointer, and held, exits a
  // keyboard lock that includes it.
  void RequestPointerLock(content::WebContents* web_contents,
                          bool user_gesture,
                          bool last_unlocked_by_target) override;
  void LostPointerLock() override;
  void RequestKeyboardLock(content::WebContents* web_contents,
                           bool esc_key_locked) override;
  void CancelKeyboardLockRequest(content::WebContents* web_contents) override;
  // A hung renderer is a page_health request (ui_page_health.h).
  void RendererUnresponsive(
      content::WebContents* source,
      content::RenderWidgetHost* render_widget_host,
      base::RepeatingClosure hang_monitor_restarter) override;
  void RendererResponsive(
      content::WebContents* source,
      content::RenderWidgetHost* render_widget_host) override;
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
  // Makes `web_contents` the page's WebContents: its first, as a tab of the
  // parking context, or a replacement in the same tab.
  void Attach(std::unique_ptr<content::WebContents> web_contents);
  // Makes `fresh`, which a tab model already owns, the page's WebContents:
  // the delegate, observers, helpers, host view, prompts and find.
  void TakeContents(content::WebContents* fresh);
  // The host view moved into or out of a window; the page follows later.
  void OnHostViewWindowChanged();
  void ActivateTab();
  // The front end of this page opened or closed: devtools.stateChanged.
  void NotifyDevTools(bool open);
  // Shows the page in a window of BDK's own: the default presenter of
  // `surface` (devtools or new_windows). Closing the window destroys the page.
  void ShowInOwnWindow(const std::string& surface,
                       const std::string& title,
                       int width,
                       int height);
  // Closes the page. In the default presenter's window it belongs to no one,
  // so it is destroyed; a page the application placed is asked to close, as
  // window.close() does.
  void CloseShownPage();
  // Destroys the page after the current task: for a page in the default
  // presenter's window, which no application owns.
  void DestroySoon();
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
  const bdk_contents_t handle_;
  std::unique_ptr<HostView> host_view_;
  // JavaScript dialogs and file pickers; find in page (ui_dialogs.h,
  // ui_find.h).
  std::unique_ptr<PageDialogs> dialogs_;
  uint32_t capture_flags_ = 0;
  // An app's page: the app, and whether its last committed page was in the
  // app's scope.
  std::string web_app_id_;
  bool in_app_scope_ = true;
  std::unique_ptr<PageFind> find_;
  // Owned by its tab model, in a host context.
  raw_ptr<content::WebContents> web_contents_ = nullptr;
  // For a DevTools front-end page: the bindings to the page it inspects.
  std::unique_ptr<DevToolsFrontend> devtools_;
  // The extension items of the context menu raised last, kept until a later
  // menu replaces them so that performing one can run it.
  std::unique_ptr<ExtensionMenuItems> extension_menu_items_;
  // The default presenter's window while it shows the page (a DevTools front
  // end, or a new page no handler placed); running it closes the window.
  base::OnceClosure own_window_;
  // For an inspected page: the page of its open front end.
  base::WeakPtr<EngineContents> devtools_page_;
  // Load and LoadForPolicy, once they have set policy_redirects_.
  void LoadUrl(const std::string& url);
  // The page's visibility follows its host view (T-ADP-11): checked on a later
  // task, after Chromium's own update.
  void ScheduleVisibilityCheck();
  void FollowHostViewVisibility();
  std::string user_agent_;
  // The navigation a native route's handler took (NoteHandledNatively), or 0.
  int64_t handled_natively_ = 0;
  // The navigation the policy ended (NotePolicyEnd), or 0, and its detail.
  int64_t policy_ended_ = 0;
  std::string policy_detail_;
  // LoadForPolicy's count, for TakePolicyRedirects.
  uint32_t policy_redirects_ = 0;
  bool visibility_check_pending_ = false;
  // The page has been reported visible once (OnVisibilityChanged).
  bool shown_once_ = false;
  bool muted_ = false;
  double zoom_ = 1.0;
  bool auto_discardable_ = true;
  bool discarded_ = false;
  bool fullscreen_ = false;
  // Exclusive access: the pointer is locked; the keyboard is, with Escape;
  // when the person last pressed Escape to unlock; Escape held down.
  bool pointer_locked_ = false;
  bool keyboard_locked_with_escape_ = false;
  base::TimeTicks last_escape_;
  base::OneShotTimer escape_held_;
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
