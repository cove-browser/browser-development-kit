// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_AUTOMATION_AUTOMATION_HOST_LAUNCHER_H_
#define BDK_ENGINE_AUTOMATION_AUTOMATION_HOST_LAUNCHER_H_

#include <map>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "bdk/engine/automation/automation_host.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace bdkcore {
struct Runtime;
class TaskPort;
}  // namespace bdkcore

namespace bdk {

// The browser process side of the automation host (T-AUT-01, T-AUT-02): the
// backend's StartTask and EndTask over a sandboxed utility process that runs
// V8. Owned by the engine backend; everything runs on the UI thread.
//
// The host is launched when the first task starts, dropped five seconds after
// the last task ends, and launched again by the next task. A crash finishes
// every running task with HostGone; the next task relaunches.
//
// A task starts like this: the script is read from
// `<bundle path>/Contents/Resources/<entry.bundle>` off the UI thread and
// checked against `entry.bundle_digest`; meanwhile the core's first call of
// `TaskPort::to_script` brings the `start` frame (the core sends it after
// StartTask returns). When both are there, AutomationHost::StartTask runs.
// Later frames go straight into the task's pipe, which queues them until the
// host binds it.
class AutomationHostLauncher {
 public:
  // `bundle_path`: the application bundle (bdk_start_config_t::bundle_path).
  explicit AutomationHostLauncher(base::FilePath bundle_path);
  ~AutomationHostLauncher();

  AutomationHostLauncher(const AutomationHostLauncher&) = delete;
  AutomationHostLauncher& operator=(const AutomationHostLauncher&) = delete;

  // Backend::StartTask. Sets `port->to_script` before it returns. A task that
  // cannot start finishes through TaskPort::ScriptFailed (asynchronously).
  void StartTask(bdkcore::Runtime& runtime,
                 const std::shared_ptr<bdkcore::TaskPort>& port);

  // Backend::EndTask: closes the task's pipe, which disposes its context.
  void EndTask(bdkcore::TaskPort& port);

 private:
  class Task;

  void LaunchHostIfNeeded();
  void OnHostDisconnected();
  void OnFrameFromCore(const std::string& task_id, const std::string& frame);
  // The bundle's text, or why it cannot be used.
  void OnBundleRead(const std::string& task_id,
                    base::expected<std::string, std::string> source);
  void StartOnHostIfReady(Task& task);
  // Ends the task and reports `message` through ScriptFailed. Never called
  // while the core is inside StartTask (see FailLater).
  void FailTask(const std::string& task_id, const std::string& message);
  // The same for a script the host stopped at its CPU budget: BudgetExceeded.
  void StopTaskOverBudget(const std::string& task_id,
                          const std::string& message);
  // Forgets the task and returns its port, or null for a task it no longer
  // has.
  std::shared_ptr<bdkcore::TaskPort> TakeTask(const std::string& task_id);
  void FailLater(std::shared_ptr<bdkcore::TaskPort> port,
                 const std::string& message);
  void StopHostIfIdle();

  const base::FilePath bundle_path_;
  mojo::Remote<mojom::AutomationHost> host_;
  std::map<std::string, std::unique_ptr<Task>> tasks_;
  base::OneShotTimer idle_timer_;
  base::WeakPtrFactory<AutomationHostLauncher> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_AUTOMATION_AUTOMATION_HOST_LAUNCHER_H_
