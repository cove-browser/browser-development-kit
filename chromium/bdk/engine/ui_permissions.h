// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_PERMISSIONS_H_
#define BDK_ENGINE_UI_PERMISSIONS_H_

namespace content {
class WebContents;
}  // namespace content

namespace bdk {

class EngineContents;

// Makes the page's permission request manager show its prompts as BDK UI
// requests of the `permissions` surface (ui-requests.md section 5.1) instead of
// Chrome's bubble. The decision goes back to the manager, which stores it in
// the profile's content settings: a persistent profile keeps it, a private one
// forgets it with the profile. Call after a WebContents becomes the page's
// (Chrome's tab helpers, and so the manager, are attached by then).
void InstallPermissionPrompts(EngineContents& page,
                              content::WebContents* contents);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_PERMISSIONS_H_
