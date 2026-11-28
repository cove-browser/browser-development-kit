// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_device_chooser.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "components/permissions/chooser_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace bdk {

namespace {

// One chooser: owns the controller, is its view, and lives until the chooser
// ends (a device chosen, a cancel, or the page's request torn down).
class DeviceChooserSession : public permissions::ChooserController::View {
 public:
  DeviceChooserSession(
      std::string kind,
      EngineContents& page,
      const url::Origin& origin,
      std::unique_ptr<permissions::ChooserController> controller)
      : kind_(std::move(kind)),
        page_(page.GetWeakPtr()),
        origin_(origin),
        controller_(std::move(controller)) {
    controller_->set_view(this);
    ResetIds();
  }

  ~DeviceChooserSession() override {
    if (controller_) {
      controller_->set_view(nullptr);
    }
  }

  base::WeakPtr<DeviceChooserSession> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  void Start() {
    base::DictValue payload = State();
    payload.Set("kind", kind_);
    payload.Set("origin", origin_.Serialize());
    request_ = RaiseUiRequest(*page_, BDK_SURFACE_DEVICE_CHOOSERS, payload,
                              base::BindOnce(&DeviceChooserSession::OnEnded,
                                             GetWeakPtr(), payload.Clone()));
    if (!request_ && !ended_) {
      End(std::nullopt);
    }
  }

  // The closure chrome::ShowDeviceChooserDialog would return ran: the page's
  // request went (its frame navigated, or the page closed).
  void TakeDown() {
    if (ended_) {
      return;
    }
    ended_ = true;
    if (request_) {
      bdkcore::withdraw_ui_request(*request_);
    }
    Destroy();
  }

  // permissions::ChooserController::View:
  void OnOptionsInitialized() override {
    ResetIds();
    Changed();
  }
  void OnOptionAdded(size_t index) override {
    if (index <= ids_.size()) {
      ids_.insert(ids_.begin() + static_cast<std::ptrdiff_t>(index),
                  next_id_++);
    }
    Changed();
  }
  void OnOptionRemoved(size_t index) override {
    if (index < ids_.size()) {
      ids_.erase(ids_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    Changed();
  }
  void OnOptionUpdated(size_t index) override { Changed(); }
  void OnAdapterEnabledChanged(bool enabled) override {
    adapter_enabled_ = enabled;
    if (!enabled) {
      ResetIds();
    }
    Changed();
  }
  void OnRefreshStateChanged(bool refreshing) override {
    scanning_ = refreshing;
    Changed();
  }

 private:
  void ResetIds() {
    ids_.clear();
    for (size_t i = 0; i < controller_->NumOptions(); ++i) {
      ids_.push_back(next_id_++);
    }
  }

  base::ListValue Devices() const {
    base::ListValue devices;
    for (size_t i = 0; i < ids_.size() && i < controller_->NumOptions(); ++i) {
      base::DictValue device;
      device.Set("id", "device" + base::NumberToString(ids_[i]));
      device.Set("name", base::UTF16ToUTF8(controller_->GetOption(i)));
      if (kind_ == "bluetooth") {
        device.Set("paired", controller_->IsPaired(i));
        device.Set("connected", controller_->IsConnected(i));
      }
      devices.Append(std::move(device));
    }
    return devices;
  }

  // The fields of the payload that change while the chooser is open.
  base::DictValue State() const {
    base::DictValue state;
    state.Set("devices", Devices());
    state.Set("adapterEnabled", adapter_enabled_);
    state.Set("scanning", scanning_);
    return state;
  }

  void Changed() {
    if (ended_) {
      return;
    }
    if (sheet_) {
      sheet_->SetDevices(Devices());
    }
    if (request_) {
      if (std::shared_ptr<bdkcore::Runtime> runtime =
              bdkcore::current_runtime()) {
        bdkcore::ui_send_update(runtime, request_, ToJson(State()));
      }
    }
  }

  void OnEnded(base::DictValue payload, const UiResult& result) {
    request_.reset();
    if (ended_) {
      return;
    }
    switch (result.outcome) {
      case bdkcore::UiOutcome::kCompleted: {
        const std::string* device = result.args.FindString("device");
        End(result.completion == "select" && device
                ? std::optional<std::string>(*device)
                : std::nullopt);
        return;
      }
      case bdkcore::UiOutcome::kDefaulted:
        if (!page_) {
          End(std::nullopt);
          return;
        }
        sheet_ = UiPresenters::Get().ShowDeviceChooser(
            &page_->host_view(), payload,
            base::BindOnce(&DeviceChooserSession::End, GetWeakPtr()));
        sheet_->SetDevices(Devices());
        return;
      default:
        // Rejected, dropped, withdrawn or disabled: the page's request
        // rejects.
        End(std::nullopt);
        return;
    }
  }

  // Connects the page to the device `id` names, or cancels when it names
  // none still offered.
  void End(std::optional<std::string> id) {
    if (ended_) {
      return;
    }
    ended_ = true;
    sheet_.reset();
    std::optional<size_t> index;
    for (size_t i = 0; id && i < ids_.size(); ++i) {
      if (*id == "device" + base::NumberToString(ids_[i])) {
        index = i;
      }
    }
    if (index && *index < controller_->NumOptions()) {
      controller_->Select({*index});
    } else {
      controller_->Cancel();
    }
    Destroy();
  }

  // The controller's answer may make Chromium run the closure while it is on
  // the stack; the session goes after it returns.
  void Destroy() {
    weak_factory_.InvalidateWeakPtrs();
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  const std::string kind_;
  base::WeakPtr<EngineContents> page_;
  const url::Origin origin_;
  std::unique_ptr<permissions::ChooserController> controller_;
  std::shared_ptr<bdkcore::Request> request_;
  std::unique_ptr<DeviceChooserSheet> sheet_;
  // The ids of the controller's options, in its order.
  std::vector<int> ids_;
  int next_id_ = 0;
  bool adapter_enabled_ = true;
  bool scanning_ = false;
  bool ended_ = false;
  base::WeakPtrFactory<DeviceChooserSession> weak_factory_{this};
};

}  // namespace

base::OnceClosure ShowDeviceChooser(
    const char* kind,
    content::RenderFrameHost* frame,
    std::unique_ptr<permissions::ChooserController> controller) {
  EngineContents* page =
      frame ? EngineContents::FromWebContents(
                  content::WebContents::FromRenderFrameHost(frame))
            : nullptr;
  if (!page) {
    // Not a page of BDK's (an extension's background page): no chooser.
    controller->Cancel();
    return base::DoNothing();
  }
  auto* session = new DeviceChooserSession(
      kind, *page, frame->GetLastCommittedOrigin(), std::move(controller));
  base::WeakPtr<DeviceChooserSession> weak = session->GetWeakPtr();
  session->Start();
  return base::BindOnce(&DeviceChooserSession::TakeDown, weak);
}

}  // namespace bdk
