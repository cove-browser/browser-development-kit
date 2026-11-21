// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/capture_service.h"

#include <utility>
#include <variant>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/printing/print_view_manager.h"
#include "components/printing/browser/print_to_pdf/pdf_print_result.h"
#include "components/printing/browser/print_to_pdf/pdf_print_utils.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace bdk {

namespace {

constexpr base::TimeDelta kCopyTimeout = base::Seconds(10);

}  // namespace

void CaptureScreenshot(content::WebContents* contents, CaptureCallback done) {
  content::RenderWidgetHostView* view = contents->GetRenderWidgetHostView();
  if (!view) {
    std::move(done).Run(std::nullopt, "the page has no view to capture");
    return;
  }
  view->CopyFromSurface(
      gfx::Rect(), gfx::Size(), kCopyTimeout,
      base::BindOnce(
          [](CaptureCallback done,
             const content::CopyFromSurfaceResult& result) {
            if (!result.has_value() || result->bitmap.drawsNothing()) {
              std::move(done).Run(
                  std::nullopt,
                  "Chromium has not rendered the page: is its view visible?");
              return;
            }
            std::optional<std::vector<uint8_t>> png =
                gfx::PNGCodec::EncodeBGRASkBitmap(
                    result->bitmap, /*discard_transparency=*/false);
            if (!png) {
              std::move(done).Run(std::nullopt,
                                  "the image could not be encoded");
              return;
            }
            std::move(done).Run(std::move(png), "");
          },
          std::move(done)));
}

void CapturePdf(content::WebContents* contents, CaptureCallback done) {
  // Created if the page has none; creating it twice does nothing.
  printing::PrintViewManager::CreateForWebContents(contents);
  printing::PrintViewManager* manager =
      printing::PrintViewManager::FromWebContents(contents);
  content::RenderFrameHost* frame = contents->GetPrimaryMainFrame();
  std::variant<printing::mojom::PrintPagesParamsPtr, std::string> params =
      print_to_pdf::GetPrintPagesParams(
          frame->GetLastCommittedURL(), std::nullopt, std::nullopt,
          std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
          std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
          std::nullopt, std::nullopt, std::nullopt);
  if (std::holds_alternative<std::string>(params)) {
    std::move(done).Run(std::nullopt, std::get<std::string>(params));
    return;
  }
  manager->PrintToPdf(
      frame, "",
      std::move(std::get<printing::mojom::PrintPagesParamsPtr>(params)),
      base::BindOnce(
          [](CaptureCallback done, print_to_pdf::PdfPrintResult result,
             scoped_refptr<base::RefCountedMemory> data) {
            if (result != print_to_pdf::PdfPrintResult::kPrintSuccess ||
                !data) {
              std::move(done).Run(std::nullopt,
                                  print_to_pdf::PdfPrintResultToString(result));
              return;
            }
            std::move(done).Run(
                std::vector<uint8_t>(data->begin(), data->end()), "");
          },
          std::move(done)));
}

void WriteCaptureFile(const base::FilePath& path,
                      std::vector<uint8_t> bytes,
                      base::OnceCallback<void(bool)> done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath path, std::vector<uint8_t> bytes) {
            return base::WriteFile(path, bytes);
          },
          path, std::move(bytes)),
      std::move(done));
}

}  // namespace bdk
