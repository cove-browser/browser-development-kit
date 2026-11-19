// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <AppKit/AppKit.h>

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "bdk/engine/services/native_presenter.h"
#include "bdk/engine/services/presenter_answers.h"

namespace bdk {

namespace {

NSWindow* WindowOf(bdk_native_handle_t handle) {
  if (handle.kind != BDK_NATIVE_NSWINDOW || !handle.pointer) {
    return nil;
  }
  return (__bridge NSWindow*)handle.pointer;
}

template <typename T>
void PostReply(base::OnceCallback<void(T)> done, T value) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(done), std::move(value)));
}

}  // namespace

void PresentAlert(const std::string& surface,
                  Alert alert,
                  bdk_native_handle_t window,
                  base::OnceCallback<void(int)> done) {
  if (std::optional<std::string> answer = TakePresenterAnswer(surface)) {
    int index = -1;
    for (size_t i = 0; i < alert.buttons.size(); ++i) {
      if (alert.buttons[i].key == *answer) {
        index = static_cast<int>(i);
      }
    }
    PostReply(std::move(done), index);
    return;
  }
  NSWindow* ns_window = WindowOf(window);
  if (!ns_window || !ns_window.visible) {
    PostReply(std::move(done), -1);
    return;
  }
  NSAlert* ns_alert = [[NSAlert alloc] init];
  ns_alert.messageText = base::SysUTF8ToNSString(alert.title);
  ns_alert.informativeText = base::SysUTF8ToNSString(alert.message);
  for (const AlertButton& button : alert.buttons) {
    [ns_alert addButtonWithTitle:base::SysUTF8ToNSString(button.title)];
  }
  __block base::OnceCallback<void(int)> reply = std::move(done);
  const size_t count = alert.buttons.size();
  [ns_alert
      beginSheetModalForWindow:ns_window
             completionHandler:^(NSModalResponse response) {
               const NSInteger index = response - NSAlertFirstButtonReturn;
               std::move(reply).Run(index >= 0 &&
                                            static_cast<size_t>(index) < count
                                        ? static_cast<int>(index)
                                        : -1);
             }];
}

void PresentSavePanel(
    const std::string& surface,
    const std::string& suggested_name,
    bdk_native_handle_t window,
    base::OnceCallback<void(std::optional<std::string>)> done) {
  if (std::optional<std::string> answer = TakePresenterAnswer(surface)) {
    PostReply(std::move(done), *answer == "cancel"
                                   ? std::optional<std::string>()
                                   : std::optional<std::string>(*answer));
    return;
  }
  NSWindow* ns_window = WindowOf(window);
  if (!ns_window || !ns_window.visible) {
    PostReply(std::move(done), std::optional<std::string>());
    return;
  }
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.nameFieldStringValue = base::SysUTF8ToNSString(suggested_name);
  __block base::OnceCallback<void(std::optional<std::string>)> reply =
      std::move(done);
  [panel
      beginSheetModalForWindow:ns_window
             completionHandler:^(NSModalResponse response) {
               if (response == NSModalResponseOK && panel.URL.path) {
                 std::move(reply).Run(base::SysNSStringToUTF8(panel.URL.path));
               } else {
                 std::move(reply).Run(std::nullopt);
               }
             }];
}

}  // namespace bdk
