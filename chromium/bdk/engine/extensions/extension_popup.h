// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSION_POPUP_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSION_POPUP_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/values.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/extensions/extension_ui_platform.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/extensions/extension_view.h"
#include "chrome/browser/ui/extensions/extension_popup_types.h"
#include "content/public/browser/web_contents_observer.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "ui/gfx/geometry/size.h"

namespace extensions {
class ExtensionViewHost;
}  // namespace extensions

namespace bdk {

class EngineContents;

// Why a popup was opened (the `trigger` of the request).
enum class PopupTrigger { kUserClick, kOpenPopupApi, kCommand };

// An extension's popup (extensions.md section 4.2, T-EXT-02): the page of an
// `ExtensionViewHost` shown in a native view of BDK's, which Chromium sizes
// through the pluggable `ExtensionView`. The popup is a request on the
// `extension_popups` surface that stays open while the view is on screen:
//
// * The request is raised once the page has loaded, with the secure view and
//   the size the page asked for. With a handler the application places the
//   view and reports it (`{"placed":true}`); with none, or `useDefault`, a
//   key-capable transient panel shows it, anchored at the window's extensions
//   anchor.
// * Size changes reach the application as {"preferredSize":{...}} updates,
//   within Chromium's popup minimum and maximum.
// * It closes, and the request is withdrawn, on blur, on `window.close()`,
//   when the page it was opened for navigates, is hidden or goes, and when
//   the extension unloads. A popup being inspected with DevTools stays open on
//   blur. The application closes it with `accept` {"action":"close"} or
//   `deny`.
//
// Owned by the extensions container of the page's window
// (extensions_container.h); it asks the container to destroy it when it closes.
class ExtensionPopup : public extensions::ExtensionView,
                       public content::WebContentsObserver,
                       public extensions::ExtensionRegistryObserver {
 public:
  using ClosedCallback = base::OnceCallback<void(ExtensionPopup*)>;

  // `opener` is the page the popup is for. `shown` runs once with the host
  // when the popup is on screen, or with null when it closed first.
  ExtensionPopup(std::unique_ptr<extensions::ExtensionViewHost> host,
                 base::WeakPtr<EngineContents> opener,
                 PopupTrigger trigger,
                 ShowPopupCallback shown,
                 ClosedCallback closed);
  ~ExtensionPopup() override;

  // Starts loading the popup's page.
  void Start();

  // Closes the popup for `reason` (a short name for tests and logs): withdraws
  // the request, takes the panel down and asks the owner to destroy it. Does
  // nothing when it is closed already.
  void Close(const char* reason);

  bool is_closed() const { return closed_; }
  bool is_shown() const { return shown_; }
  const std::string& extension_id() const { return extension_id_; }
  // The page the popup was opened for, or null once it is gone.
  EngineContents* opener() const;

  // The popup lost focus (the panel stopped being key, or the page lost
  // focus); closes it unless it is being inspected.
  void OnBlur();

  // extensions::ExtensionView:
  gfx::NativeView GetNativeView() override;
  void ResizeDueToAutoResize(content::WebContents* web_contents,
                             const gfx::Size& new_size) override;
  void RenderFrameCreated(content::RenderFrameHost* render_frame_host) override;
  bool HandleKeyboardEvent(content::WebContents* source,
                           const input::NativeWebKeyboardEvent& event) override;
  void OnLoaded() override;

  // content::WebContentsObserver (the popup's page):
  void OnWebContentsFocused(
      content::RenderWidgetHost* render_widget_host) override;
  void OnWebContentsLostFocus(
      content::RenderWidgetHost* render_widget_host) override;

  // extensions::ExtensionRegistryObserver:
  void OnExtensionUnloaded(content::BrowserContext* browser_context,
                           const extensions::Extension* extension,
                           extensions::UnloadedExtensionReason reason) override;

 private:
  class OpenerObserver;

  // Raises the request once the page has loaded and its size is known (or has
  // not come soon).
  void RaiseRequest();
  void OnRequestEnded(const UiResult& result);
  // The application's update of the open request ({"placed":true}).
  void OnApplicationUpdate(const std::string& update_json);
  // The default presenter: the transient panel, or the script of a test.
  void ShowDefault();
  void OnScriptedAnswer(const base::DictValue& answer);
  // The view is on screen: focuses the page and runs the shown callback.
  void MarkShown(bool focus);
  bool IsInspected() const;
  std::string SizeJson() const;

  const std::string extension_id_;
  const PopupTrigger trigger_;
  base::WeakPtr<EngineContents> opener_;
  ShowPopupCallback shown_callback_;
  ClosedCallback closed_callback_;
  std::unique_ptr<PopupNative> native_;
  std::unique_ptr<OpenerObserver> opener_observer_;
  base::ScopedObservation<extensions::ExtensionRegistry,
                          extensions::ExtensionRegistryObserver>
      registry_observation_{this};
  std::shared_ptr<bdkcore::Request> request_;
  // What the request said, for the default presenter.
  base::DictValue payload_;
  Dismisser scripted_dismiss_;
  gfx::Size size_;
  bool size_known_ = false;
  bool loaded_ = false;
  bool requested_ = false;
  bool shown_ = false;
  // The page has had focus: a later loss of it is a blur.
  bool focused_once_ = false;
  bool closed_ = false;
  // Declared last: destroyed first, while the popup is whole, because the host
  // calls back into its view as it goes.
  std::unique_ptr<extensions::ExtensionViewHost> host_;
  base::WeakPtrFactory<ExtensionPopup> weak_factory_{this};
};

// What the popups did, for the test hook (bdk_testing_command "extui.popup"):
// counts since the process started and why the last one closed.
struct PopupRecord {
  int opened = 0;
  int shown = 0;
  int closed = 0;
  std::string last_close_reason;
  // The size the last popup reported, in the same shape as the request's.
  std::string last_size;
};
PopupRecord PopupRecordForTesting();

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSION_POPUP_H_
