// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/automation/automation_host_service.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/thread_annotations.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "gin/array_buffer.h"
#include "gin/public/isolate_holder.h"
#include "gin/v8_initializer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "tools/v8_context_snapshot/buildflags.h"
#include "v8/include/v8.h"

namespace bdk {

namespace {

// The old generation's limit. V8 has no gentler failure at the limit than
// aborting the process (no near-heap-limit callback is installed): the browser
// sees the host disappear and finishes every running task with
// automation.hostGone.
constexpr size_t kHeapLimitBytes = size_t{512} * 1024 * 1024;

// The largest frame either side may send (automation_internal.h,
// kMaxFrameBytes; the core is not linked here, so the number is repeated).
constexpr size_t kMaxFrameBytes = size_t{32} * 1024 * 1024;

// A script that sets more timers than this gets a RangeError: timers live
// outside V8's heap, so the heap limit does not bound them.
constexpr size_t kMaxTimers = 10000;

// The globals a script's environment has beyond ECMAScript. Natives
// (`__bdkHost.send`, `setTimeout`, `clearTimeout`) are installed by C++ before
// this runs; this builds `console`, `queueMicrotask`, `AbortController` and
// `AbortSignal` on them, because bare V8 has none of these and the
// @bdk/automation library uses the last two for `task.signal` and call
// cancellation. It runs as a classic script in every context, before the task's
// module is evaluated.
constexpr char kPrelude[] = R"JS(
(function (global) {
  'use strict';
  var send = global.__bdkHost.send;

  function show(value) {
    if (typeof value === 'string') return value;
    if (value instanceof Error) return value.stack || String(value);
    try {
      var json = JSON.stringify(value);
      if (json !== undefined) return json;
    } catch (e) {}
    try { return String(value); } catch (e) {
      return Object.prototype.toString.call(value);
    }
  }

  function logger(level) {
    return function () {
      var parts = [];
      for (var i = 0; i < arguments.length; i++) parts.push(show(arguments[i]));
      try {
        send(JSON.stringify({ type: 'log', level: level, message: parts.join(' ') }));
      } catch (e) {}
    };
  }
  global.console = {
    log: logger('info'), info: logger('info'), warn: logger('warn'),
    error: logger('error'), debug: logger('debug')
  };

  global.queueMicrotask = function queueMicrotask(callback) {
    if (typeof callback !== 'function') {
      throw new TypeError('queueMicrotask expects a function');
    }
    Promise.resolve().then(callback);
  };

  var token = {};
  var fire, create;
  class AbortSignal {
    #aborted = false;
    #reason = undefined;
    #listeners = [];
    onabort = null;
    constructor(key) {
      if (key !== token) throw new TypeError('Illegal constructor');
    }
    get aborted() { return this.#aborted; }
    get reason() { return this.#reason; }
    throwIfAborted() { if (this.#aborted) throw this.#reason; }
    addEventListener(type, listener, options) {
      if (type !== 'abort' || !listener) return;
      for (var i = 0; i < this.#listeners.length; i++) {
        if (this.#listeners[i].listener === listener) return;
      }
      this.#listeners.push({ listener: listener, once: !!(options && options.once) });
    }
    removeEventListener(type, listener) {
      if (type !== 'abort') return;
      this.#listeners = this.#listeners.filter(function (l) { return l.listener !== listener; });
    }
    static abort(reason) {
      var signal = create();
      fire(signal, reason);
      return signal;
    }
    static {
      create = function () { return new AbortSignal(token); };
      fire = function (signal, reason) {
        if (signal.#aborted) return;
        if (reason === undefined) {
          reason = new Error('This operation was aborted');
          reason.name = 'AbortError';
        }
        signal.#aborted = true;
        signal.#reason = reason;
        var event = { type: 'abort', target: signal };
        var listeners = signal.#listeners.slice();
        signal.#listeners = listeners.filter(function (l) { return !l.once; });
        var handlers = [];
        if (typeof signal.onabort === 'function') handlers.push(signal.onabort);
        listeners.forEach(function (l) { handlers.push(l.listener); });
        handlers.forEach(function (handler) {
          try {
            if (typeof handler === 'function') handler.call(signal, event);
            else handler.handleEvent(event);
          } catch (e) { global.console.error(e); }
        });
      };
    }
  }
  class AbortController {
    #signal = create();
    get signal() { return this.#signal; }
    abort(reason) { fire(this.#signal, reason); }
  }
  global.AbortSignal = AbortSignal;
  global.AbortController = AbortController;
})(globalThis);
)JS";

// The browser's view of a task, kept for the callbacks V8 makes: every native
// function V8 calls carries the task's serial number, never a pointer, so a
// callback that outlives its task (a promise reaction queued in the isolate's
// shared microtask queue) finds nothing and returns.
class ScriptTask;
std::map<uint32_t, base::WeakPtr<ScriptTask>>& TaskRegistry() {
  static base::NoDestructor<std::map<uint32_t, base::WeakPtr<ScriptTask>>>
      registry;
  return *registry;
}

v8::Local<v8::String> NewString(v8::Isolate* isolate, std::string_view text) {
  // Only fails above V8's string length limit, far over a frame.
  return v8::String::NewFromUtf8(isolate, text.data(),
                                 v8::NewStringType::kNormal,
                                 base::checked_cast<int>(text.size()))
      .ToLocalChecked();
}

// Invalid UTF-16 (lone surrogates) is replaced, so the result is always valid
// UTF-8, which Mojo strings require.
std::string ToStdString(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value text(isolate, value);
  return *text ? std::string(text.as_view()) : std::string();
}

std::string ToJsonText(const base::DictValue& value) {
  return base::WriteJson(value).value_or("{}");
}

struct ErrorInfo {
  std::string message;
  std::string stack;
};

// The message and stack of whatever a script threw or rejected with: an Error,
// or any other value.
ErrorInfo DescribeError(v8::Isolate* isolate,
                        v8::Local<v8::Context> context,
                        v8::Local<v8::Value> value) {
  ErrorInfo info;
  v8::TryCatch try_catch(isolate);
  if (value.IsEmpty()) {
    info.message = "the script was stopped";
    return info;
  }
  if (value->IsObject()) {
    v8::Local<v8::Object> object = value.As<v8::Object>();
    v8::Local<v8::Value> field;
    if (object->Get(context, v8::String::NewFromUtf8Literal(isolate, "message"))
            .ToLocal(&field) &&
        field->IsString()) {
      info.message = ToStdString(isolate, field);
    }
    if (object->Get(context, v8::String::NewFromUtf8Literal(isolate, "stack"))
            .ToLocal(&field) &&
        field->IsString()) {
      info.stack = ToStdString(isolate, field);
    }
  }
  if (info.message.empty()) {
    v8::Local<v8::String> text;
    if (value->ToString(context).ToLocal(&text)) {
      info.message = ToStdString(isolate, text);
    }
  }
  if (info.message.empty()) {
    info.message = "the script threw a value that cannot be shown";
  }
  return info;
}

v8::MaybeLocal<v8::Module> RejectImport(v8::Local<v8::Context> context,
                                        v8::Local<v8::String> specifier,
                                        v8::Local<v8::FixedArray> attributes,
                                        v8::Local<v8::Module> referrer) {
  // Rolldown inlines @bdk/* and every other dependency into the one file; an
  // import left in it means the build is wrong, and nothing can satisfy it
  // here.
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  isolate->ThrowException(v8::Exception::Error(NewString(
      isolate, base::StrCat({"an automation script cannot import modules, "
                             "but it imports \"",
                             ToStdString(isolate, specifier),
                             "\"; bundle it with `bdk build automation`"}))));
  return v8::MaybeLocal<v8::Module>();
}

// Initialises V8 once for the process. Flags and the snapshot must come first.
void InitializeV8() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
#if defined(V8_USE_EXTERNAL_STARTUP_DATA)
#if BUILDFLAG(USE_V8_CONTEXT_SNAPSHOT)
  gin::V8Initializer::LoadV8Snapshot(
      gin::V8SnapshotFileType::kWithAdditionalContext);
#else
  gin::V8Initializer::LoadV8Snapshot();
#endif
#endif
  // --jitless: the helper that hosts a utility process has no
  // com.apple.security.cs.allow-jit entitlement (only the GPU and renderer
  // helpers do, chrome/app/helper-*-entitlements.plist), so V8 cannot map
  // executable memory there. It also removes dynamic code and WebAssembly from
  // the sandboxed process. Flags go through Initialize, which applies them
  // before V8 starts; feature-flag overrides are refused so that no field
  // trial can switch the JIT back on.
  gin::IsolateHolder::Initialize(gin::IsolateHolder::kNonStrictMode,
                                 gin::ArrayBufferAllocator::SharedInstance(),
                                 /*reference_table=*/nullptr,
                                 /*js_command_line_flags=*/"--jitless",
                                 /*disallow_v8_feature_flag_overrides=*/true);
}

// Stops a script that runs past its task's CPU budget (T-AUT-07). The isolate
// runs one script at a time, on the host's thread. While one runs, a task on
// the watchdog's own thread is due when the task's remaining budget has
// passed; if the same run is still going then, it terminates the execution.
// The wait is wall-clock time, which is never less than the CPU time the
// script uses meanwhile, so a script is never stopped before its budget.
class BudgetWatchdog {
 public:
  explicit BudgetWatchdog(v8::Isolate* isolate)
      : isolate_(isolate), thread_("BdkAutomationBudget") {
    CHECK(thread_.Start());
  }

  BudgetWatchdog(const BudgetWatchdog&) = delete;
  BudgetWatchdog& operator=(const BudgetWatchdog&) = delete;

  // Joins the thread first: its pending task refers to this.
  ~BudgetWatchdog() { thread_.Stop(); }

  // A run starts with `remaining` budget.
  void Arm(base::TimeDelta remaining) {
    uint64_t run;
    {
      base::AutoLock lock(lock_);
      run = ++run_;
      armed_ = true;
      fired_ = false;
    }
    thread_.task_runner()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&BudgetWatchdog::Fire, base::Unretained(this), run),
        remaining);
  }

  // The run ended. Returns whether the watchdog stopped it.
  bool Disarm() {
    base::AutoLock lock(lock_);
    armed_ = false;
    return fired_;
  }

  // Whether the watchdog stopped the run that is going on.
  bool Fired() {
    base::AutoLock lock(lock_);
    return armed_ && fired_;
  }

 private:
  void Fire(uint64_t run) {
    base::AutoLock lock(lock_);
    if (armed_ && run == run_) {
      fired_ = true;
      isolate_->TerminateExecution();
    }
  }

  const raw_ptr<v8::Isolate> isolate_;
  base::Thread thread_;
  base::Lock lock_;
  uint64_t run_ GUARDED_BY(lock_) = 0;
  bool armed_ GUARDED_BY(lock_) = false;
  bool fired_ GUARDED_BY(lock_) = false;
};

// One task: a context of its own in the process's isolate, running one script.
class ScriptTask : public mojom::AutomationTask {
 public:
  ScriptTask(v8::Isolate* isolate,
             BudgetWatchdog* watchdog,
             uint32_t serial,
             base::TimeDelta budget,
             mojo::PendingReceiver<mojom::AutomationTask> task,
             mojo::PendingRemote<mojom::AutomationTaskClient> client,
             base::OnceClosure on_closed)
      : isolate_(isolate),
        watchdog_(watchdog),
        serial_(serial),
        budget_(budget),
        receiver_(this, std::move(task)),
        client_(std::move(client)),
        on_closed_(std::move(on_closed)) {
    TaskRegistry()[serial_] = weak_factory_.GetWeakPtr();
    receiver_.set_disconnect_handler(
        base::BindOnce(&ScriptTask::OnDisconnect, base::Unretained(this)));
  }

  ScriptTask(const ScriptTask&) = delete;
  ScriptTask& operator=(const ScriptTask&) = delete;

  ~ScriptTask() override {
    TaskRegistry().erase(serial_);
    timers_.clear();
    module_.Reset();
    host_object_.Reset();
    context_.Reset();
  }

  uint32_t serial() const { return serial_; }

  // Compiles and evaluates `source`, then (once the module has settled)
  // delivers `start_frame` and calls the default export. Order matters: the
  // @bdk/automation library sets `__bdkHost.onmessage` while the module
  // evaluates, so the start frame goes to it after evaluation and before the
  // default export runs.
  void Start(const std::string& source, std::string start_frame) {
    start_frame_ = std::move(start_frame);
    ScriptRun run(this);
    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> context = v8::Context::New(isolate_);
    context_.Reset(isolate_, context);
    v8::Context::Scope context_scope(context);

    if (!InstallGlobals(context)) {
      Fail("the automation host could not set up the script's environment");
      return;
    }

    v8::TryCatch try_catch(isolate_);
    v8::ScriptOrigin origin(NewString(isolate_, "bdk-automation:script"), 0, 0,
                            false, -1, v8::Local<v8::Value>(), false, false,
                            /*is_module=*/true);
    v8::ScriptCompiler::Source script_source(NewString(isolate_, source),
                                             origin);
    v8::Local<v8::Module> module;
    if (!v8::ScriptCompiler::CompileModule(isolate_, &script_source)
             .ToLocal(&module)) {
      FailFromTryCatch(context, try_catch, "the script cannot be compiled");
      return;
    }
    module_.Reset(isolate_, module);
    if (!module->InstantiateModule(context, &RejectImport).FromMaybe(false)) {
      FailFromTryCatch(context, try_catch, "the script cannot be linked");
      return;
    }
    v8::Local<v8::Promise> evaluated;
    if (!module->Evaluate(context).ToLocal(&evaluated)) {
      FailFromTryCatch(context, try_catch, "the script cannot be evaluated");
      return;
    }
    // Await rather than read the state: a module with top-level await settles
    // later.
    Await(context, evaluated, Stage::kModuleEvaluated, Stage::kModuleRejected);
    isolate_->PerformMicrotaskCheckpoint();
  }

  // mojom::AutomationTask:
  void Deliver(const std::string& frame) override {
    if (finished_) {
      return;
    }
    if (!started_) {
      pending_.push_back(frame);
      return;
    }
    ScriptRun run(this);
    Entered entered(isolate_, context_);
    DeliverToScript(entered.context, frame);
    isolate_->PerformMicrotaskCheckpoint();
  }

 private:
  // What a promise reaction means; encoded with the serial in the function's
  // data (serial << 2 | stage).
  enum class Stage : uint32_t {
    kModuleEvaluated,
    kModuleRejected,
    kResultResolved,
    kResultRejected,
  };

  struct Timer {
    base::OneShotTimer timer;
    v8::Global<v8::Function> callback;
    std::vector<v8::Global<v8::Value>> arguments;
  };

  // One entry into the script (its start, a frame delivered, a timer): its
  // CPU time counts against the task's budget, and the watchdog may stop it.
  class ScriptRun {
   public:
    explicit ScriptRun(ScriptTask* task)
        : task_(task), start_(base::ThreadTicks::Now()) {
      if (task_->budget_.is_positive()) {
        task_->watchdog_->Arm(
            std::max(task_->budget_ - task_->used_, base::TimeDelta()));
      }
    }

    ScriptRun(const ScriptRun&) = delete;
    ScriptRun& operator=(const ScriptRun&) = delete;

    ~ScriptRun() {
      if (!task_->budget_.is_positive()) {
        return;
      }
      task_->used_ += base::ThreadTicks::Now() - start_;
      const bool stopped = task_->watchdog_->Disarm();
      if (stopped) {
        // The isolate serves the other tasks next.
        task_->isolate_->CancelTerminateExecution();
      }
      if (stopped || task_->used_ >= task_->budget_) {
        task_->ExceedBudget();
      }
    }

   private:
    const raw_ptr<ScriptTask> task_;
    const base::ThreadTicks start_;
  };

  // The isolate, a handle scope and the task's context, entered.
  struct Entered {
    Entered(v8::Isolate* isolate, const v8::Global<v8::Context>& global)
        : isolate_scope(isolate),
          handle_scope(isolate),
          context(global.Get(isolate)),
          context_scope(context) {}
    v8::Isolate::Scope isolate_scope;
    v8::HandleScope handle_scope;
    v8::Local<v8::Context> context;
    v8::Context::Scope context_scope;
  };

  static ScriptTask* Lookup(uint32_t serial) {
    auto it = TaskRegistry().find(serial);
    return it == TaskRegistry().end() ? nullptr : it->second.get();
  }

  static uint32_t DataOf(const v8::FunctionCallbackInfo<v8::Value>& info) {
    return info.DataV2()
        .As<v8::Value>()
        ->Uint32Value(info.GetIsolate()->GetCurrentContext())
        .FromMaybe(0);
  }

  // `__bdkHost.send(frame)`.
  static void NativeSend(const v8::FunctionCallbackInfo<v8::Value>& info) {
    ScriptTask* task = Lookup(DataOf(info));
    if (!task) {
      return;
    }
    v8::Isolate* isolate = info.GetIsolate();
    if (info.Length() < 1 || !info[0]->IsString()) {
      isolate->ThrowException(v8::Exception::TypeError(
          NewString(isolate, "__bdkHost.send expects a string")));
      return;
    }
    v8::String::Utf8Value text(isolate, info[0]);
    if (text.length() > kMaxFrameBytes) {
      isolate->ThrowException(v8::Exception::RangeError(
          NewString(isolate, "the frame is larger than 32 MiB")));
      return;
    }
    task->SendFrame(std::string(text.as_view()));
  }

  // `setTimeout(callback, delay, ...arguments)`: returns a number.
  static void NativeSetTimeout(
      const v8::FunctionCallbackInfo<v8::Value>& info) {
    ScriptTask* task = Lookup(DataOf(info));
    if (!task) {
      return;
    }
    v8::Isolate* isolate = info.GetIsolate();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    if (info.Length() < 1 || !info[0]->IsFunction()) {
      isolate->ThrowException(v8::Exception::TypeError(
          NewString(isolate, "setTimeout expects a function")));
      return;
    }
    if (task->timers_.size() >= kMaxTimers) {
      isolate->ThrowException(v8::Exception::RangeError(
          NewString(isolate, "too many pending timers")));
      return;
    }
    double delay = 0;
    if (info.Length() > 1) {
      delay = info[1]->NumberValue(context).FromMaybe(0);
    }
    if (!(delay > 0)) {  // Also NaN.
      delay = 0;
    }
    delay = std::min(delay, 2147483647.0);

    int id = task->next_timer_id_++;
    auto timer = std::make_unique<Timer>();
    timer->callback.Reset(isolate, info[0].As<v8::Function>());
    for (int i = 2; i < info.Length(); ++i) {
      timer->arguments.emplace_back(isolate, info[i]);
    }
    timer->timer.Start(FROM_HERE,
                       base::Milliseconds(static_cast<int64_t>(delay)),
                       base::BindOnce(&ScriptTask::FireTimer,
                                      task->weak_factory_.GetWeakPtr(), id));
    task->timers_[id] = std::move(timer);
    info.GetReturnValue().Set(v8::Integer::New(isolate, id));
  }

  // `clearTimeout(id)`: anything that is not a pending timer's id is ignored.
  static void NativeClearTimeout(
      const v8::FunctionCallbackInfo<v8::Value>& info) {
    ScriptTask* task = Lookup(DataOf(info));
    if (!task || info.Length() < 1) {
      return;
    }
    double id = info[0]
                    ->NumberValue(info.GetIsolate()->GetCurrentContext())
                    .FromMaybe(0);
    task->timers_.erase(static_cast<int>(id));
  }

  // The reaction of a promise the task awaits.
  static void NativeSettled(const v8::FunctionCallbackInfo<v8::Value>& info) {
    uint32_t data = DataOf(info);
    ScriptTask* task = Lookup(data >> 2);
    if (!task) {
      return;
    }
    v8::Local<v8::Value> value =
        info.Length() > 0 ? info[0]
                          : v8::Undefined(info.GetIsolate()).As<v8::Value>();
    task->OnSettled(static_cast<Stage>(data & 3), value);
  }

  v8::Local<v8::Function> MakeFunction(v8::Local<v8::Context> context,
                                       v8::FunctionCallback callback,
                                       uint32_t data) {
    return v8::Function::New(context, callback,
                             v8::Integer::NewFromUnsigned(isolate_, data))
        .ToLocalChecked();
  }

  // `globalThis.__bdkHost = {send, onmessage: null}` (before the module runs,
  // which sets onmessage), `setTimeout`, `clearTimeout`, then the prelude.
  bool InstallGlobals(v8::Local<v8::Context> context) {
    v8::Local<v8::Object> global = context->Global();
    v8::Local<v8::Object> host = v8::Object::New(isolate_);
    if (host->Set(context, v8::String::NewFromUtf8Literal(isolate_, "send"),
                  MakeFunction(context, &NativeSend, serial_))
            .IsNothing() ||
        host->Set(context,
                  v8::String::NewFromUtf8Literal(isolate_, "onmessage"),
                  v8::Null(isolate_))
            .IsNothing() ||
        global
            ->Set(context,
                  v8::String::NewFromUtf8Literal(isolate_, "__bdkHost"), host)
            .IsNothing() ||
        global
            ->Set(context,
                  v8::String::NewFromUtf8Literal(isolate_, "setTimeout"),
                  MakeFunction(context, &NativeSetTimeout, serial_))
            .IsNothing() ||
        global
            ->Set(context,
                  v8::String::NewFromUtf8Literal(isolate_, "clearTimeout"),
                  MakeFunction(context, &NativeClearTimeout, serial_))
            .IsNothing()) {
      return false;
    }
    host_object_.Reset(isolate_, host);

    v8::ScriptOrigin origin(NewString(isolate_, "bdk-automation:prelude"));
    v8::ScriptCompiler::Source source(NewString(isolate_, kPrelude), origin);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> ignored;
    return v8::ScriptCompiler::Compile(context, &source).ToLocal(&script) &&
           script->Run(context).ToLocal(&ignored);
  }

  // Calls `ok` or `bad` once `value` (any value, not only a promise) settles.
  void Await(v8::Local<v8::Context> context,
             v8::Local<v8::Value> value,
             Stage ok,
             Stage bad) {
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
        resolver->Resolve(context, value).IsNothing()) {
      Fail("the automation host could not wait for the script");
      return;
    }
    v8::Local<v8::Promise> promise = resolver->GetPromise();
    promise->MarkAsHandled();
    uint32_t base_data = serial_ << 2;
    if (promise
            ->Then(context,
                   MakeFunction(context, &NativeSettled,
                                base_data | static_cast<uint32_t>(ok)),
                   MakeFunction(context, &NativeSettled,
                                base_data | static_cast<uint32_t>(bad)))
            .IsEmpty()) {
      Fail("the automation host could not wait for the script");
    }
  }

  void OnSettled(Stage stage, v8::Local<v8::Value> value) {
    if (finished_) {
      return;
    }
    v8::Local<v8::Context> context = isolate_->GetCurrentContext();
    switch (stage) {
      case Stage::kModuleEvaluated:
        RunDefaultExport(context);
        break;
      case Stage::kModuleRejected:
        FailWith(context, value, "the script cannot be evaluated");
        break;
      case Stage::kResultResolved:
        Finish(context, value);
        break;
      case Stage::kResultRejected:
        SendFailedFrame(context, value);
        break;
    }
  }

  // The module evaluated: find its default export, deliver the start frame,
  // call the export with the task's arguments.
  void RunDefaultExport(v8::Local<v8::Context> context) {
    v8::TryCatch try_catch(isolate_);
    v8::Local<v8::Module> module = module_.Get(isolate_);
    v8::Local<v8::Value> exports = module->GetModuleNamespace();
    v8::Local<v8::Value> entry;
    if (!exports->IsObject() ||
        !exports.As<v8::Object>()
             ->Get(context, v8::String::NewFromUtf8Literal(isolate_, "default"))
             .ToLocal(&entry) ||
        !entry->IsFunction()) {
      Fail("the script has no default export function (export default ...)");
      return;
    }
    v8::Local<v8::Value> start;
    v8::Local<v8::Value> arguments;
    if (!v8::JSON::Parse(context, NewString(isolate_, start_frame_))
             .ToLocal(&start) ||
        !start->IsObject() ||
        !start.As<v8::Object>()
             ->Get(context,
                   v8::String::NewFromUtf8Literal(isolate_, "arguments"))
             .ToLocal(&arguments)) {
      Fail("the start frame is not a JSON object");
      return;
    }

    DeliverToScript(context, start_frame_);
    start_frame_.clear();
    started_ = true;

    v8::Local<v8::Value> argv[] = {arguments};
    v8::Local<v8::Value> result;
    if (!entry.As<v8::Function>()
             ->Call(context, v8::Undefined(isolate_), 1, argv)
             .ToLocal(&result)) {
      SendFailedFrame(context, try_catch.Exception());
      return;
    }
    Await(context, result, Stage::kResultResolved, Stage::kResultRejected);

    // Frames that arrived while the module was evaluating.
    std::deque<std::string> pending = std::move(pending_);
    for (const std::string& frame : pending) {
      if (finished_) {
        break;
      }
      DeliverToScript(context, frame);
    }
  }

  // Calls `__bdkHost.onmessage(frame)` if the script set it. A throwing
  // handler is logged and does not end the task.
  void DeliverToScript(v8::Local<v8::Context> context,
                       const std::string& frame) {
    v8::Local<v8::Object> host = host_object_.Get(isolate_);
    v8::Local<v8::Value> handler;
    if (!host->Get(context,
                   v8::String::NewFromUtf8Literal(isolate_, "onmessage"))
             .ToLocal(&handler) ||
        !handler->IsFunction()) {
      return;
    }
    v8::TryCatch try_catch(isolate_);
    v8::Local<v8::Value> argv[] = {NewString(isolate_, frame)};
    if (handler.As<v8::Function>()->Call(context, host, 1, argv).IsEmpty()) {
      SendLog(
          "error",
          "__bdkHost.onmessage threw: " +
              DescribeError(isolate_, context, try_catch.Exception()).message);
    }
  }

  void FireTimer(int id) {
    auto it = timers_.find(id);
    if (it == timers_.end() || finished_) {
      return;
    }
    std::unique_ptr<Timer> timer = std::move(it->second);
    timers_.erase(it);

    ScriptRun run(this);
    Entered entered(isolate_, context_);
    v8::TryCatch try_catch(isolate_);
    std::vector<v8::Local<v8::Value>> argv;
    for (const auto& argument : timer->arguments) {
      argv.push_back(argument.Get(isolate_));
    }
    if (timer->callback.Get(isolate_)
            ->Call(entered.context, v8::Undefined(isolate_),
                   base::checked_cast<int>(argv.size()), argv.data())
            .IsEmpty()) {
      SendLog("error", "a timer callback threw: " +
                           DescribeError(isolate_, entered.context,
                                         try_catch.Exception())
                               .message);
    }
    isolate_->PerformMicrotaskCheckpoint();
  }

  // The default export returned (or its promise resolved): `done`.
  void Finish(v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
    std::string json = "null";
    // JSON.stringify answers undefined for these; the frame says null.
    if (!value->IsUndefined() && !value->IsFunction() && !value->IsSymbol()) {
      v8::TryCatch try_catch(isolate_);
      v8::Local<v8::String> text;
      if (v8::JSON::Stringify(context, value).ToLocal(&text)) {
        json = ToStdString(isolate_, text);
      } else if (try_catch.HasCaught()) {
        SendFailedMessage(
            "the task's result cannot be converted to JSON: " +
            DescribeError(isolate_, context, try_catch.Exception()).message);
        return;
      }
    }
    if (json.size() > kMaxFrameBytes) {
      SendFailedMessage("the task's result is larger than 32 MiB as JSON");
      return;
    }
    Conclude(base::StrCat({"{\"type\":\"done\",\"result\":", json, "}"}));
  }

  // The default export threw or rejected: `failed`.
  void SendFailedFrame(v8::Local<v8::Context> context,
                       v8::Local<v8::Value> error) {
    ErrorInfo info = DescribeError(isolate_, context, error);
    base::DictValue details;
    details.Set("message", info.message);
    if (!info.stack.empty()) {
      details.Set("stack", info.stack);
    }
    base::DictValue frame;
    frame.Set("type", "failed");
    frame.Set("error", std::move(details));
    Conclude(ToJsonText(frame));
  }

  void SendFailedMessage(const std::string& message) {
    base::DictValue details;
    details.Set("message", message);
    base::DictValue frame;
    frame.Set("type", "failed");
    frame.Set("error", std::move(details));
    Conclude(ToJsonText(frame));
  }

  void SendLog(const char* level, const std::string& message) {
    base::DictValue frame;
    frame.Set("type", "log");
    frame.Set("level", level);
    frame.Set("message", message);
    SendFrame(ToJsonText(frame));
  }

  void SendFrame(std::string frame) {
    if (!finished_ && !OverBudget()) {
      client_->Send(std::move(frame));
    }
  }

  // The last frame of the task; nothing is delivered or sent after it, and the
  // pending timers go. The browser closes the task's pipe when it has read it.
  // A script stopped at its budget unwinds through here with a termination
  // error; it ends as over budget instead.
  void Conclude(std::string frame) {
    if (OverBudget()) {
      ExceedBudget();
      return;
    }
    SendFrame(std::move(frame));
    finished_ = true;
    timers_.clear();
  }

  // The script could not be loaded or started (not: the script threw).
  void Fail(const std::string& message) {
    if (OverBudget()) {
      ExceedBudget();
      return;
    }
    if (finished_) {
      return;
    }
    finished_ = true;
    timers_.clear();
    client_->Failed(message);
  }

  void FailWith(v8::Local<v8::Context> context,
                v8::Local<v8::Value> error,
                const std::string& what) {
    Fail(what + ": " + DescribeError(isolate_, context, error).message);
  }

  void FailFromTryCatch(v8::Local<v8::Context> context,
                        const v8::TryCatch& try_catch,
                        const std::string& what) {
    std::string where;
    v8::Local<v8::Message> message = try_catch.Message();
    if (!message.IsEmpty()) {
      where = " (line " +
              std::to_string(message->GetLineNumber(context).FromMaybe(0)) +
              ")";
    }
    Fail(what + where + ": " +
         DescribeError(isolate_, context, try_catch.Exception()).message);
  }

  bool OverBudget() { return budget_.is_positive() && watchdog_->Fired(); }

  // The script used its budget: the task ends, and only BudgetExceeded says
  // so (a script cannot send that itself).
  void ExceedBudget() {
    if (finished_) {
      return;
    }
    finished_ = true;
    timers_.clear();
    client_->BudgetExceeded(
        base::StrCat({"the script used its CPU budget of ",
                      base::NumberToString(budget_.InMilliseconds()), " ms"}));
  }

  void OnDisconnect() {
    // The browser closed the task's pipe (or went away): this task is over.
    // Moving the callback out first keeps `this` untouched after it runs.
    base::OnceClosure closed = std::move(on_closed_);
    std::move(closed).Run();
  }

  raw_ptr<v8::Isolate> isolate_;
  raw_ptr<BudgetWatchdog> watchdog_;
  const uint32_t serial_;
  // Zero: no budget.
  const base::TimeDelta budget_;
  base::TimeDelta used_;
  mojo::Receiver<mojom::AutomationTask> receiver_;
  mojo::Remote<mojom::AutomationTaskClient> client_;
  base::OnceClosure on_closed_;

  v8::Global<v8::Context> context_;
  v8::Global<v8::Module> module_;
  v8::Global<v8::Object> host_object_;

  std::string start_frame_;
  std::deque<std::string> pending_;  // Delivered once the export has run.
  bool started_ = false;
  bool finished_ = false;
  std::map<int, std::unique_ptr<Timer>> timers_;
  int next_timer_id_ = 1;

  base::WeakPtrFactory<ScriptTask> weak_factory_{this};
};

class AutomationHostService : public mojom::AutomationHost {
 public:
  explicit AutomationHostService(
      mojo::PendingReceiver<mojom::AutomationHost> receiver)
      : receiver_(this, std::move(receiver)) {}

  AutomationHostService(const AutomationHostService&) = delete;
  AutomationHostService& operator=(const AutomationHostService&) = delete;

  ~AutomationHostService() override = default;

  // mojom::AutomationHost:
  void StartTask(
      const std::string& task_id,
      const std::string& script_source,
      const std::string& start_frame,
      uint32_t cpu_budget_ms,
      mojo::PendingReceiver<mojom::AutomationTask> task,
      mojo::PendingRemote<mojom::AutomationTaskClient> client) override {
    EnsureIsolate();
    uint32_t serial = (next_serial_++) & 0x3fffffff;
    auto script_task = std::make_unique<ScriptTask>(
        isolate_holder_->isolate(), watchdog_.get(), serial,
        base::Milliseconds(cpu_budget_ms), std::move(task), std::move(client),
        base::BindOnce(&AutomationHostService::RemoveTask,
                       base::Unretained(this), serial));
    ScriptTask* raw_task = script_task.get();
    tasks_[serial] = std::move(script_task);
    raw_task->Start(script_source, start_frame);
  }

 private:
  void EnsureIsolate() {
    if (isolate_holder_) {
      return;
    }
    InitializeV8();
    std::unique_ptr<v8::Isolate::CreateParams> params =
        gin::IsolateHolder::getDefaultIsolateParams();
    params->constraints.set_max_old_generation_size_in_bytes(kHeapLimitBytes);
    params->allow_atomics_wait = false;
    isolate_holder_ = std::make_unique<gin::IsolateHolder>(
        base::SingleThreadTaskRunner::GetCurrentDefault(),
        gin::IsolateHolder::kSingleThread,
        gin::IsolateHolder::IsolateType::kUtility, std::move(params));
    // Microtasks run when this code says so (after every entry into a script),
    // not whenever V8's call depth reaches zero.
    isolate_holder_->isolate()->SetMicrotasksPolicy(
        v8::MicrotasksPolicy::kExplicit);
    watchdog_ = std::make_unique<BudgetWatchdog>(isolate_holder_->isolate());
  }

  void RemoveTask(uint32_t serial) { tasks_.erase(serial); }

  mojo::Receiver<mojom::AutomationHost> receiver_;
  // Declared before the tasks: tasks hold V8 handles and must go first.
  std::unique_ptr<gin::IsolateHolder> isolate_holder_;
  // Between the two: it uses the isolate, and the tasks use it.
  std::unique_ptr<BudgetWatchdog> watchdog_;
  std::map<uint32_t, std::unique_ptr<ScriptTask>> tasks_;
  uint32_t next_serial_ = 1;
};

}  // namespace

std::unique_ptr<mojom::AutomationHost> RunAutomationHost(
    mojo::PendingReceiver<mojom::AutomationHost> receiver) {
  return std::make_unique<AutomationHostService>(std::move(receiver));
}

}  // namespace bdk
