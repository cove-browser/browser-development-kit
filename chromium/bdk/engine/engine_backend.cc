// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The Chromium backend of the runtime core (packages/core/runtime): what a
// profile and a page are when Chromium runs them. The core keeps the handles,
// the checks and the bookkeeping, exactly as for the stub runtime.

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "bdk/core/generated/bdk_abi_hash.h"
#include "bdk/core/runtime/automation_internal.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/core/runtime/providers_internal.h"
#include "bdk/engine/automation/automation_host_launcher.h"
#include "bdk/engine/chromium_main.h"
#include "bdk/engine/devtools_frontend.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/action_service.h"
#include "bdk/engine/extensions/bundled_extensions.h"
#include "bdk/engine/extensions/extension_menu.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/host_context.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/identity.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/page_protocol.h"
#include "bdk/engine/pages/bundled_page_ui.h"
#include "bdk/engine/pages/page_commands.h"
#include "bdk/engine/pages/page_routes.h"
#include "bdk/engine/providers/geolocation_provider.h"
#include "bdk/engine/providers/provider_settings.h"
#include "bdk/engine/providers/reputation_service.h"
#include "bdk/engine/services/authentication.h"
#include "bdk/engine/services/download_prompts.h"
#include "bdk/engine/services/events.h"
#include "bdk/engine/services/passwords_service.h"
#include "bdk/engine/services/print_prompts.h"
#include "bdk/engine/services/prompt_hooks.h"
#include "bdk/engine/services/services.h"
#include "bdk/engine/services/user_scripts.h"
#include "bdk/engine/session_state.h"
#include "bdk/engine/startup_completion.h"
#include "bdk/engine/testing.h"
#include "bdk/engine/ui_find.h"
#include "bdk/engine/ui_notifications.h"
#include "bdk/engine/ui_payments.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_screen_capture.h"
#include "bdk/engine/ui_select_file.h"
#include "bdk/engine/web_apps.h"
#include "bdk/engine/window_registry.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/profiles/delete_profile_helper.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_metrics.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/ssl_status.h"
#include "content/public/browser/web_contents.h"
#include "net/cert/cert_status_flags.h"
#include "url/gurl.h"

namespace bdk {

namespace {

using bdkcore::Contents;
using bdkcore::Done;
using bdkcore::Runtime;

// How often a profile open waits to see its profile destroyed.
constexpr base::TimeDelta kUnloadPoll = base::Milliseconds(10);

// Lists the persistent profiles and the next free identifier number, in the
// user data directory, next to Chromium's own Local State.
constexpr char kRegistryFile[] = "BDK Profiles.json";

// Application settings (`app.` keys), one file per profile in its directory
// and one for the application's own in the user data directory. The core
// answers the settings commands; the engine only keeps the file. The UI thread
// may not block, so a profile's file is read while it opens and the text is
// kept in memory; a change is written on the registry writer's sequence.
constexpr char kAppSettingsFile[] = "BDK App Settings.json";

// Test only: a runtime shutdown leaves Chromium running, so that one process
// can start and stop many runtimes (the conformance suite does). Chromium can
// be started only once per process.
constexpr char kKeepChromiumEnv[] = "BDK_ENGINE_KEEP_CHROMIUM";

EngineProfile& ProfileData(const bdkcore::Profile& p) {
  return *static_cast<EngineProfile*>(p.data.get());
}

EngineContents& Page(const Contents& c) {
  return *EngineContents::From(c);
}

class EngineBackend final : public bdkcore::Backend {
 public:
  // bdkcore::Backend:
  void Post(std::function<void()> task) override {
    NoteTaskPosted();
    auto run = base::BindOnce(
        [](std::function<void()> t) {
          NoteTaskRun();
          t();
        },
        std::move(task));
    if (chromium_tasks_.load()) {
      content::GetUIThreadTaskRunner({})->PostTask(FROM_HERE, std::move(run));
    } else {
      PostToMainThread(std::move(run));
    }
  }

