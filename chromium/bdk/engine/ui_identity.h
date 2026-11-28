// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_IDENTITY_H_
#define BDK_ENGINE_UI_IDENTITY_H_

#include <memory>

namespace content {
class IdentityRequestDialogController;
class WebContents;
}  // namespace content

namespace bdk {

// FedCM on the engine (ui-requests.md 5.22, T-UIR-14): the dialog controller
// BDK's browser client returns instead of Chrome's views one. Each step of
// Chromium's sign-in is an `identity` request, withdrawn when Chromium moves
// on: chooseAccount, loading, verifying, failure, error and registerIdP. An
// identity provider's own pages (its sign-in page, an error's details) are
// new-window requests of kind identityProvider. Default presenter: a sheet
// with Cancel and one "Continue as" button per account, and a sheet for a
// failure, an error and a registration.
std::unique_ptr<content::IdentityRequestDialogController>
CreateIdentityDialogController(content::WebContents* rp_web_contents);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_IDENTITY_H_
