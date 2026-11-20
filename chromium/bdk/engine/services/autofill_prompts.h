// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_AUTOFILL_PROMPTS_H_
#define BDK_ENGINE_SERVICES_AUTOFILL_PROMPTS_H_

#include <memory>

class BrowserWindowInterface;

namespace autofill {
class AutofillBubbleHandler;
}  // namespace autofill

namespace bdk {

// The autofill prompts of T-UIR-04 (ui-requests.md 5.8).
//
// Save and update address, and save card: Chromium asks the window for its
// AutofillBubbleHandler (the address bubbles through
// BrowserWindow::GetAutofillBubbleHandler(), the card bubbles through the
// Browser's user data). BDK's handler raises each as an `autofill` request;
// the default presenter is a native sheet. Every other bubble the handler is
// asked for (offers, IBAN, virtual cards, mandatory re-authentication, ...) is
// server-driven or needs the toolbar, and is not shown.
//
// Makes the handler of the window of `browser` and registers it in the
// Browser's user data host, so both routes find it. The caller owns it and
// destroys it before the Browser.
std::unique_ptr<autofill::AutofillBubbleHandler> CreateAutofillBubbleHandler(
    BrowserWindowInterface* browser);

// The suggestion popup. Chromium builds it through a factory (a patch lets an
// embedder set one). BDK's: with a handler for `autofill_suggestions` the
// suggestions are a request of that surface; with none, Chromium's own popup,
// which is not given a request. A disabled surface offers nothing, so nothing
// is filled.
void InstallAutofillPopupHook();

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_AUTOFILL_PROMPTS_H_
