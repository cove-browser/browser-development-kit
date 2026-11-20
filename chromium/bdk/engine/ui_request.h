// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_REQUEST_H_
#define BDK_ENGINE_UI_REQUEST_H_

#include <array>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/values.h"
#include "bdk/core/runtime/core_internal.h"

namespace bdk {

class EngineContents;
class HostView;

// How a UI request the engine raised ended, as the engine's handlers read it:
// the router's outcome, the completion and its arguments (an empty dictionary
// when it had none), and whether a granting completion carried the gesture
// the operating system delivered (a recent mouse-up or key-down of this
// application's window, ui-requests.md section 3).
struct UiResult {
  bdkcore::UiOutcome outcome = bdkcore::UiOutcome::kNone;
  std::string completion;
  base::DictValue args;
  bool gesture_ok = false;
};

using UiFinish = base::OnceCallback<void(const UiResult&)>;

// The engine's one way to raise a request: raises one of `surface` for
// `contents` through the core's router, with `payload` (the fields of
// ui-requests.md section 5 for the surface) and calls `finish` (which may be
// null) once, later, when it ends. The request is anchored at the top of
// `anchor_view`'s frame when given, or at `point_in_view` (view coordinates)
// when that is given too. Returns null, and never calls `finish`, when the
// runtime is gone or shutting down. The caller may withdraw the request it
// gets (bdkcore::withdraw_ui_request).
std::shared_ptr<bdkcore::Request> RaiseUiRequest(
    const std::shared_ptr<bdkcore::Contents>& contents,
    uint32_t surface,
    const base::DictValue& payload,
    UiFinish finish,
    HostView* anchor_view = nullptr,
    std::optional<std::array<double, 2>> point_in_view = std::nullopt);

// The same for a page, anchored at the top of its view.
std::shared_ptr<bdkcore::Request> RaiseUiRequest(EngineContents& page,
                                                 uint32_t surface,
                                                 const base::DictValue& payload,
                                                 UiFinish finish);

// Whether the configuration leaves the surface enabled: when it does not, the
// web API behind it reports itself unavailable instead of raising a request.
bool SurfaceAvailable(const std::shared_ptr<bdkcore::Contents>& contents,
                      uint32_t surface);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_REQUEST_H_
