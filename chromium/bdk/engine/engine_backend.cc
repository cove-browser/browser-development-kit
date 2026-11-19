// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The Chromium backend of the runtime core (packages/core/runtime): what a
// profile and a page are when Chromium runs them. The core keeps the handles,
// the checks and the bookkeeping, exactly as for the stub runtime.

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "bdk/core/generated/bdk_abi_hash.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/chromium_main.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/identity.h"
#include "bdk/engine/session_state.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/delete_profile_helper.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_metrics.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/ssl_status.h"
#include "content/public/browser/web_contents.h"
#include "net/cert/cert_status_flags.h"
#include "url/gurl.h"

namespace bdk {

namespace {

using bdkcore::Contents;
using bdkcore::Done;
using bdkcore::Runtime;

// Lists the persistent profiles and the next free identifier number, in the
// user data directory, next to Chromium's own Local State.
constexpr char kRegistryFile[] = "BDK Profiles.json";

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
    auto run =
        base::BindOnce([](std::function<void()> t) { t(); }, std::move(task));
    if (chromium_tasks_.load()) {
      content::GetUIThreadTaskRunner({})->PostTask(FROM_HERE, std::move(run));
    } else {
      PostToMainThread(std::move(run));
    }
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
    const std::string override_path =
        bdkcore::str(config.user_data_path_override);
    base::FilePath user_data_dir;
    if (!override_path.empty()) {
      user_data_dir = base::FilePath(override_path);
    } else {
      base::FilePath app_data;
      if (!base::PathService::Get(base::DIR_APP_DATA, &app_data)) {
        return bdkcore::fail(out_error, BDK_DOMAIN_RUNTIME, "noDataDirectory",
                             "the application data directory is unknown");
      }
      user_data_dir = app_data.Append(identity->storage_directory);
    }
    if (IsChromiumRunning()) {
      if (user_data_dir != user_data_dir_) {
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
      user_data_dir_ = user_data_dir;
      // The UI thread may not block once Chromium runs: read the registry now
      // and keep it in memory from here on.
      registry_ = ReadRegistry(user_data_dir_);
      ApplyIdentityBeforeStart(*identity);
      ChromiumStartOptions options;
      base::PathService::Get(base::FILE_EXE, &options.program);
      options.user_data_dir = user_data_dir;
      options.switches = ExtraSwitches();
      std::string error;
      if (!StartChromium(options, &error)) {
        return bdkcore::fail(out_error, BDK_DOMAIN_RUNTIME, "startFailed",
                             error);
      }
      ApplyIdentityAfterStart(*identity);
      chromium_tasks_.store(true);
      registry_writer_ = base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
    }
    base_profile_ = ProfileManager::GetLastUsedProfileIfLoaded();
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
    return BDK_OK;
  }

  void Shutdown(Runtime& rt, std::function<void()> done) override {
    // The core has made every handle stale; what is left goes now.
    for (auto& c : rt.contents) {
      c->data.reset();
    }
    rt.contents.clear();
    for (auto& p : rt.profiles) {
      if (p->data) {
        ReleaseProfile(*p);
      }
    }
    if (!base::Environment::Create()
             ->GetVar(kKeepChromiumEnv)
             .value_or("")
             .empty()) {
      done();
      return;
    }
    chromium_tasks_.store(false);
    // Chromium destroys its profiles as it stops.
    base_profile_ = nullptr;
    StopChromium(
        base::BindOnce([](std::function<void()> d) { d(); }, std::move(done)));
  }

  std::string ManifestJson() override {
    return base::WriteJson(
               base::DictValue()
                   .Set("bdk", "0.0.0")
                   .Set("chromium", version_info::GetVersionNumber())
                   .Set("digest", base::Value())
                   .Set("abiVersion", BDK_ABI_VERSION)
                   .Set("abiHash", BDK_ABI_HASH))
        .value_or("{}");
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
    const base::FilePath path = user_data_dir_.AppendASCII(profile->id);
    std::weak_ptr<bdkcore::Profile> weak = profile;
    g_browser_process->profile_manager()->CreateProfileAsync(
        path, base::BindOnce(
                  [](std::weak_ptr<bdkcore::Profile> weak, Done done,
                     Profile* loaded) {
                    std::shared_ptr<bdkcore::Profile> p = weak.lock();
                    if (!loaded) {
                      done("openFailed", "Chromium could not load the profile");
                      return;
                    }
                    if (!p || !p->open) {
                      done("openFailed", "the profile was closed");
                      return;
                    }
                    auto data = std::make_unique<EngineProfile>();
                    data->profile = loaded;
                    data->keep_alive = std::make_unique<ScopedProfileKeepAlive>(
                        loaded, ProfileKeepAliveOrigin::kBrowserWindow);
                    p->data = std::move(data);
                    done("", "");
                  },
                  weak, std::move(done)));
  }

