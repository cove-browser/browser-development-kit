// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_SCREEN_CAPTURE_H_
#define BDK_ENGINE_UI_SCREEN_CAPTURE_H_

#include <cstdint>

namespace content {
class WebContents;
}  // namespace content

namespace bdk {

// Screen capture (ui-requests.md 5.16, T-UIR-13): getDisplayMedia and
// chrome.desktopCapture.chooseDesktopMedia. Chrome's access handler, source
// lists and streams are used as they are; BDK supplies the two views pieces
// through patch bdk-let-an-embedder-supply-the-screen-capture-ui:
//
// - The picker: a screen_capture request of the page with {origin,
//   requestsAudio, sources: [{id, kind (screen, window, page), name,
//   thumbnail?}]} and the same {sources} as an update whenever the lists
//   change. "share" with {source: id, audio?} and the person's gesture shares
//   it; anything else refuses (NotAllowedError). The default presenter is a
//   sheet of BDK's.
// - The notification while a page shares: BDK's "sharing" bar in the page's
//   window, with Stop, when the application has no screen_capture handler; an
//   application with one shows its own, following BDK_CAPTURE_DISPLAY.

// Installs both factories, once, as Chromium starts.
void InstallScreenCaptureUi();

// The page's BDK_CAPTURE_* flags.
uint32_t CaptureFlags(content::WebContents* contents);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_SCREEN_CAPTURE_H_
