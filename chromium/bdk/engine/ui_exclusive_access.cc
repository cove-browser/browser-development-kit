// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_exclusive_access.h"

#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_bubble_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "url/origin.h"

namespace bdk {

void NoteExclusiveAccess(EngineContents& page,
                         ExclusiveAccessKind kind,
                         bool escape_held) {
  content::WebContents* contents = page.web_contents();
  if (!contents) {
    return;
  }
  const std::string origin =
      contents->GetPrimaryMainFrame()->GetLastCommittedOrigin().Serialize();
  const ExclusiveAccessBubbleType type =
      kind == ExclusiveAccessKind::kPointerLock
          ? EXCLUSIVE_ACCESS_BUBBLE_TYPE_POINTERLOCK_EXIT_INSTRUCTION
          : (escape_held
                 ? EXCLUSIVE_ACCESS_BUBBLE_TYPE_KEYBOARD_LOCK_EXIT_INSTRUCTION
                 : EXCLUSIVE_ACCESS_BUBBLE_TYPE_FULLSCREEN_EXIT_INSTRUCTION);
  base::DictValue payload;
  payload.Set("kind", kind == ExclusiveAccessKind::kPointerLock
                          ? "pointerLock"
                          : "keyboardLock");
  payload.Set("origin", origin);
  payload.Set(
      "exitInstruction",
      base::UTF16ToUTF8(exclusive_access_bubble::GetInstructionTextForType(
          type, u"Esc", base::UTF8ToUTF16(origin),
          /*has_download=*/false, /*notify_overridden=*/false)));
  base::WeakPtr<EngineContents> weak = page.GetWeakPtr();
  RaiseUiRequest(
      page, BDK_SURFACE_EXCLUSIVE_ACCESS, payload,
      base::BindOnce(
          [](base::WeakPtr<EngineContents> page, base::DictValue payload,
             const UiResult& result) {
            // The instruction must be shown: a notice that is not
            // acknowledged is shown by BDK (the core turns a rejection or a
            // drop into the default).
            if (result.outcome != bdkcore::UiOutcome::kDefaulted || !page) {
              return;
            }
            UiPresenters::Get()
                .ShowExclusiveAccessNotice(&page->host_view(), payload)
                .Reset();
          },
          weak, payload.Clone()));
}

}  // namespace bdk
