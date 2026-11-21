// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/automation/automation_host_launcher.h"

#include <optional>
#include <string_view>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "bdk/core/runtime/automation_internal.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/json_util.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/service_process_host.h"
#include "crypto/hash.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace bdk {

namespace {

constexpr base::TimeDelta kIdleTimeout = base::Seconds(5);
constexpr char kDigestPrefix[] = "sha256:";

// Reads the bundle and checks its digest. Runs on a thread pool thread (it
// blocks). `relative` comes from the resolved configuration: the application's
// own file, but it is still refused when it leaves the bundle's Resources
// directory, so that a bad configuration cannot make the host run another
// file even if its digest were forged to match.
base::expected<std::string, std::string> ReadBundle(
    base::FilePath resources,
    std::string relative,
    std::string expected_digest) {
  base::FilePath path(relative);
  if (path.IsAbsolute() || path.ReferencesParent()) {
    return base::unexpected(
        "the script's bundle path must stay inside Contents/Resources: " +
        relative);
  }
  path = resources.Append(path);
  std::string source;
  if (!base::ReadFileToString(path, &source)) {
    return base::unexpected("the script's bundle is missing: " + path.value() +
                            "; run `bdk build automation`");
  }
  const std::string actual = base::StrCat(
      {kDigestPrefix,
       base::HexEncodeLower(crypto::hash::Sha256(std::string_view(source)))});
  if (actual != expected_digest) {
    return base::unexpected(
        "the script's bundle does not match its digest (expected " +
        expected_digest + ", found " + actual +
        "); run `bdk build automation`");
  }
  if (!base::IsStringUTF8(source)) {
    return base::unexpected("the script's bundle is not valid UTF-8");
  }
  return source;
}

}  // namespace

// One task of the launcher: what the browser keeps while the host runs it. It
// is also the task's client (the script's side of the conversation).
class AutomationHostLauncher::Task : public mojom::AutomationTaskClient {
 public:
  Task(base::WeakPtr<AutomationHostLauncher> launcher,
       std::shared_ptr<bdkcore::TaskPort> port)
      : client_(this), launcher_(std::move(launcher)), port_(std::move(port)) {}

  ~Task() override = default;

  // mojom::AutomationTaskClient:
  void Send(const std::string& frame) override {
    // The port may finish the task (a `done` frame ends it, and the core then
    // calls EndTask, which destroys this); nothing touches members after.
    port_->FromScript(frame);
  }

  void Failed(const std::string& message) override {
    if (launcher_) {
      launcher_->FailTask(port_->id(), message);
    }
  }

  const std::shared_ptr<bdkcore::TaskPort>& port() const { return port_; }

  // Pipes: the host gets `host_task` and the client with StartTask; Deliver
  // goes through `task`, which queues until the host binds its end.
  mojo::Remote<mojom::AutomationTask> task;
  mojo::PendingReceiver<mojom::AutomationTask> host_task;
  mojo::Receiver<mojom::AutomationTaskClient> client_;

  std::string start_frame;
  bool have_start_frame = false;
  std::optional<std::string> source;
  bool started = false;

 private:
  base::WeakPtr<AutomationHostLauncher> launcher_;
  std::shared_ptr<bdkcore::TaskPort> port_;
};

AutomationHostLauncher::AutomationHostLauncher(base::FilePath bundle_path)
    : bundle_path_(std::move(bundle_path)) {}

AutomationHostLauncher::~AutomationHostLauncher() = default;

void AutomationHostLauncher::StartTask(
    bdkcore::Runtime& runtime,
    const std::shared_ptr<bdkcore::TaskPort>& port) {
  const std::string id = port->id();
  auto task = std::make_unique<Task>(weak_factory_.GetWeakPtr(), port);

  // Set first: the core calls it once StartTask returns, even when this task
  // cannot start (it then finds no task and drops the frame).
  port->to_script = [weak = weak_factory_.GetWeakPtr(),
                     id](const std::string& frame) {
    if (weak) {
      weak->OnFrameFromCore(id, frame);
    }
  };

  std::optional<base::DictValue> entry = ParseJsonDict(port->entry_json());
  const std::string* bundle = entry ? entry->FindString("bundle") : nullptr;
  if (!bundle || bundle->empty()) {
    FailLater(port, "script \"" + port->script() +
                        "\" has no bundle; run `bdk build automation`");
    return;
  }
  const std::string* digest = entry->FindString("bundle_digest");
  if (!digest || !digest->starts_with(kDigestPrefix)) {
    FailLater(port, "script \"" + port->script() +
                        "\" has no bundle_digest (sha256:<hex>); run `bdk "
                        "build automation`");
    return;
  }

  idle_timer_.Stop();
  LaunchHostIfNeeded();
  task->host_task = task->task.BindNewPipeAndPassReceiver();
  tasks_[id] = std::move(task);

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&ReadBundle,
                     bundle_path_.Append("Contents").Append("Resources"),
                     *bundle, *digest),
      base::BindOnce(&AutomationHostLauncher::OnBundleRead,
                     weak_factory_.GetWeakPtr(), id));
}

