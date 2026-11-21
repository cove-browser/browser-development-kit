// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_WINDOW_REGISTRY_H_
#define BDK_ENGINE_WINDOW_REGISTRY_H_

#include <array>
#include <optional>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_base.h"

namespace bdkcore {
struct Window;
}  // namespace bdkcore

namespace bdk {

class HostContext;
class HostView;

// What the engine knows about the windows the application registered
// (bdk_window.h, T-SWF-08) beyond their host contexts (host_context.h): which
// pages arrived in which window and in what order, which context of a window
// was active last, where the application wants default presenters to attach,
// and the requests the engine raises for a window. Platform-neutral.
// HostContexts owns the contexts and EngineContents moves the pages; both tell
// this class, and the backend's attachment queries (Backend::WindowForContents,
// WindowContents, WindowExtensionId) read it.
class WindowRegistry {
 public:
  static WindowRegistry& Get();

  // The page `contents` is a tab of a context of `window` now; null for a
  // parking context. A page that entered a window and was moved to parking
  // because the window went is attached to no window.
  void NotePage(bdk_contents_t contents, const bdkcore::Window* window);
  void ForgetPage(bdk_contents_t contents);
  // `context` became the context of its window that the window's active page
  // is in.
  void NoteActive(const bdkcore::Window* window, const HostContext* context);
  void ForgetContext(const HostContext* context);
  // The window went: nothing is remembered of it.
  void ForgetWindow(const bdkcore::Window* window);

  // The window the page is attached to or was last attached to, or 0.
  bdk_window_t WindowOfPage(bdk_contents_t contents) const;
  // The pages attached or last attached to `window`, in the order they
  // arrived.
  std::vector<bdk_contents_t> PagesOf(const bdkcore::Window* window) const;
  // The context whose active tab the window's active page is: the one that
  // became active last, or the newest context when none did. Null when the
  // window has none.
  HostContext* ActiveContext(const bdkcore::Window* window) const;

 private:
  friend class base::NoDestructor<WindowRegistry>;
  WindowRegistry();
  ~WindowRegistry();

  // Windows by handle: the core frees a window at shutdown without
  // unregistering it, so nothing here may point at one.
  struct Arrival {
    bdk_contents_t contents;
    bdk_window_t window;
  };
  // Pages in the order they arrived in their window (a move appends).
  std::vector<Arrival> arrivals_;
  struct Active {
    bdk_window_t window;
    raw_ptr<const HostContext> context;
  };
  std::vector<Active> active_;
};

// Where the application wants a default presenter for `role`
// (BDK_ANCHOR_ADDRESS_BAR, _EXTENSIONS, _DOWNLOADS) to attach in `window`:
// x, y, width, height in window coordinates, origin top left. Nothing when
// the window has no anchor provider or it answers "no anchor".
std::optional<std::array<double, 4>> WindowAnchor(const bdkcore::Window* window,
                                                  uint32_t role);
// The same for the registered window `view` is in; nothing when it is in
// none, or `view` is null.
std::optional<std::array<double, 4>> ViewWindowAnchor(HostView* view,
                                                      uint32_t role);

// Raises a request for the registered window with handle `window`
// (bdk_window.h: setFrame, activate, minimize, zoom, enterFullscreen,
// exitFullscreen, close). Later, `apply` runs with true when the engine
// should apply it to the native window (the application has no handler, or
// answered "use default") and with false when the application handled or
// rejected it. It does not run when the window or the runtime goes first.
void RaiseWindowRequest(bdk_window_t window,
                        base::DictValue request,
                        base::OnceCallback<void(bool apply)> done);

// A window request body of `kind`. The engine cannot tell which extension
// called windows.update, so no "initiator" is set.
base::DictValue WindowRequest(const char* kind);

}  // namespace bdk

#endif  // BDK_ENGINE_WINDOW_REGISTRY_H_
