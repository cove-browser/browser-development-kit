// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_UI_REQUESTS_H_
#define BDK_ENGINE_SERVICES_UI_REQUESTS_H_

#include <functional>
#include <memory>
#include <string>

#include "base/values.h"
#include "bdk/core/include/bdk/bdk_base.h"
#include "bdk/core/runtime/core_internal.h"

namespace content {
class WebContents;
}  // namespace content

namespace bdk {

class EngineContents;

// What the prompts of engine/services share: finding the BDK page behind a
// WebContents, raising a UI request for it, and the page's window.

// The BDK page of `contents`, or null when it is not one (a WebContents of
// Chromium's own, a DevTools front end, a page that was already closed).
EngineContents* FindEngineContents(content::WebContents* contents);

using RequestFinished = std::function<void(bdkcore::UiOutcome outcome,
                                           const std::string& completion,
                                           const std::string& arguments)>;

// Raises a UI request of `surface` for `page` with `payload`, and calls
// `on_finish` exactly once, later, with how it ended (ui_router.cc). Returns
// the request, so a caller can withdraw it (bdkcore::withdraw_ui_request), or
// null when the runtime is shutting down (`on_finish` is never called).
std::shared_ptr<bdkcore::Request> RaiseUiRequest(EngineContents& page,
                                                 uint32_t surface,
                                                 const base::DictValue& payload,
                                                 RequestFinished on_finish);

// The native window of the page's host view, or a handle of kind 0.
bdk_native_handle_t WindowOfPage(EngineContents& page);

// Whether `arguments` (the JSON a completion carried) has `key` set to true.
bool ArgumentIsTrue(const std::string& arguments, const char* key);
// A string member of `arguments`, or empty.
std::string ArgumentString(const std::string& arguments, const char* key);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_UI_REQUESTS_H_
