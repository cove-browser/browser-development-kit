// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_PAGE_HEALTH_H_
#define BDK_ENGINE_UI_PAGE_HEALTH_H_

#include "base/functional/callback_forward.h"

namespace content {
class RenderWidgetHost;
class WebContents;
}  // namespace content

namespace bdk {

class EngineContents;

// The page_health surface on Chromium (ui-requests.md 5.19, T-UIR-15): what
// Chromium hands to views, as requests of BDK's own.
//
// - An unresponsive page: Chromium's hang monitor asks TabDialogs to show its
//   hung-renderer dialog (BdkTabDialogs forwards it here). It becomes one
//   request per hang, withdrawn when the renderer answers again; "terminate"
//   ends the renderer, "wait" (or a denial, or no answer) restarts the hang
//   monitor. The default presenter is a sheet offering both.
// - Blocked popups: each popup Chromium's popup blocker blocks raises a notice
//   listing every popup blocked since the last notice was answered (a new one
//   withdraws the last); a grant opens one of them, which then asks where it
//   goes (new_windows), or allows the site's popups in the profile. A
//   navigation of the page withdraws it. The default presenter is a sheet.

// Called by BdkTabDialogs.
void ShowUnresponsivePage(content::WebContents* contents,
                          content::RenderWidgetHost* render_widget_host,
                          base::RepeatingClosure hang_monitor_restarter);
void HideUnresponsivePage(content::WebContents* contents);
bool IsShowingUnresponsivePage(content::WebContents* contents);

// Watches the page's popup blocker; called when a page takes a WebContents.
void InstallPageHealth(EngineContents& page, content::WebContents* contents);
// Withdraws what is open and stops watching, while the tab is still whole;
// called before a page's WebContents goes.
void UninstallPageHealth(content::WebContents* contents);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_PAGE_HEALTH_H_
