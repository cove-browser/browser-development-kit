// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/panel_testing.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/bdk_browser_window.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/side_panel_host.h"
#include "bdk/engine/host_context.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/window_registry.h"
#include "chrome/browser/extensions/window_controller.h"
#include "chrome/browser/extensions/window_controller_list.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/service_worker_context.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_util.h"
#include "extensions/browser/process_manager.h"
#include "extensions/browser/service_worker/worker_id.h"
#include "extensions/browser/unpacked_installer.h"
#include "extensions/common/extension.h"

namespace bdk {

namespace {

// The last `extensions.load`: done, the extension id (empty on failure) and
// the error.
struct LoadState {
  bool done = false;
  std::string id;
  std::string error;
};
LoadState& LoadRecord() {
  static base::NoDestructor<LoadState> state;
  return *state;
}

// The last `extensions.eval`.
struct EvalState {
  bool done = false;
  std::string value = "null";
  std::string error;
};
EvalState& EvalRecord() {
  static base::NoDestructor<EvalState> state;
  return *state;
}

Profile* ProfileOfPage(const base::DictValue& args) {
  const std::string* handle = args.FindString("contents");
  uint64_t value = 0;
  if (!handle || !base::StringToUint64(*handle, &value)) {
    return nullptr;
  }
  std::shared_ptr<bdkcore::Contents> c =
      bdkcore::contents_of(static_cast<bdk_contents_t>(value));
  return c && EngineContents::From(*c) ? ProfileOf(*c) : nullptr;
}

// The SidePanelController of the context of the window `args` names that holds
// its active page.
SidePanelController* ControllerOfWindow(const base::DictValue& args) {
  const std::string* handle = args.FindString("window");
  uint64_t value = 0;
  if (!handle || !base::StringToUint64(*handle, &value)) {
    return nullptr;
  }
  std::shared_ptr<bdkcore::Window> window =
      bdkcore::window_of(static_cast<bdk_window_t>(value));
  HostContext* context =
      window ? WindowRegistry::Get().ActiveContext(window.get()) : nullptr;
  BdkBrowserWindow* browser_window =
      context ? context->browser_window() : nullptr;
  return browser_window ? browser_window->side_panel_controller() : nullptr;
}

base::ListValue FrameList(const std::array<double, 4>& frame) {
  base::ListValue list;
  for (double v : frame) {
    list.Append(v);
  }
  return list;
}

}  // namespace

std::optional<bool> RunExtensionPanelCommand(const std::string& name,
                                             const base::DictValue& args,
                                             base::DictValue& result) {
  if (name == "extensions.load") {
    // {"contents", "path"}: loads the unpacked extension in the profile of the
    // page, without the install prompt (the fixtures of the engine tests).
    // Poll extensions.loaded.
    Profile* profile = ProfileOfPage(args);
    const std::string* path = args.FindString("path");
    if (!profile || !path) {
      return false;
    }
    LoadRecord() = LoadState();
    scoped_refptr<extensions::UnpackedInstaller> installer =
        extensions::UnpackedInstaller::Create(profile);
    installer->set_completion_callback(
        base::BindOnce([](const extensions::Extension* extension,
                          const base::FilePath&, const std::u16string& error) {
          LoadRecord().done = true;
          LoadRecord().id = extension ? extension->id() : std::string();
          LoadRecord().error = base::UTF16ToUTF8(error);
        }));
    installer->Load(base::FilePath::FromUTF8Unsafe(*path));
    return true;
  }
  if (name == "extensions.loaded") {
    result.Set("done", LoadRecord().done);
    result.Set("id", LoadRecord().id);
    result.Set("error", LoadRecord().error);
    return true;
  }
  if (name == "extensions.eval") {
    // {"contents", "extension", "script"}: runs the script in the extension's
    // service worker. The result is what the script evaluates to, so a script
    // that needs a promise stores its answer in a global and a second call
    // reads it. Poll extensions.eval.result.
    Profile* profile = ProfileOfPage(args);
    const std::string* id = args.FindString("extension");
    const std::string* script = args.FindString("script");
    if (!profile || !id || !script) {
      return false;
    }
    EvalRecord() = EvalState();
    std::vector<extensions::WorkerId> workers =
        extensions::ProcessManager::Get(profile)->GetServiceWorkersForExtension(
            *id);
    if (workers.size() != 1u) {
      EvalRecord().done = true;
      EvalRecord().error = "workers: " + base::NumberToString(workers.size());
      return true;
    }
    extensions::util::GetServiceWorkerContextForExtensionId(*id, profile)
        ->ExecuteScriptForTest(
            *script, workers[0].version_id,
            base::BindOnce([](base::Value value,
                              const std::optional<std::string>& error) {
              EvalRecord().done = true;
              EvalRecord().value =
                  value.is_string() ? value.GetString()
                                    : base::WriteJson(value).value_or("null");
              EvalRecord().error = error.value_or(std::string());
            }));
    return true;
  }
  if (name == "extensions.eval.result") {
    result.Set("done", EvalRecord().done);
    result.Set("value", EvalRecord().value);
    result.Set("error", EvalRecord().error);
    return true;
  }
  if (name == "window.extension_windows") {
    // {"contents"}: the windows the extension windows API lists for the page's
    // profile: [{"id", "type"}].
    Profile* profile = ProfileOfPage(args);
    if (!profile) {
      return false;
    }
    base::ListValue windows;
    for (extensions::WindowController* controller :
         *extensions::WindowControllerList::GetInstance()) {
      if (controller->profile() == profile) {
        base::DictValue entry;
        entry.Set("id", controller->GetWindowId());
        entry.Set("type", controller->GetWindowTypeText());
        windows.Append(std::move(entry));
      }
    }
    result.Set("windows", std::move(windows));
    return true;
  }
  if (name == "sidepanel.state") {
    // {"window"}: what the window's open extension side panel is.
    SidePanelController* controller = ControllerOfWindow(args);
    if (!controller) {
      return false;
    }
    const SidePanelState state = controller->State();
    result.Set("open", state.open);
    result.Set("extension", state.extension_id);
    result.Set("url", state.url);
    result.Set("pageScoped", state.page_scoped);
    result.Set("visible", state.visible);
    result.Set("defaultPresenter", state.default_presenter);
    result.Set("frame", FrameList(state.frame));
    result.Set("requestOpen", state.request_open);
    result.Set("opened", state.opened_dispatched);
    return true;
  }
  if (name == "sidepanel.drag") {
    // {"window", "delta"}: drags the default panel's divider.
    SidePanelController* controller = ControllerOfWindow(args);
    return controller && args.FindInt("delta") &&
           controller->DragDividerForTesting(*args.FindInt("delta"));
  }
  if (name == "prompt.anchored") {
    // The frame of the prompt attached to a window anchor, in the window's
    // content view.
    std::optional<std::array<double, 4>> frame = AnchoredPromptFrame();
    result.Set("attached", frame.has_value());
    result.Set("frame", FrameList(frame.value_or(std::array<double, 4>())));
    return true;
  }
  if (name == "prompt.anchored.answer") {
    return args.FindInt("index") &&
           AnswerAnchoredPromptForTesting(*args.FindInt("index"));
  }
  return std::nullopt;
}

}  // namespace bdk
