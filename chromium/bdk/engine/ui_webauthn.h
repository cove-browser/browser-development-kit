// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_WEBAUTHN_H_
#define BDK_ENGINE_UI_WEBAUTHN_H_

namespace bdk {

// Passkeys and security keys (ui-requests.md 5.18, T-UIR-09).
//
// The request itself is Chrome's: ChromeAuthenticatorRequestDelegate and its
// AuthenticatorRequestDialogModel decide the steps, the mechanisms and the
// accounts. Chrome shows the model in a tab-modal views dialog, which needs a
// views widget to parent to and ends the process on a BDK page; a patch lets
// the engine supply the dialog instead. BDK's dialog raises one `webauthn`
// request per step the model shows (choosing a mechanism, touching a security
// key, entering its PIN, choosing an account, connecting a phone, a
// confirmation or an error), with Chromium's own wording for that step, and
// withdraws it when the model moves on. The application's completion goes
// back to the model; with no handler the default presenter is a native sheet.
// A rejected, dropped or disabled request cancels the ceremony, so the page's
// promise rejects with NotAllowedError. A page that is not a BDK page gets no
// dialog and its request is cancelled the same way.
//
// Installs the factory the patch adds, and reports Bluetooth as unsupported
// when the application has no Bluetooth metadata in its Info.plist (macOS ends
// a process that calls Bluetooth without it). Called once, from
// InstallPromptHooks.
void InstallWebAuthnDialogHook();

}  // namespace bdk

#endif  // BDK_ENGINE_UI_WEBAUTHN_H_
