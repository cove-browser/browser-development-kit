// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/device_chooser_testing.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/ui_device_chooser.h"
#include "components/permissions/chooser_controller.h"
#include "content/public/browser/web_contents.h"

namespace bdk {

namespace {

// How the last fake chooser ended.
struct Outcome {
  bool ended = false;
  std::optional<std::string> chosen;
};

Outcome& LastOutcome() {
  static base::NoDestructor<Outcome> outcome;
  return *outcome;
}

class FakeController;

base::WeakPtr<FakeController>& Current() {
  static base::NoDestructor<base::WeakPtr<FakeController>> current;
  return *current;
}

class FakeController : public permissions::ChooserController {
 public:
  explicit FakeController(std::vector<std::string> devices)
      : permissions::ChooserController(u"Fake devices"),
        devices_(std::move(devices)) {}
  ~FakeController() override { LastOutcome().ended = true; }

  base::WeakPtr<FakeController> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  void Add(const std::string& name) {
    devices_.push_back(name);
    if (view()) {
      view()->OnOptionAdded(devices_.size() - 1);
    }
  }
  bool Remove(const std::string& name) {
    auto it = std::ranges::find(devices_, name);
    if (it == devices_.end()) {
      return false;
    }
    const size_t index = static_cast<size_t>(it - devices_.begin());
    devices_.erase(it);
    if (view()) {
      view()->OnOptionRemoved(index);
    }
    return true;
  }

  // permissions::ChooserController:
  std::u16string GetNoOptionsText() const override { return u"No devices"; }
  std::u16string GetOkButtonLabel() const override { return u"Connect"; }
  std::pair<std::u16string, std::u16string> GetThrobberLabelAndTooltip()
      const override {
    return {u"Scanning", u"Scanning"};
  }
  size_t NumOptions() const override { return devices_.size(); }
  std::u16string GetOption(size_t index) const override {
    return base::UTF8ToUTF16(devices_[index]);
  }
  void Select(const std::vector<size_t>& indices) override {
    LastOutcome().chosen = devices_[indices.front()];
  }
  void Cancel() override {}
  void Close() override {}
  void OpenHelpCenterUrl() const override {}

 private:
  std::vector<std::string> devices_;
  base::WeakPtrFactory<FakeController> weak_factory_{this};
};

}  // namespace

bool RunChooserTestingCommand(const std::string& name,
                              EngineContents* page,
                              const base::DictValue& args,
                              base::DictValue& result) {
  if (name == "chooser.open") {
    const std::string* kind = args.FindString("kind");
    const base::ListValue* names = args.FindList("devices");
    if (!page || !kind || !names) {
      return false;
    }
    std::vector<std::string> devices;
    for (const base::Value& device : *names) {
      if (device.is_string()) {
        devices.push_back(device.GetString());
      }
    }
    LastOutcome() = Outcome();
    auto controller = std::make_unique<FakeController>(std::move(devices));
    Current() = controller->GetWeakPtr();
    // The closure is dropped: nothing of the page's takes this chooser down.
    ShowDeviceChooser(kind->c_str(),
                      page->web_contents()->GetPrimaryMainFrame(),
                      std::move(controller));
    return true;
  }
  if (name == "chooser.add" || name == "chooser.remove") {
    const std::string* device = args.FindString("name");
    FakeController* controller = Current().get();
    if (!device || !controller) {
      return false;
    }
    if (name == "chooser.add") {
      controller->Add(*device);
      return true;
    }
    return controller->Remove(*device);
  }
  if (name == "chooser.result") {
    result.Set("ended", LastOutcome().ended);
    if (LastOutcome().chosen) {
      result.Set("chosen", *LastOutcome().chosen);
    }
    return true;
  }
  return false;
}

}  // namespace bdk
