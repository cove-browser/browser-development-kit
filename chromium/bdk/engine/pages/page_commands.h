// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_PAGES_PAGE_COMMANDS_H_
#define BDK_ENGINE_PAGES_PAGE_COMMANDS_H_

#include <cstdint>

namespace bdk {

class EngineContents;

// Page commands (T-SWF-06, BDK_PAGE_COMMAND_*), as Chrome's own command
// controller enables them for a tab: history moves when there is somewhere to
// go, edit commands whenever there is a page, save and view source only for
// a page Chromium can save, zoom within Chromium's preset steps, fullscreen
// only to leave HTML fullscreen.
bool CanPerformPageCommand(EngineContents& page, uint32_t command);

// Performs a command CanPerformPageCommand allowed, except find, find next,
// find previous and print, which the backend sends through the same entry
// points as bdk_contents_find_* and bdk_contents_print. View source and
// DevTools open a page of their own through a new-window request.
void PerformPageCommand(EngineContents& page, uint32_t command);

// BDK_ZOOM_SCOPE_PAGE: this page only (a temporary level). BDK_ZOOM_SCOPE_SITE:
// Chromium's per-host level in the profile, which every page of the site
// follows; the page's own temporary level is cleared so it follows too.
void SetPageZoom(EngineContents& page, double factor, uint32_t scope);

// Closes the picture-in-picture window if this page opened it.
void ExitPageFromPictureInPicture(EngineContents& page);

}  // namespace bdk

#endif  // BDK_ENGINE_PAGES_PAGE_COMMANDS_H_