void AutomationHostLauncher::EndTask(bdkcore::TaskPort& port) {
  // Closing the task's pipe (the Task's destructor) is what ends it on the
  // host.
  tasks_.erase(port.id());
  StopHostIfIdle();
}

void AutomationHostLauncher::LaunchHostIfNeeded() {
  if (host_.is_bound()) {
    return;
  }
  content::ServiceProcessHost::Launch(
      host_.BindNewPipeAndPassReceiver(),
      content::ServiceProcessHost::Options()
          .WithDisplayName(std::string("BDK Automation"))
          .Pass());
  host_.set_disconnect_handler(base::BindOnce(
      &AutomationHostLauncher::OnHostDisconnected, base::Unretained(this)));
}

void AutomationHostLauncher::OnHostDisconnected() {
  host_.reset();
  // Move the tasks out first: HostGone makes the core call EndTask, which must
  // find nothing to erase while this loop runs.
  std::map<std::string, std::unique_ptr<Task>> tasks = std::move(tasks_);
  tasks_.clear();
  for (auto& [id, task] : tasks) {
    std::shared_ptr<bdkcore::TaskPort> port = task->port();
    task.reset();
    port->HostGone("the automation host process exited");
  }
}

void AutomationHostLauncher::OnFrameFromCore(const std::string& task_id,
                                             const std::string& frame) {
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) {
    return;
  }
  Task& task = *it->second;
  if (!base::IsStringUTF8(frame)) {
    // A Mojo string must be UTF-8; a frame that is not would break the pipe
    // and take every task of the host with it.
    FailTask(task_id, "a frame for the script is not valid UTF-8");
    return;
  }
  if (!task.have_start_frame) {
    // The first frame the core sends is `start`.
    task.start_frame = frame;
    task.have_start_frame = true;
    StartOnHostIfReady(task);
    return;
  }
  task.task->Deliver(frame);
}

void AutomationHostLauncher::OnBundleRead(
    const std::string& task_id,
    base::expected<std::string, std::string> source) {
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) {
    return;  // Ended while the file was read.
  }
  if (!source.has_value()) {
    FailTask(task_id, source.error());
    return;
  }
  it->second->source = std::move(source.value());
  StartOnHostIfReady(*it->second);
}

void AutomationHostLauncher::StartOnHostIfReady(Task& task) {
  if (task.started || !task.have_start_frame || !task.source.has_value() ||
      !host_.is_bound()) {
    return;
  }
  task.started = true;
  host_->StartTask(task.port()->id(), *task.source, task.start_frame,
                   std::move(task.host_task),
                   task.client_.BindNewPipeAndPassRemote());
  task.source.reset();
  task.start_frame.clear();
}

void AutomationHostLauncher::FailTask(const std::string& task_id,
                                      const std::string& message) {
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) {
    return;
  }
  // Erase first: ScriptFailed may call EndTask for this very task.
  std::shared_ptr<bdkcore::TaskPort> port = it->second->port();
  tasks_.erase(it);
  port->ScriptFailed(message);
  StopHostIfIdle();
}

void AutomationHostLauncher::FailLater(std::shared_ptr<bdkcore::TaskPort> port,
                                       const std::string& message) {
  // The core is inside StartTask: it finishes the task after this returns.
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce([](std::shared_ptr<bdkcore::TaskPort> port,
                        std::string message) { port->ScriptFailed(message); },
                     std::move(port), message));
}

void AutomationHostLauncher::StopHostIfIdle() {
  if (!tasks_.empty() || !host_.is_bound()) {
    return;
  }
  idle_timer_.Start(FROM_HERE, kIdleTimeout,
                    base::BindOnce(
                        [](AutomationHostLauncher* self) {
                          if (self->tasks_.empty()) {
                            self->host_.reset();
                          }
                        },
                        base::Unretained(this)));
}

}  // namespace bdk
