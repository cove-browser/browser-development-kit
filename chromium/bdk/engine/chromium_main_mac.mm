// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/chromium_main.h"

#import <AppKit/AppKit.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "base/check.h"
#include "base/command_line.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/sampling_heap_profiler/poisson_allocation_sampler.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "bdk/engine/content_browser_client.h"
#include "chrome/app/chrome_main.h"
#include "chrome/app/chrome_main_delegate.h"
#include "chrome/app/chrome_main_mac.h"
#include "chrome/app/startup_timestamps.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "components/keep_alive_registry/keep_alive_types.h"
#include "components/keep_alive_registry/scoped_keep_alive.h"
#include "content/public/app/content_main.h"
#include "content/public/app/content_main_runner.h"
#include "content/public/browser/browser_main_runner.h"
#include "content/public/common/main_function_params.h"

namespace bdk {

namespace {

// The switch the //chrome start-up patch reads to leave NSApplication's
// delegate and main menu to the hosting application.
constexpr char kBdkHostedSwitch[] = "bdk-hosted";

// ChromeMainDelegate, except that the browser process is initialised and
// handed back to the application instead of running its main loop here.
class HostedMainDelegate : public ChromeMainDelegate {
 public:
  explicit HostedMainDelegate(const StartupTimestamps& timestamps)
      : ChromeMainDelegate(timestamps) {}

  std::variant<int, content::MainFunctionParams> RunProcess(
      const std::string& process_type,
      content::MainFunctionParams main_function_params) override {
    if (!process_type.empty()) {
      return ChromeMainDelegate::RunProcess(process_type,
                                            std::move(main_function_params));
    }
    browser_runner_ = content::BrowserMainRunner::Create();
    // -1 means the browser initialised and its main loop should run.
    const int exit_code =
        browser_runner_->Initialize(std::move(main_function_params));
    if (exit_code >= 0) {
      browser_runner_.reset();
      return exit_code;
    }
    return 0;
  }

  std::unique_ptr<ChromeContentBrowserClient> CreateChromeContentBrowserClient()
      override {
    return std::make_unique<BdkContentBrowserClient>();
  }

  content::BrowserMainRunner* browser_runner() { return browser_runner_.get(); }

  std::unique_ptr<content::BrowserMainRunner> TakeBrowserRunner() {
    return std::move(browser_runner_);
  }