  bdk_status_t OpenPrivateProfile(Runtime& rt,
                                  bdkcore::Profile& profile,
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
    auto data = std::make_unique<EngineProfile>();
    data->profile = otr;
    profile.data = std::move(data);
    return BDK_OK;
  }

  void CloseProfile(Runtime& rt, bdkcore::Profile& profile) override {
    ReleaseProfile(profile);
  }

  void DeleteProfile(Runtime& rt,
                     const bdkcore::Profile& profile,
                     Done done) override {
    const base::FilePath path = user_data_dir_.AppendASCII(profile.id);
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
    c->data = std::make_unique<EngineContents>(
        c, content::WebContents::Create(
               content::WebContents::CreateParams(profile)));
    if (!initial_url.empty()) {
      Page(*c).Load(initial_url);
    }
    return BDK_OK;
  }

  void CloseContents(const std::shared_ptr<Contents>& c) override {
    Page(*c).Close();
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
          bdkcore::Entry{entry->GetURL().spec(),
                         base::UTF16ToUTF8(entry->GetTitleForDisplay())});
    }
    *current = entries->empty() ? -1 : controller.GetLastCommittedEntryIndex();
  }

  void State(const Contents& c, bdkcore::ContentsState* state) override {
    EngineContents& page = Page(c);
    content::WebContents* contents = page.web_contents();
    const GURL url = contents->GetLastCommittedURL();
    state->url = url.is_valid() ? url.spec() : "about:blank";
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
    state->visibility = contents->GetVisibility() == content::Visibility::HIDDEN
                            ? BDK_VISIBILITY_HIDDEN
                            : BDK_VISIBILITY_VISIBLE;
    content::RenderWidgetHostView* view = contents->GetRenderWidgetHostView();
    state->focused = view && view->HasFocus();
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
    Page(c).web_contents()->SetAudioMuted(muted);
    return BDK_OK;
  }

  bdk_status_t SetZoom(Contents& c,
                       double zoom,
                       bdk_error_t* out_error) override {
    Page(c).SetZoom(zoom);
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

  bdk_status_t NativeView(Contents& c,
                          bdk_native_handle_t* out,
                          bdk_error_t* out_error) override {
    *out = Page(c).host_view().NativeHandle();
    return BDK_OK;
  }

  bdk_status_t Focus(Runtime& rt, const std::shared_ptr<Contents>& c) override {
    Page(*c).LoadIfDiscarded();
    Page(*c).web_contents()->Focus();
    return BDK_OK;
  }

  bdk_status_t RegisterWindow(Runtime& rt,
                              const bdkcore::Window& window,
                              bdk_error_t* out_error) override {
    return BDK_OK;
  }

  void UnregisterWindow(const bdkcore::Window& window) override {}

 private:
  void ReleaseProfile(bdkcore::Profile& profile) {
    if (!profile.data) {
      return;
    }
    EngineProfile& data = ProfileData(profile);
    if (profile.ephemeral && data.profile) {
      ProfileDestroyer::DestroyOTRProfileWhenAppropriate(data.profile);
    }
    data.profile = nullptr;
    profile.data.reset();
  }

  struct Registry {
    uint64_t next_id = 1;
    std::vector<std::pair<std::string, std::string>> profiles;  // id, name
  };

  static Registry ReadRegistry(const base::FilePath& user_data_dir) {
    Registry registry;
    std::string text;
    if (!base::ReadFileToString(user_data_dir.AppendASCII(kRegistryFile),
                                &text)) {
      return registry;
    }
    std::optional<base::DictValue> dict =
        base::JSONReader::ReadDict(text, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
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
  raw_ptr<Profile> base_profile_ = nullptr;
  scoped_refptr<base::SequencedTaskRunner> registry_writer_;
  Registry registry_;
};

}  // namespace

}  // namespace bdk

namespace bdkcore {

Backend& backend() {
  static base::NoDestructor<bdk::EngineBackend> backend;
  return *backend;
}

}  // namespace bdkcore
