// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Screenshots and print-to-PDF of a page (T-SVC-06). The engine side of what
// services.md section 5.15 calls contents.screenshot and contents.pdf: those
// commands are defined with the automation work (M7), so nothing in the
// interface reaches these functions yet; the test hooks do (bdk_testing.h).

#ifndef BDK_ENGINE_SERVICES_CAPTURE_SERVICE_H_
#define BDK_ENGINE_SERVICES_CAPTURE_SERVICE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"

namespace content {
class WebContents;
}

namespace bdk {

// The bytes of the capture, or nullopt with the reason it failed. Runs on the
// UI thread.
using CaptureCallback =
    base::OnceCallback<void(std::optional<std::vector<uint8_t>> bytes,
                            std::string error)>;

// The visible area of the page as a PNG image.
void CaptureScreenshot(content::WebContents* contents, CaptureCallback done);

// The whole document as a PDF, with the page's own print styles.
void CapturePdf(content::WebContents* contents, CaptureCallback done);

// Writes bytes to a file the caller named, off the UI thread; `done` runs on
// the UI thread with whether it was written.
void WriteCaptureFile(const base::FilePath& path,
                      std::vector<uint8_t> bytes,
                      base::OnceCallback<void(bool)> done);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_CAPTURE_SERVICE_H_
