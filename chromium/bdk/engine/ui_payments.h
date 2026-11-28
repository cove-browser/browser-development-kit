// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_PAYMENTS_H_
#define BDK_ENGINE_UI_PAYMENTS_H_

namespace bdk {

// Payment Request on the engine (ui-requests.md 5.22, T-UIR-14). A page's
// PaymentRequest.show() is a `payments` request naming the total, the line
// items and the payment apps that can pay; a grant with the person's gesture
// pays with the chosen app, through Chromium's own payment flow. Chrome's
// sheet is a views dialog: patch
// bdk-let-an-embedder-present-the-payment-request-sheet asks BDK first. A
// payment handler that opens its own page (PaymentRequestEvent.openWindow())
// gets a new-window request of kind paymentHandler, closed when the payment
// ends. Default presenter: a sheet with the total, Cancel and one "Pay with"
// button per app.

// Sets the dialog factory, once, as Chromium starts.
void InstallPaymentHooks();

}  // namespace bdk

#endif  // BDK_ENGINE_UI_PAYMENTS_H_
