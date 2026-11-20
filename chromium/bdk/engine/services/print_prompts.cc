// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/print_prompts.h"

#include <string>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/services/presenter_answers.h"
#include "bdk/engine/services/ui_requests.h"
#include "chrome/browser/printing/print_view_manager.h"
#include "content/public/browser/web_contents.h"

namespace bdk {

namespace {

// Opens the system's print panel for the page.
void PrintWithSystemDialog(base::WeakPtr<content::WebContents> weak) {
  content::WebContents* contents = weak.get();
  if (!contents) {
    return;
  }
  if (std::optional<std::string> answer = TakePresenterAnswer("printing")) {
    return;  // a test answered for the panel: none is opened
  }
  if (auto* manager = printing::PrintViewManager::FromWebContents(contents)) {
    manager->BasicPrint(contents->GetPrimaryMainFrame());
  }
}

}  // namespace

bool OnPrintRequested(content::WebContents* initiator, bool is_user_initiated) {
  EngineContents* page = FindEngineContents(initiator);
  if (!page) {
    return false;
  }
  base::DictValue payload;
  payload.Set("isUserInitiated", is_user_initiated);
  base::WeakPtr<content::WebContents> weak = initiator->GetWeakPtr();
  RaiseUiRequest(
      *page, BDK_SURFACE_PRINTING, payload,
      [weak](bdkcore::UiOutcome outcome, const std::string& completion,
             const std::string& arguments) {
        if (outcome == bdkcore::UiOutcome::kDefaulted) {
          PrintWithSystemDialog(weak);
        } else if (outcome == bdkcore::UiOutcome::kCompleted &&
                   completion == "accept" &&
                   ArgumentString(arguments, "action") == "systemDialog") {
          PrintWithSystemDialog(weak);
        }
        // Cancelled, rejected, dropped, disabled, and "savePDF" (the print
        // to PDF command is the way to a file): nothing is printed.
      });
  return true;
}

}  // namespace bdk
