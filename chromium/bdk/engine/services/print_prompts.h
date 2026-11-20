// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_PRINT_PROMPTS_H_
#define BDK_ENGINE_SERVICES_PRINT_PROMPTS_H_

namespace content {
class WebContents;
}  // namespace content

namespace bdk {

// Printing (ui-requests.md 5.9, T-UIR-04). window.print() and the print
// command raise a request of the `printing` surface. Chromium's print preview
// is a constrained web dialog of a Browser window, which BDK's windowless
// Browsers cannot show (feasibility.md section 1), so `previewContents` is not
// offered and the default presenter is the system's print panel, through
// Chromium's basic print path. The application can present its own print UI,
// complete the request with the system dialog, or cancel it.
//
// The hook BDK's print patch calls: true when the request was taken (the
// page is a BDK page), so that Chromium creates no preview dialog.
bool OnPrintRequested(content::WebContents* initiator, bool is_user_initiated);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_PRINT_PROMPTS_H_
