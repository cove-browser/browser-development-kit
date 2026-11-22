// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSION_UI_PLATFORM_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSION_UI_PLATFORM_H_

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "bdk/core/include/bdk/bdk_base.h"

namespace content {
class WebContents;
}  // namespace content

namespace bdk {

// The platform half of the extension UI (T-EXT-02): the native view an
// extension popup is shown in and its default panel, the default extensions
// menu and the title-bar button that opens it. Platform-neutral: no AppKit,
// Win32 or toolkit type appears here (architecture.md section 14). The
// implementations for macOS are extension_popup_mac.mm and
// extension_menu_mac.mm.

// A rectangle in window coordinates, origin top-left: x, y, width, height.
using WindowRect = std::array<double, 4>;

// The native view of an extension popup, and the transient panel that shows it
// when the application has no handler for the popup.
class PopupNative {
 public:
  // Makes the view for `host_contents`, the popup's page, and puts the page's
  // own view in it.
  static std::unique_ptr<PopupNative> Create(
      content::WebContents* host_contents);

  virtual ~PopupNative() = default;

  // The view the application embeds (BDK_NATIVE_NSVIEW on macOS).
  virtual bdk_native_handle_t SecureView() = 0;
  // Sizes the view, in device-independent pixels.
  virtual void SetSize(int width, int height) = 0;

  // The default presenter: shows the view in a transient panel that can become
  // key. The panel sits under the anchor (a rectangle of `window`, the native
  // window of the page the popup was opened for) with its trailing edge at the
  // anchor's, or in the top trailing corner of the window's content when there
  // is no anchor; with no window it sits at the top right of the screen.
  // `resigned_key` runs once, when the panel stops being key after it has been.
  virtual void ShowInPanel(bdk_native_handle_t window,
                           std::optional<WindowRect> anchor,
                           base::OnceClosure resigned_key) = 0;
  // Takes the panel down and gives the view back; does nothing when there is
  // none.
  virtual void ClosePanel() = 0;
  virtual bool IsInPanel() const = 0;
};

// One line of the default extensions menu.
struct ExtensionMenuEntry {
  // The extension's identifier, or "manage" for the last line. Empty: a
  // separator.
  std::string id;
  std::string title;
  // The action's badge text, shown after the title.
  std::string badge;
  // The action's icon, a data: URL.
  std::string icon;
  bool enabled = true;
};

// Shows the default extensions menu and calls `chosen` with the chosen line's
// id, or does not call it when the menu is dismissed. The menu opens under
// `anchor_view` (a native view, the title-bar button) when there is one, else
// under `anchor` (a rectangle of `window`) with its trailing edge at the
// anchor's, else in the top trailing corner of the window's content. Returns
// when the menu closes.
void ShowExtensionsMenu(bdk_native_handle_t window,
                        std::optional<WindowRect> anchor,
                        bdk_native_handle_t anchor_view,
                        const std::vector<ExtensionMenuEntry>& entries,
                        base::OnceCallback<void(const std::string& id)> chosen);

// The accessory button in a window's title bar that opens the default
// extensions menu when the application registered no extensions anchor. It
// sits at the trailing edge of the title bar, after the window's own title-bar
// content (extensions.md section 4.7, settled in T-EXT-02).
class TitlebarMenuButton {
 public:
  // `clicked` gets the button's native view, to anchor the menu under. Null
  // when the window cannot take an accessory (it has no title bar).
  static std::unique_ptr<TitlebarMenuButton> Create(
      bdk_native_handle_t window,
      base::RepeatingCallback<void(bdk_native_handle_t button)> clicked);

  virtual ~TitlebarMenuButton() = default;

  // The button is shown only while the window's profile has an extension with
  // an action.
  virtual void SetVisible(bool visible) = 0;
  virtual bool IsVisible() const = 0;
  // Where the button sits in its window, for tests; all zero when it is
  // hidden.
  virtual WindowRect FrameInWindow() const = 0;
};

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSION_UI_PLATFORM_H_