  // Deadlines of commands (contents.waitFor). Before Chromium runs there is
  // no page to wait for, so the task runs once the queue reaches it.
  void PostDelayed(std::function<void()> task, int64_t delay_ms) override {
    if (!chromium_tasks_.load()) {
      Post(std::move(task));
      return;
    }
    NoteTaskPosted();
    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](std::function<void()> t) {
              NoteTaskRun();
              t();
            },
            std::move(task)),
        base::Milliseconds(delay_ms));
  }

  // The profile snapshot and downgrade policy (T-PKG-05; profile_versions.h)
  // applies to the directory Chromium will open, before it opens it. Chromium
  // starts once per process: a later runtime of the same process uses the
  // directory Chromium already has open, which the policy must not move.
  std::string ProfileDataDirectory(const bdk_start_config_t& config,
                                   const std::string& resolved) override {
    if (IsChromiumRunning()) {
      return "";
    }
    std::optional<Identity> identity = ParseIdentity(resolved);
    if (!identity) {
      return "";  // Start reports it.
    }
    return UserDataDirectory(config, *identity).value();
  }

  std::string EngineVersion() override {
    return std::string(version_info::GetVersionNumber());
  }

  bdk_status_t Start(Runtime& rt,
                     const bdk_start_config_t& config,
                     const std::string& resolved,
                     bdk_error_t* out_error) override {
    std::optional<Identity> identity = ParseIdentity(resolved);
    if (!identity) {
      return bdkcore::fail(out_error, BDK_DOMAIN_CONFIG,
                           BDK_CODE_RESOLVED_INVALID,
                           "the resolved configuration has no browser name");
    }
    base::FilePath user_data_dir = UserDataDirectory(config, *identity);
    if (user_data_dir.empty()) {
      return bdkcore::fail(out_error, BDK_DOMAIN_RUNTIME, "noDataDirectory",
                           "the application data directory is unknown");
    }
    // Providers (T-PRV-09): Chromium's threads read the choice, and the
    // Google key must reach Chromium before anything reads it.
    SetProviderSettings(rt);
    GeolocationProviderChanged();
    {
      std::string key_error;
      if (!ApplyGoogleKey(bdkcore::provider_google_key(rt), &key_error)) {
        return bdkcore::fail(out_error, BDK_DOMAIN_RUNTIME, "startFailed",
                             key_error);
      }
    }
    if (IsChromiumRunning()) {
      if (user_data_dir != requested_user_data_dir_) {
        return bdkcore::fail(
            out_error, BDK_DOMAIN_RUNTIME, "restartUnsupported",
            "Chromium already runs in this process with another data "
            "directory");
      }
    } else {
      if (!base::CreateDirectory(user_data_dir)) {
        return bdkcore::fail(out_error, BDK_DOMAIN_RUNTIME, "noDataDirectory",
                             "cannot create " + user_data_dir.value());
      }
      // Chromium compares profile paths with its own resolved user data
      // directory: resolve symbolic links (/var is /private/var) and doubled
      // separators now.
      requested_user_data_dir_ = user_data_dir;
      user_data_dir = base::MakeAbsoluteFilePath(user_data_dir);
      user_data_dir_ = user_data_dir;
      // The UI thread may not block once Chromium runs: read the registry now
      // and keep it in memory from here on.
      registry_ = ReadRegistry(user_data_dir_);
      app_settings_text_[""] = ReadAppSettings(user_data_dir_);
      ApplyIdentityBeforeStart(*identity);
      ChromiumStartOptions options;
      base::PathService::Get(base::FILE_EXE, &options.program);
      options.user_data_dir = user_data_dir;
      options.switches = ProviderSwitches(CurrentProviderSettings());
      // [services.push] provider = "none" (web-apps.md 7, T-PWA-05): the
      // Push API is absent from pages, so no page can subscribe and Chromium's
      // push client never checks in with Google's messaging hosts (it does on
      // the first subscription). Read once, as Chromium starts.
      if (rt.web_apps.push_provider != "google") {
        options.switches.push_back("disable-blink-features=PushMessaging");
      }
      // Payment Request is a `payments` request (T-UIR-14); with the surface
      // disabled, pages have no PaymentRequest. Chromium's shortcut that pays
      // with a single app without showing the sheet is off, so a page never
      // pays without the person choosing (ui-requests.md 5.22).
      options.switches.push_back(
          bdkcore::ui_surface_available(rt, BDK_SURFACE_PAYMENTS)
              ? "disable-features=WebPaymentsSingleAppUiSkip"
              : "disable-features=WebPaymentsSingleAppUiSkip,WebPayments");
      for (std::string& extra : ExtraSwitches()) {
        options.switches.push_back(std::move(extra));
      }
      InstallReputationService();
      // The branded scheme is registered in every process as Chromium starts
      // (the scheme patch reads the switch; the browser client copies it to
      // each child). Chromium starts once per process, so a later runtime of
      // the same process keeps the first runtime's registration.
      const std::string scheme = InternalSchemeOf(resolved);
      if (!scheme.empty()) {
        options.switches.push_back(std::string(kInternalSchemeSwitch) + "=" +
                                   scheme);
      }
      std::string error;
      if (!StartChromium(options, &error)) {
        return bdkcore::fail(out_error, BDK_DOMAIN_RUNTIME, "startFailed",
                             error);
      }
      ApplyIdentityAfterStart(*identity);
      ApplyLocalStatePrivacyPrefs();
      InstallPromptHooks(bdkcore::webauthn_phones_allowed(rt));
      InstallSelectFileDialogs();
      InstallScreenCaptureUi();
      InstallWebAppHooks();
      InstallPaymentHooks();
      chromium_tasks_.store(true);
      registry_writer_ = base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
    }
    SetRuntimeInfo(resolved);
    SetPageProtocolRuntimeInfo(resolved);
    ConfigureDevTools(resolved,
                      base::FilePath(bdkcore::str(config.bundle_path)));
    // Each runtime reads scripts from its own application's bundle.
    automation_host_ = std::make_unique<AutomationHostLauncher>(
        base::FilePath(bdkcore::str(config.bundle_path)));
    SetPagesBundlePath(base::FilePath(bdkcore::str(config.bundle_path)));
    RegisterBundledPageUIs(rt);
    // The profile Chromium started with. A runtime started in a Chromium that
    // is still running (BDK_ENGINE_KEEP_CHROMIUM) keeps it: the last used
    // profile may by then be one BDK opened and closed, which is unloaded.
    if (!base_profile_) {
      base_profile_ = ProfileManager::GetLastUsedProfileIfLoaded();
    }
    if (!base_profile_) {
      return bdkcore::fail(out_error, BDK_DOMAIN_RUNTIME, "startFailed",
                           "Chromium loaded no profile");
    }
    rt.profiles.clear();
    rt.next_id = registry_.next_id;
    for (const auto& [id, name] : registry_.profiles) {
      auto p = std::make_shared<bdkcore::Profile>();
      p->id = id;
      p->name = name;
      rt.profiles.push_back(std::move(p));
    }
    // Chromium's after-start-up work may run from the next idle on.
    RuntimeStarted();
    return BDK_OK;
  }

  void Shutdown(Runtime& rt, std::function<void()> done) override {
    // The core ended every task (automation_shutdown); this drops the host.
    automation_host_.reset();
    // The core has made every handle stale; what is left goes now.
    for (auto& c : rt.contents) {
      c->data.reset();
    }
    rt.contents.clear();
    // Every page is gone: the contexts that held them go before the profiles.
    HostContexts::Get().DestroyAll();
    for (auto& p : rt.profiles) {
      if (p->data) {
        ReleaseProfile(*p);
      }
    }
    if (!base::Environment::Create()
             ->GetVar(kKeepChromiumEnv)
             .value_or("")
             .empty()) {
      // Chromium stays, but nothing of this runtime's profiles does.
      while (!loaded_.empty()) {
        UnloadWhenClosed(loaded_.begin()->first);
      }
      done();
      return;
    }
    chromium_tasks_.store(false);
    // Chromium destroys its profiles as it stops.
    loaded_.clear();
    base_profile_ = nullptr;
    StopChromium(
        base::BindOnce([](std::function<void()> d) { d(); }, std::move(done)));
  }

  std::string ManifestJson() override {
    return ToJson(base::DictValue()
                      .Set("bdk", "0.0.0")
                      .Set("chromium", version_info::GetVersionNumber())
                      .Set("digest", base::Value())
                      .Set("abiVersion", BDK_ABI_VERSION)
                      .Set("abiHash", BDK_ABI_HASH));
  }

  // Built-in commands, service by service (engine/services): passwords and
  // autofill first, then the rest.
  void ExecuteCommand(
      Runtime& rt,
      const std::shared_ptr<bdkcore::BuiltinCall>& call) override {
    if (ExecutePasswordsOrAutofillCommand(rt, call)) {
      return;
    }
    ExecuteBuiltinCommand(rt, call);
  }

  void ProfilesChanged(Runtime& rt) override {
    base::ListValue profiles;
    registry_.next_id = rt.next_id;
    registry_.profiles.clear();
    for (const auto& p : rt.profiles) {
      if (!p->ephemeral) {
        registry_.profiles.emplace_back(p->id, p->name);
        profiles.Append(
            base::DictValue().Set("id", p->id).Set("name", p->name));
      }
    }
    std::string text = base::WriteJsonWithOptions(
                           base::DictValue()
                               .Set("nextId", base::NumberToString(rt.next_id))
                               .Set("profiles", std::move(profiles)),
                           base::JSONWriter::OPTIONS_PRETTY_PRINT)
                           .value_or("{}");
    registry_writer_->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::FilePath path, std::string contents) {
              base::WriteFile(path, contents);
            },
            user_data_dir_.AppendASCII(kRegistryFile), std::move(text)));
  }

  void OpenProfile(Runtime& rt,
                   const std::shared_ptr<bdkcore::Profile>& profile,
                   Done done) override {
    LoadProfile(profile, std::move(done));
  }

  void LoadProfile(const std::shared_ptr<bdkcore::Profile>& profile,
                   Done done) {
    const base::FilePath path = user_data_dir_.AppendASCII(profile->id);
    if (IsUnloading(path)) {
      NoteTaskPosted();
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(
              [](EngineBackend* backend,
                 const std::shared_ptr<bdkcore::Profile>& profile, Done done) {
                NoteTaskRun();
                backend->LoadProfile(profile, std::move(done));
              },
              base::Unretained(this), profile, std::move(done)),
          kUnloadPoll);
      return;
    }
    std::weak_ptr<bdkcore::Profile> weak = profile;
    // The open counts as pending work until its result is delivered: the
    // profile loads asynchronously and its settings file is read on another
    // sequence, neither of which is a BDK task on the UI thread.
    NoteTaskPosted();
    g_browser_process->profile_manager()->CreateProfileAsync(
        path, base::BindOnce(
                  [](EngineBackend* backend, base::FilePath path,
                     std::weak_ptr<bdkcore::Profile> weak, Done done,
                     Profile* loaded) {
                    std::shared_ptr<bdkcore::Profile> p = weak.lock();
                    if (!loaded) {
                      NoteTaskRun();
                      done("openFailed", "Chromium could not load the profile");
                      return;
                    }
                    if (!p || !p->open) {
                      // Loaded for nobody: let it unload, as a closed one.
                      backend->KeepLoaded(path, loaded);
                      backend->UnloadWhenClosed(path);
                      NoteTaskRun();
                      done("openFailed", "the profile was closed");
                      return;
                    }
                    backend->KeepLoaded(path, loaded);
                    // Web notifications become UI requests
                    // (ui_notifications.h).
                    InstallNotificationDisplay(loaded);
                    InstallDownloadPrompts(loaded);
                    ApplyPrivacyPrefs(loaded);
                    auto data = std::make_unique<EngineProfile>();
                    data->profile = loaded;
                    data->events = CreateServiceEvents(loaded, p);
                    p->data = std::move(data);
                    // The application's own extensions load before the
                    // profile is handed over, so they run before its first
                    // navigation (extensions.md 9).
                    LoadBundledExtensions(
                        loaded,
                        base::BindOnce(
                            [](EngineBackend* backend,
                               std::shared_ptr<bdkcore::Profile> p, Done done) {
                              backend->LoadAppSettingsThenDone(p,
                                                               std::move(done));
                            },
                            base::Unretained(backend), p, std::move(done)));
                  },
                  base::Unretained(this), path, weak, std::move(done)));
  }

  bdk_status_t OpenPrivateProfile(
      Runtime& rt,
      const std::shared_ptr<bdkcore::Profile>& profile,
      bdk_error_t* out_error) override {
    // Each private profile is its own off-the-record profile: cookies, cache,
    // storage and permissions are kept apart from every other profile, and
    // nothing is written to disk.
    Profile* otr = base_profile_->GetOffTheRecordProfile(
        Profile::OTRProfileID::CreateUnique("BDK::Private"),
        /*create_if_needed=*/true);
    if (!otr) {
      return bdkcore::fail(out_error, BDK_DOMAIN_PROFILE, "openFailed",
                           "Chromium could not create a private profile");
    }
    InstallDownloadPrompts(otr);
    ApplyPrivacyPrefs(otr);
    auto data = std::make_unique<EngineProfile>();
    data->profile = otr;
    // Cookie and download events; ProfileEvents leaves out the history and
    // bookmarks an off-the-record profile shares with its original.
    data->events = CreateServiceEvents(otr, profile);
    profile->data = std::move(data);
    return BDK_OK;
  }

  void CloseProfile(Runtime& rt, bdkcore::Profile& profile) override {
    // The profile's pages are destroyed; the contexts that held them go before
    // the profile may.
    if (profile.data) {
      HostContexts::Get().DestroyProfileContexts(ProfileData(profile).profile);
      UnloadWhenClosed(user_data_dir_.AppendASCII(profile.id));
    }
    ReleaseProfile(profile);
  }

  std::string LoadAppSettings(Runtime& rt,
                              const bdkcore::Profile* profile) override {
    auto it = app_settings_text_.find(profile ? profile->id : std::string());
    return it == app_settings_text_.end() ? std::string() : it->second;
  }

  void SaveAppSettings(Runtime& rt,
                       const bdkcore::Profile* profile,
                       const std::string& json) override {
    app_settings_text_[profile ? profile->id : std::string()] = json;
    registry_writer_->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::FilePath path, std::string json) {
                         base::ImportantFileWriter::WriteFileAtomically(path,
                                                                        json);
                       },
                       AppSettingsPath(profile), json));
  }

  // Reads the profile's application settings off the UI thread, then opens it.
  void LoadAppSettingsThenDone(const std::shared_ptr<bdkcore::Profile>& profile,
                               Done done) {
    registry_writer_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&ReadAppSettings, AppSettingsDir(profile.get())),
        base::BindOnce(
            [](EngineBackend* backend, std::string id, Done done,
               std::string text) {
              backend->app_settings_text_[id] = std::move(text);
              NoteTaskRun();
              done("", "");
            },
            base::Unretained(this), profile->id, std::move(done)));
  }

  void DeleteProfile(Runtime& rt,
                     const bdkcore::Profile& profile,
                     Done done) override {
    const base::FilePath path = user_data_dir_.AppendASCII(profile.id);
    // Chromium unloads it as part of the deletion.
    loaded_.erase(path);
    ProfileManager* manager = g_browser_process->profile_manager();
    // A profile that was never opened has nothing on disk and is unknown to
    // Chromium.
    if (manager->GetProfileAttributesStorage().GetProfileAttributesWithPath(
            path)) {
      manager->GetDeleteProfileHelper().MaybeScheduleProfileForDeletion(
          path, base::DoNothing(), ProfileMetrics::DELETE_PROFILE_SETTINGS);
    }
    done("", "");
  }

  bdk_status_t CreateContents(Runtime& rt,
                              const std::shared_ptr<Contents>& c,
                              const std::string& initial_url,
                              bdk_error_t* out_error) override {
    Profile* profile = ProfileData(*c->profile).profile;
    if (!profile) {
      return bdkcore::fail(out_error, BDK_DOMAIN_PROFILE, "notOpen",
                           "the profile is not loaded yet");
    }
    if (!HostContexts::CanHost(profile)) {
      return bdkcore::fail(out_error, BDK_DOMAIN_PROFILE, "notOpen",
                           "Chromium cannot open a page in this profile");
    }
    // With its renderer from the start: a page can take focus and receive
    // input before its first navigation, as the stub's pages can.
    content::WebContents::CreateParams params(profile);
    params.desired_renderer_state =
        content::WebContents::CreateParams::kInitializeAndWarmupRendererProcess;
    c->data = std::make_unique<EngineContents>(
        c, content::WebContents::Create(params));
    if (!initial_url.empty()) {
      Page(*c).Load(initial_url);
    }
    return BDK_OK;
  }

  void CloseContents(const std::shared_ptr<Contents>& c) override {
    Page(*c).Close();
  }

  void StartTask(Runtime& rt,
                 const std::shared_ptr<bdkcore::TaskPort>& task) override {
    if (!automation_host_) {
      bdkcore::Backend::StartTask(rt, task);  // finishes it: no host
      return;
    }
    automation_host_->StartTask(rt, task);
  }

  void EndTask(Runtime& rt, bdkcore::TaskPort& task) override {
    if (automation_host_) {
      automation_host_->EndTask(task);
    }
  }

  void UserScriptsChanged(
      Runtime& rt,
      bdkcore::Profile& profile,
      const std::vector<bdkcore::UserScript>& scripts) override {
    // Scripts are registered on an open profile; a closed one has no pages.
    auto* data = static_cast<EngineProfile*>(profile.data.get());
    if (data && data->profile) {
      ApplyUserScripts(data->profile, scripts);
    }
  }

  void CallerRevoked(Runtime& rt, const bdkcore::Caller& caller) override {
    DetachDebugClientsOf(caller);
  }

  void Authenticate(Runtime& rt,
                    const std::string& reason,
                    std::function<void(bool)> done) override {
    AuthenticateUser(
        reason, base::BindOnce([](std::function<void(bool)> done,
                                  bool authenticated) { done(authenticated); },
                               std::move(done)));
  }

  void DestroyContents(Runtime& rt, Contents& c) override { c.data.reset(); }

  bdk_status_t Load(const std::shared_ptr<Contents>& c,
                    const std::string& url,
                    bdk_error_t* out_error) override {
    GURL gurl(url);
    if (!gurl.is_valid()) {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    Page(*c).Load(url);
    return BDK_OK;
  }

  bdk_status_t Reload(const std::shared_ptr<Contents>& c,
                      bool bypass_cache,
                      bdk_error_t* out_error) override {
    Page(*c).LoadIfDiscarded();
    Page(*c).web_contents()->GetController().Reload(
        bypass_cache ? content::ReloadType::BYPASSING_CACHE
                     : content::ReloadType::NORMAL,
        /*check_for_repost=*/false);
    return BDK_OK;
  }

  void Stop(const std::shared_ptr<Contents>& c) override {
    Page(*c).web_contents()->Stop();
  }

  bdk_status_t GoToIndex(const std::shared_ptr<Contents>& c,
                         int index,
                         bdk_error_t* out_error) override {
    Page(*c).LoadIfDiscarded();
    Page(*c).web_contents()->GetController().GoToIndex(index);
    return BDK_OK;
  }

  void Entries(const Contents& c,
               int* current,
               std::vector<bdkcore::Entry>* entries) override {
    content::NavigationController& controller =
        Page(c).web_contents()->GetController();
    entries->clear();
    for (int i = 0; i < controller.GetEntryCount(); ++i) {
      content::NavigationEntry* entry = controller.GetEntryAtIndex(i);
      if (entry->IsInitialEntry()) {
        continue;
      }
      entries->push_back(
          bdkcore::Entry{DisplayUrl(entry->GetURL()),
                         base::UTF16ToUTF8(entry->GetTitleForDisplay())});
    }
    *current = entries->empty() ? -1 : controller.GetLastCommittedEntryIndex();
  }

  void State(const Contents& c, bdkcore::ContentsState* state) override {
    EngineContents& page = Page(c);
    content::WebContents* contents = page.web_contents();
    const GURL url = contents->GetLastCommittedURL();
    state->url = url.is_valid() ? DisplayUrl(url) : "about:blank";
    content::NavigationEntry* entry =
        contents->GetController().GetLastCommittedEntry();
    state->title = entry && !entry->IsInitialEntry()
                       ? base::UTF16ToUTF8(contents->GetTitle())
                       : "";
    state->loading = contents->IsLoading();
    state->progress = contents->IsLoading() ? contents->GetLoadProgress() : 1.0;
    if (!entry || entry->IsInitialEntry()) {
      state->progress = 0.0;
    }
    if (url.SchemeIs(url::kHttpsScheme)) {
      const bool broken =
          entry && net::IsCertStatusError(entry->GetSSL().cert_status);
      state->security = broken ? BDK_SECURITY_BROKEN : BDK_SECURITY_SECURE;
    } else if (url.SchemeIs(url::kHttpScheme)) {
      state->security = BDK_SECURITY_NONE;
    } else {
      state->security = BDK_SECURITY_UNKNOWN;
    }
    state->audio = (contents->IsCurrentlyAudible() ? BDK_AUDIO_AUDIBLE : 0u) |
                   (contents->IsAudioMuted() ? BDK_AUDIO_MUTED : 0u);
    state->capture = CaptureFlags(contents);
    state->visibility = contents->GetVisibility() == content::Visibility::HIDDEN
                            ? BDK_VISIBILITY_HIDDEN
                            : BDK_VISIBILITY_VISIBLE;
    // As the focus events report it, so state and events agree.
    state->focused = page.focused();
    state->zoom = page.Zoom();
  }

  std::string SessionState(const Contents& c) override {
    return SerializeNavigation(Page(c).web_contents()->GetController());
  }

  bdk_status_t Restore(const std::shared_ptr<Contents>& c,
                       bdk_bytes_t state,
                       bdk_error_t* out_error) override {
    EngineContents& page = Page(*c);
    content::BrowserContext* context = page.web_contents()->GetBrowserContext();
    RestoredNavigation restored;
    switch (ParseNavigation(
        // SAFETY: the ABI lends `state.length` bytes at `state.data`.
        UNSAFE_BUFFERS(base::span<const uint8_t>(state.data, state.length)),
        context, &restored)) {
      case ParseResult::kOk:
        break;
      case ParseResult::kTooNew:
        return bdkcore::fail(out_error, BDK_DOMAIN_CONTENTS, "stateTooNew",
                             "the session state is from a newer format");
      case ParseResult::kMalformed:
        return BDK_ERR_INVALID_ARGUMENT;
    }
    // A NavigationController can be restored only before its first use, so
    // the page gets a new WebContents; the handle and the view stay.
    std::unique_ptr<content::WebContents> fresh = content::WebContents::Create(
        content::WebContents::CreateParams(context));
    const bool has_entries = !restored.entries.empty();
    if (has_entries) {
      fresh->GetController().Restore(
          restored.current, content::RestoreType::kRestored, &restored.entries);
    }
    page.ReplaceWebContents(std::move(fresh));
    if (has_entries) {
      page.web_contents()->GetController().LoadIfNecessary();
    }
    return BDK_OK;
  }

  bdk_status_t SetMuted(Contents& c,
                        bool muted,
                        bdk_error_t* out_error) override {
    Page(c).SetMuted(muted);
    return BDK_OK;
  }

  bdk_status_t SetZoom(Contents& c,
                       double zoom,
                       uint32_t scope,
                       bdk_error_t* out_error) override {
    SetPageZoom(Page(c), zoom, scope);
    return BDK_OK;
  }

  bdk_status_t SetUserAgent(Contents& c,
                            const std::string& user_agent,
                            bdk_error_t* out_error) override {
    Page(c).SetUserAgent(user_agent);
    return BDK_OK;
  }

  bdk_status_t Discard(Contents& c, bdk_error_t* out_error) override {
    Page(c).Discard();
    return BDK_OK;
  }

  bdk_status_t SetAutoDiscardable(Contents& c,
                                  bool discardable,
                                  bdk_error_t* out_error) override {
    Page(c).set_auto_discardable(discardable);
    return BDK_OK;
  }

  bdk_status_t FindStart(const std::shared_ptr<Contents>& c,
                         const std::string& text,
                         bool match_case,
                         bdk_error_t* out_error) override {
    Page(*c).find().Start(text, match_case);
    return BDK_OK;
  }

  bdk_status_t FindNext(const std::shared_ptr<Contents>& c,
                        bool forward,
                        bdk_error_t* out_error) override {
    Page(*c).find().Next(forward);
    return BDK_OK;
  }

  bdk_status_t FindStop(const std::shared_ptr<Contents>& c,
                        bool clear_selection,
                        bdk_error_t* out_error) override {
    Page(*c).find().Stop(clear_selection);
    return BDK_OK;
  }

  bdk_status_t FindPresent(const std::shared_ptr<Contents>& c,
                           bdk_error_t* out_error) override {
    Page(*c).find().Present();
    return BDK_OK;
  }

  bdk_status_t Print(const std::shared_ptr<Contents>& c,
                     bdk_error_t* out_error) override {
    // The path the print command takes: a `printing` request, then the
    // application's choice or the default presenter.
    OnPrintRequested(Page(*c).web_contents(), /*is_user_initiated=*/true);
    return BDK_OK;
  }

  bool CanPerformCommand(const std::shared_ptr<Contents>& c,
                         uint32_t command) override {
    return CanPerformPageCommand(Page(*c), command);
  }

  bdk_status_t PerformCommand(const std::shared_ptr<Contents>& c,
                              uint32_t command,
                              bdk_error_t* out_error) override {
    switch (command) {
      case BDK_PAGE_COMMAND_FIND:
        return FindPresent(c, out_error);
      case BDK_PAGE_COMMAND_FIND_NEXT:
      case BDK_PAGE_COMMAND_FIND_PREVIOUS:
        return FindNext(c, command == BDK_PAGE_COMMAND_FIND_NEXT, out_error);
      case BDK_PAGE_COMMAND_PRINT:
        return Print(c, out_error);
      default:
        Page(*c).LoadIfDiscarded();
        PerformPageCommand(Page(*c), command);
        return BDK_OK;
    }
  }

  bdk_status_t ExitPictureInPicture(const std::shared_ptr<Contents>& c,
                                    bdk_error_t* out_error) override {
    ExitPageFromPictureInPicture(Page(*c));
    return BDK_OK;
  }

  bdk_status_t NativeView(Contents& c,
                          bdk_native_handle_t* out,
                          bdk_error_t* out_error) override {
    *out = Page(c).host_view().NativeHandle();
    return BDK_OK;
  }

  bdk_status_t Focus(Runtime& rt, const std::shared_ptr<Contents>& c) override {
    Page(*c).Focus();
    return BDK_OK;
  }

  bdk_status_t RegisterWindow(Runtime& rt,
                              const bdkcore::Window& window,
                              bdk_error_t* out_error) override {
    HostContexts::Get().RegisterWindow(&window, window.native);
    // Pages whose views are already in the window move into its contexts.
    for (EngineContents* page : EngineContents::All()) {
      page->Reconcile();
    }
    return BDK_OK;
  }

  // Extension actions and the default menu (extensions/action_service.h).
  bdk_status_t ExtensionActivateAction(Runtime& rt,
                                       const std::shared_ptr<Contents>& c,
                                       const std::string& extension_id,
                                       bdk_native_handle_t gesture,
                                       std::string* result_json,
                                       bdk_error_t* out_error) override {
    return ActivateExtensionAction(c, extension_id, gesture, result_json,
                                   out_error);
  }

  bdk_status_t ExtensionPerformCommand(Runtime& rt,
                                       const std::shared_ptr<Contents>& c,
                                       const std::string& extension_id,
                                       const std::string& command,
                                       bdk_native_handle_t gesture,
                                       std::string* result_json,
                                       bdk_error_t* out_error) override {
    return PerformExtensionCommand(c, extension_id, command, gesture,
                                   result_json, out_error);
  }

  bdk_status_t ExtensionPresentMenu(Runtime& rt,
                                    const bdkcore::Window& window,
                                    bdk_native_handle_t gesture,
                                    bdk_error_t* out_error) override {
    if (!UiPresenters::Get().IsRecentGesture(gesture)) {
      return bdkcore::fail(out_error, BDK_DOMAIN_UI, BDK_CODE_GESTURE_REQUIRED,
                           "the extensions menu opens for the event that asked "
                           "for it, delivered recently to one of the "
                           "application's windows");
    }
    std::shared_ptr<bdkcore::Window> shared =
        RegisteredWindowOf(rt, window.native);
    std::string error;
    if (!shared ||
        !PresentExtensionsMenu(rt, shared, bdk_native_handle_t{}, &error)) {
      return bdkcore::fail(out_error, BDK_DOMAIN_EXTENSION, BDK_CODE_NOT_FOUND,
                           shared ? error : "the window is not registered");
    }
    return BDK_OK;
  }

  void UnregisterWindow(const bdkcore::Window& window) override {
    // The window's pages go to parking, then its contexts close.
    for (EngineContents* page : EngineContents::All()) {
      if (page->IsInWindow(&window)) {
        page->Park();
      }
    }
    HostContexts::Get().UnregisterWindow(&window);
  }

  // The attachments of bdk_window.h, which the window registry keeps.
  bdk_window_t WindowForContents(const Contents& c) override {
    return WindowRegistry::Get().WindowOfPage(c.handle);
  }

  void WindowContents(const bdkcore::Window& window,
                      std::vector<bdk_contents_t>* contents,
                      bdk_contents_t* active) override {
    *contents = WindowRegistry::Get().PagesOf(&window);
    *active = 0;
    HostContext* context = WindowRegistry::Get().ActiveContext(&window);
    Browser* browser = context ? context->browser() : nullptr;
    if (!browser) {
      return;
    }
    if (EngineContents* page = EngineContents::FromWebContents(
            browser->GetTabStripModel()->GetActiveWebContents())) {
      *active = page->handle();
    }
  }

  // The window id extensions see (windows.get) is the session identifier of
  // the context of the window's active page: a window is one extension window
  // per profile.
  int32_t WindowExtensionId(const bdkcore::Window& window) override {
    HostContext* context = WindowRegistry::Get().ActiveContext(&window);
    Browser* browser = context ? context->browser() : nullptr;
    return browser ? extensions::ExtensionTabUtil::GetWindowId(browser) : -1;
  }

  // Keeps a persistent profile loaded for the life of the runtime, from its
  // first open. Closing it in BDK does not unload it in Chromium: an unload
  // runs in the background, and opening the profile again meanwhile would
  // give Chromium a second instance on the same files, whose databases the
  // first still holds.
  void KeepLoaded(const base::FilePath& path, Profile* profile) {
    if (!loaded_.contains(path)) {
      loaded_[path] = {profile->GetWeakPtr(),
                       std::make_unique<ScopedProfileKeepAlive>(
                           profile, ProfileKeepAliveOrigin::kBrowserWindow)};
    }
  }

  // Lets Chromium unload a persistent profile that BDK has closed. Its
  // destruction is asynchronous, so the path is remembered until the Profile
  // object is gone (IsUnloading); without this every profile a runtime ever
  // opened would stay loaded, with its databases and file descriptors, until
  // the runtime stopped.
  void UnloadWhenClosed(const base::FilePath& path) {
    auto it = loaded_.find(path);
    if (it == loaded_.end()) {
      return;
    }
    unloading_[path] = it->second.profile;
    loaded_.erase(it);
  }

  // True while Chromium has unloaded the profile at `path` but not yet
  // destroyed it. A profile Chromium still holds loaded (another keep-alive)
  // is not unloading: opening it again gets the same instance.
  bool IsUnloading(const base::FilePath& path) {
    auto it = unloading_.find(path);
    if (it == unloading_.end()) {
      return false;
    }
    if (!it->second ||
        g_browser_process->profile_manager()->GetProfileByPath(path)) {
      unloading_.erase(it);
      return false;
    }
    return true;
  }

  // For bdk_testing_forget_profiles().
  void ForgetProfiles() { registry_.profiles.clear(); }

 private:
  void ReleaseProfile(bdkcore::Profile& profile) {
    if (!profile.data) {
      return;
    }
    // Let go of every pointer to the profile before Chromium may destroy it.
    Profile* chromium_profile = ProfileData(profile).profile;
    profile.data.reset();
    if (profile.ephemeral && chromium_profile) {
      ProfileDestroyer::DestroyOTRProfileWhenAppropriate(chromium_profile);
    }
  }

  base::FilePath AppSettingsDir(const bdkcore::Profile* profile) const {
    return profile ? user_data_dir_.AppendASCII(profile->id) : user_data_dir_;
  }

  base::FilePath AppSettingsPath(const bdkcore::Profile* profile) const {
    return AppSettingsDir(profile).AppendASCII(kAppSettingsFile);
  }

  static std::string ReadAppSettings(const base::FilePath& dir) {
    std::string text;
    base::ReadFileToString(dir.AppendASCII(kAppSettingsFile), &text);
    return text;
  }

  struct Registry {
    uint64_t next_id = 1;
    std::vector<std::pair<std::string, std::string>> profiles;  // id, name
  };

  // The data directory a start names (the launch option), or the [storage]
  // directory in the application data directory; empty when that is unknown.
  static base::FilePath UserDataDirectory(const bdk_start_config_t& config,
                                          const Identity& identity) {
    const std::string override_path =
        bdkcore::str(config.user_data_path_override);
    if (!override_path.empty()) {
      return base::FilePath(override_path);
    }
    base::FilePath app_data;
    if (!base::PathService::Get(base::DIR_APP_DATA, &app_data)) {
      return base::FilePath();
    }
    return app_data.Append(identity.storage_directory);
  }

  static Registry ReadRegistry(const base::FilePath& user_data_dir) {
    Registry registry;
    std::string text;
    if (!base::ReadFileToString(user_data_dir.AppendASCII(kRegistryFile),
                                &text)) {
      return registry;
    }
    std::optional<base::DictValue> dict = ParseJsonDict(text);
    if (!dict) {
      return registry;
    }
    if (const std::string* next = dict->FindString("nextId")) {
      uint64_t value = 1;
      if (base::StringToUint64(*next, &value)) {
        registry.next_id = value;
      }
    }
    if (const base::ListValue* list = dict->FindList("profiles")) {
      for (const base::Value& v : *list) {
        const base::DictValue* d = v.GetIfDict();
        const std::string* id = d ? d->FindString("id") : nullptr;
        const std::string* name = d ? d->FindString("name") : nullptr;
        if (id && name) {
          registry.profiles.emplace_back(*id, *name);
        }
      }
    }
    return registry;
  }

  std::atomic<bool> chromium_tasks_{false};
  base::FilePath user_data_dir_;
  // As the application gave it, before symbolic links are resolved.
  base::FilePath requested_user_data_dir_;
  raw_ptr<Profile> base_profile_ = nullptr;
  scoped_refptr<base::SequencedTaskRunner> registry_writer_;
  // The automation host: V8 in a utility process, started with the first task.
  std::unique_ptr<AutomationHostLauncher> automation_host_;
  // The text of each scope's application settings file, by profile id ("" for
  // the application's).
  std::map<std::string, std::string> app_settings_text_;
  Registry registry_;
  struct LoadedProfile {
    base::WeakPtr<Profile> profile;
    std::unique_ptr<ScopedProfileKeepAlive> keep_alive;
  };
  std::map<base::FilePath, LoadedProfile> loaded_;
  // Persistent profiles BDK closed that Chromium is still destroying.
  std::map<base::FilePath, base::WeakPtr<Profile>> unloading_;
};

}  // namespace

}  // namespace bdk

namespace bdkcore {

Backend& backend() {
  static base::NoDestructor<bdk::EngineBackend> backend;
  return *backend;
}

}  // namespace bdkcore

namespace bdk {

void ForgetPersistentProfilesForTesting() {
  static_cast<EngineBackend&>(bdkcore::backend()).ForgetProfiles();
}

}  // namespace bdk
