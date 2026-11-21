// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Screenshots and print-to-PDF of a page (T-SVC-06). The test hooks start
// both (bdk_testing.h); contents.pdf (contents_service.cc) uses CapturePdf with
// its options, and contents.screenshot goes through the DevTools protocol
// instead (page_protocol.h), which can capture the whole page or a region.

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

// What contents.pdf lets the caller choose. Lengths are in inches, as the
// DevTools protocol's Page.printToPDF takes them; an unset value is Chromium's
// default (US Letter, 0.4 inch margins, scale 1).
struct PdfOptions {
  PdfOptions();
  PdfOptions(const PdfOptions&);
  ~PdfOptions();

  std::optional<bool> landscape;
  std::optional<bool> print_background;
  std::optional<double> scale;
  std::optional<double> paper_width;
  std::optional<double> paper_height;
  std::optional<double> margin_top;
  std::optional<double> margin_bottom;
  std::optional<double> margin_left;
  std::optional<double> margin_right;
  // "1-3, 5"; empty prints every page.
  std::string page_ranges;
};

// The whole document as a PDF, with the page's own print styles.
void CapturePdf(content::WebContents* contents, CaptureCallback done);
void CapturePdf(content::WebContents* contents,
                const PdfOptions& options,
                CaptureCallback done);

// Writes bytes to a file the caller named, off the UI thread; `done` runs on
// the UI thread with whether it was written.
void WriteCaptureFile(const base::FilePath& path,
                      std::vector<uint8_t> bytes,
                      base::OnceCallback<void(bool)> done);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_CAPTURE_SERVICE_H_
