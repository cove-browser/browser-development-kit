// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_NOTIFICATIONS_H_
#define BDK_ENGINE_UI_NOTIFICATIONS_H_

class Profile;

namespace bdk {

// Makes the profile's notification display service show web and extension
// notifications as UI requests of the `notifications` surface (ui-requests.md
// section 5.13). Clicks and closes go back through Chrome's own notification
// handlers, so the page sees onclick and onclose. Notifications of Chrome's
// own features (sharing, announcements and the like) are not shown. Call once
// per profile as it opens, before its display service is used.
void InstallNotificationDisplay(Profile* profile);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_NOTIFICATIONS_H_