 private:
  std::unique_ptr<content::BrowserMainRunner> browser_runner_;
};

struct State {
  // The argv strings and the array that points at them must outlive the
  // content runner: base::CommandLine and the process title keep pointers.
  std::vector<std::string> args;
  std::vector<const char*> argv;
  std::unique_ptr<HostedMainDelegate> delegate;
  std::unique_ptr<content::ContentMainRunner> content_runner;
  std::unique_ptr<content::BrowserMainRunner> browser_runner;
  std::unique_ptr<ScopedKeepAlive> keep_alive;
  base::OnceClosure stopped;
  bool started = false;
  bool running = false;
  bool loop_running = false;
};

State& GetState() {
  static base::NoDestructor<State> state;
  return *state;
}

void FinishStop() {
  State& state = GetState();
  if (state.browser_runner) {
    state.browser_runner->Shutdown();
    state.browser_runner.reset();
  }
  if (state.content_runner) {
    state.content_runner->Shutdown();
  }
  state.running = false;
  if (state.stopped) {
    // The task runners are gone with the content runner; the main queue is
    // not.
    PostToMainThread(std::move(state.stopped));
  }
}

// Runs the browser main loop nested inside the application's loop. It
// returns when Chromium quits, which StopChromium() causes.
void RunBrowserLoop() {
  State& state = GetState();
  if (!state.running || !state.browser_runner) {
    return;
  }
  state.loop_running = true;
  state.browser_runner->Run();
  state.loop_running = false;
  FinishStop();
}

}  // namespace

ChromiumStartOptions::ChromiumStartOptions() = default;
ChromiumStartOptions::ChromiumStartOptions(const ChromiumStartOptions&) =
    default;
ChromiumStartOptions::~ChromiumStartOptions() = default;

bool StartChromium(const ChromiumStartOptions& options, std::string* error) {
  CHECK([NSThread isMainThread]);
  State& state = GetState();
  if (state.started) {
    *error = "Chromium was already started in this process";
    return false;
  }
  if (!NSApp ||
      ![NSApp isKindOfClass:NSClassFromString(@"BrowserCrApplication")]) {
    *error = "the application's principal class must be BDKApplication or a "
             "subclass of it";
    return false;
  }
  state.started = true;

  state.args.push_back(options.program.value());
  state.args.push_back(std::string("--") + kBdkHostedSwitch);
  state.args.push_back("--user-data-dir=" + options.user_data_dir.value());
  // The application owns windows and session restoration: Chromium opens no
  // window of its own at start-up (architecture.md section 4.4), and runs no
  // first-run or default-browser flow.
  state.args.push_back("--no-startup-window");
  state.args.push_back("--no-first-run");
  state.args.push_back("--no-default-browser-check");
  // Chromium copies its bundle to a temporary directory at every launch
  // (MacAppCodeSignClone) so that an in-place update cannot invalidate the
  // running code's signature. BDK never updates itself, and a copy left by a
  // process that does not exit cleanly is never removed: engine test runs
  // left gigabytes.
  std::string disabled_features = "MacAppCodeSignClone";
  // Browser dialogs that are tab-modal views dialogs, which a BDK page's
  // window cannot parent (it is not a views widget, so each ended the process
  // at a check), and that BDK neither routes nor shows a sheet for yet
  // (ui-requests.md 8.2, T-UIR-10). HTTPS-First then asks with its
  // interstitial page, in the page, as it did before the dialog. The others'
  // APIs are absent from pages, so a site sees them as unsupported: Payment
  // Request and Secure Payment Confirmation, FedCM, the Digital Credentials
  // API, and Web Bluetooth (its chooser is refused, ui_browser_dialogs.h, and
  // its adapter would need the application's Bluetooth usage string). Pages
  // ask for web app installs only through beforeinstallprompt, which
  // [web_apps] enabled governs (web-apps.md 10, T-PWA-06): navigator.install()
  // and the <install> element reach views dialogs whether installs are on or
  // off, so both are absent (WebAppInstallation, InstallElement).
  disabled_features +=
      ",HttpsFirstDialogUi,WebPayments,SecurePaymentConfirmation,FedCm,"
      "WebIdentityDigitalCredentials,WebIdentityDigitalCredentialsCreation,"
      "WebBluetooth,WebAppInstallation,InstallElement";
  // The heavy-ad intervention's opt-out store is kept in memory, as Chrome
  // keeps it for an off-the-record profile. On disk, it opens a database at
  // best-effort priority when the profile starts; a profile deleted before
  // that load finishes is a fatal check in the SQL layer, and the application
  // decides when its profiles are deleted. The cost: how often the
  // intervention has fired on a site is forgotten when the application quits.
  //
  // Start-up ends when the runtime's start has completed and Chromium's main
  // loop is first idle, not when a visible tab of a Chromium window has loaded
  // (there is none): see startup_completion.h.
  std::string enabled_features =
      "HeavyAdPrivacyMitigations:OptOutStoreDisabled/true,"
      "ImprovedStartupBestEffortDelay:StartupDelayVisibleTabTimeout/0s";
  // Chromium reads only the last --disable-features and --enable-features, so
  // the caller's lists are merged into one switch each.
  constexpr std::string_view kDisableFeatures = "disable-features=";
  constexpr std::string_view kEnableFeatures = "enable-features=";
  for (const std::string& s : options.switches) {
    if (s.starts_with(kDisableFeatures)) {
      disabled_features += "," + s.substr(kDisableFeatures.size());
      continue;
    }
    if (s.starts_with(kEnableFeatures)) {
      enabled_features += "," + s.substr(kEnableFeatures.size());
      continue;
    }
    state.args.push_back("--" + s);
  }
  state.args.push_back("--disable-features=" + disabled_features);
  state.args.push_back("--enable-features=" + enabled_features);
  for (const std::string& a : state.args) {
    state.argv.push_back(a.c_str());
  }

  // The command line already exists (BDKApplicationBase sets it up for
  // Chromium's application class); it becomes Chromium's.
  if (base::CommandLine::InitializedForCurrentProcess()) {
    base::CommandLine::ForCurrentProcess()->InitFromArgv(
        static_cast<int>(state.argv.size()), state.argv.data());
  } else {
    base::CommandLine::Init(static_cast<int>(state.argv.size()),
                            state.argv.data());
  }
  // ChromeMain() records this snapshot; chrome://policy and launch reporting
  // read it (patch:
  // bdk-let-an-embedder-record-the-initial-browser-command-line).
  SetInitialBrowserCommandLine(*base::CommandLine::ForCurrentProcess());
  SetUpBundleOverrides();
  base::PoissonAllocationSampler::Init();

  state.delegate = std::make_unique<HostedMainDelegate>(
      StartupTimestamps{.exe_entry_point_ticks = base::TimeTicks::Now()});
  content::ContentMainParams params(state.delegate.get());
  params.argc = static_cast<int>(state.argv.size());
  params.argv = state.argv.data();
  params.keep_runner_after_run = true;
  state.content_runner = content::ContentMainRunner::Create();
  const int exit_code =
      content::RunContentProcess(std::move(params), state.content_runner.get());
  state.browser_runner = state.delegate->TakeBrowserRunner();
  if (exit_code != 0 || !state.browser_runner) {
    *error = "Chromium failed to start (exit code " +
             base::NumberToString(exit_code) + ")";
    return false;
  }
  // The application, not a browser window, keeps the browser process alive.
  state.keep_alive = std::make_unique<ScopedKeepAlive>(
      KeepAliveOrigin::APP_CONTROLLER, KeepAliveRestartOption::DISABLED);
  state.running = true;
  // A one-shot run-loop timer starts the nested loop. Not a main-queue block:
  // while a main-queue block runs no other can, so everything posted to the
  // main queue would wait for the browser to stop. Not a run-loop block
  // either: CFRunLoop takes all pending blocks at once, so blocks queued with
  // this one would wait the same way.
  CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(
      kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), 0, 0, 0,
      ^(CFRunLoopTimerRef) {
        RunBrowserLoop();
      });
  CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
  CFRelease(timer);
  return true;
}

bool IsChromiumRunning() {
  return GetState().running;
}

void PostToMainThread(base::OnceClosure task) {
  __block base::OnceClosure block_task = std::move(task);
  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
    std::move(block_task).Run();
  });
  CFRunLoopWakeUp(CFRunLoopGetMain());
}

void StopChromium(base::OnceClosure done) {
  State& state = GetState();
  CHECK(state.running);
  state.stopped = std::move(done);
  browser_shutdown::OnShutdownStarting(
      browser_shutdown::ShutdownType::kOtherExit);
  if (!state.loop_running) {
    // The loop never ran (stopped before the first turn of the application's
    // loop): its task finds `running` false and returns.
    state.keep_alive.reset();
    FinishStop();
    return;
  }
  // Dropping the last keep-alive makes the browser process quit its main
  // loop, and RunBrowserLoop() finishes the stop.
  state.keep_alive.reset();
}

}  // namespace bdk
