// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_request.h"

#include <array>
#include <optional>
#include <utility>

#include "base/containers/span.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/ui_presenters.h"

namespace bdk {

bool SurfaceAvailable(const std::shared_ptr<bdkcore::Contents>& contents,
                      uint32_t surface) {
  std::shared_ptr<bdkcore::Runtime> rt =
      contents ? contents->rt.lock() : nullptr;
  return rt && bdkcore::ui_surface_available(*rt, surface);
}

bool SurfaceHasHandler(const std::shared_ptr<bdkcore::Contents>& contents,
                       uint32_t surface) {
  std::shared_ptr<bdkcore::Runtime> rt =
      contents ? contents->rt.lock() : nullptr;
  return rt && rt->ui_handlers.count(surface) > 0;
}

std::shared_ptr<bdkcore::Request> RaiseUiRequest(
    const std::shared_ptr<bdkcore::Contents>& contents,
    uint32_t surface,
    const base::DictValue& payload,
    UiFinish finish,
    HostView* anchor_view,
    std::optional<std::array<double, 2>> point_in_view) {
  std::shared_ptr<bdkcore::Runtime> rt =
      contents ? contents->rt.lock() : nullptr;
  if (!rt || rt->shutting) {
    return nullptr;
  }
  auto request = std::make_shared<bdkcore::Request>();
  request->surface = surface;
  request->contents = contents->handle;
  request->payload = ToJson(payload);
  if (anchor_view) {
    const std::array<double, 4> frame = anchor_view->FrameInWindow();
    base::span<double, 4> anchor(request->anchor);
    anchor[0] = frame[0];
    anchor[1] = frame[1];
    anchor[2] = frame[2];
    anchor[3] = 0;
    if (point_in_view) {
      anchor[0] += (*point_in_view)[0];
      anchor[1] += (*point_in_view)[1];
      anchor[2] = 0;
    }
  }
  // With no finish the request is a notice (a fullscreen exit, say): it has
  // no on_finish, so the router delivers it at once instead of queueing it
  // behind a request the handler still holds.
  if (!finish) {
    return bdkcore::raise_ui_request(rt, request);
  }
  // The lambda lives in the request, and the router calls it while the
  // request is alive, so it may read the request's gesture.
  const bdkcore::Request* raw = request.get();
  auto shared_finish = std::make_shared<UiFinish>(std::move(finish));
  request->on_finish = [raw, shared_finish](bdkcore::UiOutcome outcome,
                                            const std::string& completion,
                                            const std::string& args) {
    UiResult result;
    result.outcome = outcome;
    result.completion = completion;
    result.gesture_ok = UiPresenters::Get().IsRecentGesture(raw->gesture);
    // The router checked only the kind of the gesture handle; the engine
    // checks that it is the operating system's recent event. A grant without
    // one is no grant on any surface: it reaches the handler as a rejection,
    // which takes the path that grants nothing.
    if (outcome == bdkcore::UiOutcome::kCompleted && !result.gesture_ok &&
        bdkcore::completion_grants(raw->surface, completion)) {
      result.outcome = bdkcore::UiOutcome::kRejected;
      result.completion.clear();
      if (*shared_finish) {
        std::move(*shared_finish).Run(result);
      }
      return;
    }
    if (std::optional<base::DictValue> parsed = ParseJsonDict(args)) {
      result.args = std::move(*parsed);
    }
    if (*shared_finish) {
      std::move(*shared_finish).Run(result);
    }
  };
  return bdkcore::raise_ui_request(rt, request);
}

std::shared_ptr<bdkcore::Request> RaiseUiRequest(EngineContents& page,
                                                 uint32_t surface,
                                                 const base::DictValue& payload,
                                                 UiFinish finish) {
  return RaiseUiRequest(page.Core(), surface, payload, std::move(finish),
                        &page.host_view());
}

}  // namespace bdk
