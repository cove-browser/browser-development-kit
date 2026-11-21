// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/pages/page_commands.h"

#include <memory>

#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/core/runtime/routes.h"
#include "bdk/engine/engine_contents.h"
#include "chrome/browser/picture_in_picture/picture_in_picture_window_manager.h"
#include "components/find_in_page/find_tab_helper.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "net/base/url_util.h"
#include "third_party/blink/public/common/page/page_zoom.h"

namespace bdk {
namespace {

constexpr double kEpsilon = 1e-9;

// The next of Chromium's preset zoom factors above (direction 1) or below
// (-1) the current one; the current one at either end.
double ZoomStep(double factor, int direction) {
  double found = factor;
  for (double step : blink::kPresetBrowserZoomFactors) {
    if (direction > 0 && step > factor + kEpsilon) {
      return step;
    }
    if (direction < 0 && step < factor - kEpsilon) {
      found = step;
    }
  }
  return found;
}

bool HasPage(content::WebContents* contents) {
  return contents->GetController().GetLastCommittedEntry() != nullptr;
}

// An internal page is neither saved nor shown as source, as in the stub: both
// would show its chrome:// URL, which the application never sees.
bool IsInternalPage(content::WebContents* contents) {
  content::NavigationEntry* entry =
      contents->GetController().GetLastCommittedEntry();
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  return entry && rt && bdkcore::is_internal_page(*rt, entry->GetURL().spec());
}

bool FindActive(content::WebContents* contents) {
  auto* helper = find_in_page::FindTabHelper::FromWebContents(contents);
  return helper && !helper->find_text().empty();
}

}  // namespace

bool CanPerformPageCommand(EngineContents& page, uint32_t command) {
  content::WebContents* contents = page.web_contents();
  content::NavigationController& controller = contents->GetController();
  const double zoom = page.Zoom();
  switch (command) {
    case BDK_PAGE_COMMAND_BACK:
      return controller.CanGoBack();
    case BDK_PAGE_COMMAND_FORWARD:
      return controller.CanGoForward();
    case BDK_PAGE_COMMAND_STOP:
      return contents->IsLoading();
    case BDK_PAGE_COMMAND_FIND_NEXT:
    case BDK_PAGE_COMMAND_FIND_PREVIOUS:
      return FindActive(contents);
    case BDK_PAGE_COMMAND_ZOOM_IN:
      return ZoomStep(zoom, 1) > zoom + kEpsilon;
    case BDK_PAGE_COMMAND_ZOOM_OUT:
      return ZoomStep(zoom, -1) < zoom - kEpsilon;
    case BDK_PAGE_COMMAND_ZOOM_RESET:
      return zoom < 1.0 - kEpsilon || zoom > 1.0 + kEpsilon;
    case BDK_PAGE_COMMAND_SAVE_PAGE_AS:
      return HasPage(contents) && !IsInternalPage(contents) &&
             contents->IsSavable();
    case BDK_PAGE_COMMAND_VIEW_SOURCE:
      return !IsInternalPage(contents) && controller.CanViewSource();
    case BDK_PAGE_COMMAND_TOGGLE_DEV_TOOLS:
      return HasPage(contents) && page.CanOpenDevTools();
    case BDK_PAGE_COMMAND_TOGGLE_FULLSCREEN:
      return contents->IsFullscreen();
    default:
      return HasPage(contents);
  }
}

void PerformPageCommand(EngineContents& page, uint32_t command) {
  content::WebContents* contents = page.web_contents();
  content::NavigationController& controller = contents->GetController();
  switch (command) {
    case BDK_PAGE_COMMAND_BACK:
      controller.GoBack();
      return;
    case BDK_PAGE_COMMAND_FORWARD:
      controller.GoForward();
      return;
    case BDK_PAGE_COMMAND_RELOAD:
      controller.Reload(content::ReloadType::NORMAL, /*check_for_repost=*/true);
      return;
    case BDK_PAGE_COMMAND_RELOAD_BYPASSING_CACHE:
      controller.Reload(content::ReloadType::BYPASSING_CACHE,
                        /*check_for_repost=*/true);
      return;
    case BDK_PAGE_COMMAND_STOP:
      contents->Stop();
      return;
    case BDK_PAGE_COMMAND_CUT:
      contents->Cut();
      return;
    case BDK_PAGE_COMMAND_COPY:
      contents->Copy();
      return;
    case BDK_PAGE_COMMAND_PASTE:
      contents->Paste();
      return;
    case BDK_PAGE_COMMAND_PASTE_AND_MATCH_STYLE:
      contents->PasteAndMatchStyle();
      return;
    case BDK_PAGE_COMMAND_UNDO:
      contents->Undo();
      return;
    case BDK_PAGE_COMMAND_REDO:
      contents->Redo();
      return;
    case BDK_PAGE_COMMAND_SELECT_ALL:
      contents->SelectAll();
      return;
    case BDK_PAGE_COMMAND_DELETE:
      contents->Delete();
      return;
    case BDK_PAGE_COMMAND_ZOOM_IN:
      page.SetZoom(ZoomStep(page.Zoom(), 1));
      return;
    case BDK_PAGE_COMMAND_ZOOM_OUT:
      page.SetZoom(ZoomStep(page.Zoom(), -1));
      return;
    case BDK_PAGE_COMMAND_ZOOM_RESET:
      page.SetZoom(1.0);
      return;
    case BDK_PAGE_COMMAND_SAVE_PAGE_AS:
      // Chromium's save-package flow asks where to save with the system's
      // save panel.
      contents->OnSavePage();
      return;
    case BDK_PAGE_COMMAND_VIEW_SOURCE:
      page.OpenViewSource();
      return;
    case BDK_PAGE_COMMAND_TOGGLE_MUTE:
      page.SetMuted(!contents->IsAudioMuted());
      return;
    case BDK_PAGE_COMMAND_TOGGLE_DEV_TOOLS:
      page.OpenDevTools();
      return;
    case BDK_PAGE_COMMAND_TOGGLE_FULLSCREEN:
      contents->ExitFullscreen(/*will_cause_resize=*/true);
      return;
    default:
      return;  // find, find next and previous, print: the backend's
  }
}

void SetPageZoom(EngineContents& page, double factor, uint32_t scope) {
  if (scope != BDK_ZOOM_SCOPE_SITE) {
    page.SetZoom(factor);
    return;
  }
  content::WebContents* contents = page.web_contents();
  content::HostZoomMap* map = content::HostZoomMap::GetForWebContents(contents);
  map->ClearTemporaryZoomLevel(contents->GetPrimaryMainFrame()->GetGlobalId());
  map->SetZoomLevelForHost(
      net::GetHostOrSpecFromURL(contents->GetLastCommittedURL()),
      blink::ZoomFactorToZoomLevel(factor));
}

void ExitPageFromPictureInPicture(EngineContents& page) {
  PictureInPictureWindowManager* manager =
      PictureInPictureWindowManager::GetInstance();
  if (manager->GetWebContents() == page.web_contents()) {
    manager->ExitPictureInPicture();
  }
}

}  // namespace bdk
