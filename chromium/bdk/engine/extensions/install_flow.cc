// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/install_flow.h"

#include <list>
#include <map>
#include <memory>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/version.h"
#include "base/version_info/version_info.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_fetch.h"
#include "bdk/engine/extensions/extension_info.h"
#include "bdk/engine/extensions/extension_support.h"
#include "bdk/engine/extensions/install_prompts.h"
#include "bdk/engine/extensions/management_policy.h"
#include "chrome/browser/extensions/updater/extension_updater.h"
#include "chrome/browser/profiles/profile.h"
#include "components/crx_file/crx_verifier.h"
#include "extensions/browser/crx_file_info.h"
#include "extensions/browser/crx_installer.h"
#include "extensions/browser/extension_file_task_runner.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/install/crx_install_error.h"
#include "extensions/browser/unpacked_installer.h"
#include "extensions/browser/updater/extension_downloader_types.h"
#include "extensions/browser/updater/safe_manifest_parser.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_urls.h"
#include "extensions/common/file_util.h"
#include "extensions/common/mojom/manifest.mojom.h"
#include "net/base/url_util.h"
#include "url/gurl.h"

namespace bdk {

namespace {

using extensions::CrxInstaller;
using extensions::Extension;
using extensions::ExtensionRegistry;
using Payload = ExtensionInstallPrompt::DoneCallbackPayload;

constexpr char kChromeWebStore[] = "chrome-web-store";
constexpr size_t kMaxUpdateManifestSize = 512 * 1024;

std::string Utf8(const std::u16string& text) {
  return base::UTF16ToUTF8(text);
}

// What one installation carries from step to step.
struct State {
  base::WeakPtr<Profile> profile;
  base::WeakPtr<EngineContents> page;
  InstallSource source;
  bool dry_run = false;
  std::shared_ptr<PromptTrace> trace = std::make_shared<PromptTrace>();
  FlowDone done;
};
using StatePtr = std::shared_ptr<State>;

void Fail(const StatePtr& state,
          const char* code,
          const std::string& message,
          const char* domain = BDK_DOMAIN_EXTENSION) {
  if (!state->done) {
    return;
  }
  FlowResult result;
  result.domain = domain;
  result.code = code;
  result.message = message;
  std::move(state->done).Run(std::move(result));
}

void Succeed(const StatePtr& state,
             const std::string& id,
             const std::string& report_json) {
  if (!state->done) {
    return;
  }
  FlowResult result;
  result.ok = true;
  result.extension_id = id;
  result.report_json = report_json;
  std::move(state->done).Run(std::move(result));
}

// An error message that starts with one of BDK's markers says why BDK refused
// (management_policy.h); anything else is Chromium's.
void FailFromMessage(const StatePtr& state, const std::string& message) {
  auto strip = [&message](const char* marker) {
    return message.substr(std::string(marker).size());
  };
  if (message.starts_with(kRefusalIncompatible)) {
    Fail(state, BDK_CODE_INCOMPATIBLE, strip(kRefusalIncompatible));
  } else if (message.starts_with(kRefusalSource)) {
    Fail(state, BDK_CODE_SOURCE_NOT_ALLOWED, strip(kRefusalSource));
  } else {
    Fail(state, BDK_CODE_MANIFEST_INVALID, message);
  }
}

InstallPromptContext PromptContextOf(const StatePtr& state,
                                     std::optional<SourceRecord> record) {
  InstallPromptContext context;
  context.page = state->page;
  context.source = state->source.kind;
  context.record = std::move(record);
  context.trace = state->trace;
  return context;
}

// ---- Unpacked directories ----

struct Loaded {
  scoped_refptr<Extension> extension;
  std::u16string error;
  bool missing = false;
};

Loaded LoadUnpacked(base::FilePath path) {
  Loaded loaded;
  const base::FilePath absolute = base::MakeAbsoluteFilePath(path);
  if (absolute.empty() || !base::DirectoryExists(absolute)) {
    loaded.missing = true;
    return loaded;
  }
  loaded.extension = extensions::file_util::LoadExtension(
      absolute, extensions::mojom::ManifestLocation::kUnpacked,
      Extension::FOLLOW_SYMLINKS_ANYWHERE |
          Extension::REQUIRE_MODERN_MANIFEST_VERSION,
      &loaded.error);
  return loaded;
}

void OnUnpackedInstalled(StatePtr state,
                         const Extension* extension,
                         const base::FilePath&,
                         const std::u16string& error) {
  if (extension) {
    Succeed(state, extension->id(), std::string());
  } else {
    FailFromMessage(state, Utf8(error));
  }
}

void OnUnpackedPrompt(StatePtr state,
                      base::FilePath path,
                      scoped_refptr<Extension> keep_alive,
                      Payload payload) {
  Profile* profile = state->profile.get();
  if (!profile) {
    Fail(state, BDK_CODE_CANCELLED, "the profile was closed");
    return;
  }
  if (payload.result != ExtensionInstallPrompt::Result::ACCEPTED &&
      payload.result !=
          ExtensionInstallPrompt::Result::ACCEPTED_WITH_WITHHELD_PERMISSIONS) {
    Fail(state, BDK_CODE_CANCELLED, "the installation was not approved");
    return;
  }
  scoped_refptr<extensions::UnpackedInstaller> installer =
      extensions::UnpackedInstaller::Create(profile);
  installer->set_be_noisy_on_failure(false);
  installer->set_completion_callback(
      base::BindOnce(&OnUnpackedInstalled, state));
  installer->Load(path);
}

void OnUnpackedLoaded(StatePtr state, base::FilePath path, Loaded loaded) {
  Profile* profile = state->profile.get();
  if (!profile) {
    Fail(state, BDK_CODE_CANCELLED, "the profile was closed");
    return;
  }
  if (loaded.missing) {
    Fail(state, BDK_CODE_NOT_FOUND,
         "there is no directory at " + path.AsUTF8Unsafe());
    return;
  }
  if (!loaded.extension) {
    Fail(state, BDK_CODE_MANIFEST_INVALID, Utf8(loaded.error));
    return;
  }
  const Evaluation evaluation = EvaluateExtension(profile, *loaded.extension);
  if (state->dry_run) {
    std::shared_ptr<bdkcore::Runtime> runtime = CurrentRuntime();
    Succeed(state, loaded.extension->id(),
            !evaluation.report_json.empty()
                ? evaluation.report_json
                : (runtime ? ReportOf(*runtime, *loaded.extension).json()
                           : std::string()));
    return;
  }
  if (evaluation.refused) {
    Fail(state, evaluation.code.c_str(), evaluation.message);
    return;
  }
  scoped_refptr<Extension> extension = loaded.extension;
  PromptForExtension(profile, PromptContextOf(state, std::nullopt),
                     extension.get(),
                     extensions::InstallPromptData::INSTALL_PROMPT, nullptr,
                     base::BindOnce(&OnUnpackedPrompt, state, path, extension));
}

void InstallUnpacked(const StatePtr& state) {
  const base::FilePath path =
      base::FilePath::FromUTF8Unsafe(state->source.path);
  extensions::GetExtensionFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&LoadUnpacked, path),
      base::BindOnce(&OnUnpackedLoaded, state, path));
}

// ---- Packages: a CRX file, however it came ----

struct CrxParams {
  base::FilePath file;
  bool delete_source = false;
  std::string expected_id;
  std::string expected_hash;
  std::string expected_version;
  bool from_store = false;
  SourceRecord record;
};

void OnCrxInstalled(StatePtr state,
                    scoped_refptr<CrxInstaller> installer,
                    const std::optional<extensions::CrxInstallError>& error) {
  Profile* profile = state->profile.get();
  const Extension* extension = installer->extension();
  if (!error) {
    Succeed(state, extension ? extension->id() : std::string(), std::string());
    return;
  }
  std::optional<Evaluation> evaluation;
  if (profile && extension) {
    evaluation = TakeEvaluation(profile, extension->id());
  }
  if (state->dry_run) {
    // The prompt stopped the installation after it had the extension's
    // report; a refusal by the policy has it too.
    if (!state->trace->report_json.empty()) {
      Succeed(state, extension ? extension->id() : std::string(),
              state->trace->report_json);
      return;
    }
    if (evaluation && !evaluation->report_json.empty()) {
      Succeed(state, extension->id(), evaluation->report_json);
      return;
    }
  }
  const std::string message = Utf8(error->message());
  if (message.starts_with(kRefusalIncompatible) ||
      message.starts_with(kRefusalSource)) {
    FailFromMessage(state, message);
    return;
  }
  if (state->trace->declined ||
      error->detail() == extensions::CrxInstallErrorDetail::USER_CANCELED ||
      error->detail() == extensions::CrxInstallErrorDetail::USER_ABORTED) {
    Fail(state, BDK_CODE_CANCELLED, "the installation was not approved");
    return;
  }
  Fail(state, BDK_CODE_MANIFEST_INVALID, message);
}

void InstallCrxPackage(const StatePtr& state, const CrxParams& params) {
  Profile* profile = state->profile.get();
  if (!profile) {
    Fail(state, BDK_CODE_CANCELLED, "the profile was closed");
    return;
  }
  scoped_refptr<CrxInstaller> installer = CrxInstaller::Create(
      profile,
      CreateInstallClient(profile, PromptContextOf(state, params.record)));
  installer->set_error_on_unsupported_requirements(true);
  // BDK's policy replaces Chromium's off-store default-deny: [extensions.
  // install] crx was checked before this, and the prompt stays.
  installer->set_off_store_install_allow_reason(
      params.from_store ? CrxInstaller::OffStoreInstallDisallowed
                        : CrxInstaller::OffStoreInstallAllowedFromSettingsPage);
  installer->set_install_immediately(true);
  installer->set_delete_source(params.delete_source);
  if (params.from_store) {
    installer->set_is_gallery_install(true);
  }
  if (!params.expected_id.empty()) {
    installer->set_expected_id(params.expected_id);
  }
  if (!params.expected_hash.empty()) {
    installer->set_expected_hash(params.expected_hash);
  }
  const base::Version version(params.expected_version);
  if (version.IsValid()) {
    installer->set_expected_version(version,
                                    /*fail_install_if_unexpected=*/true);
  }
  // The installer posts its callbacks, so the callback keeps it alive (the
  // installer drops the callback once it has posted it).
  installer->AddInstallerCallback(
      base::BindOnce(&OnCrxInstalled, state, installer));
  installer->InstallCrx(params.file);
}

void OnPackageExists(StatePtr state, CrxParams params, bool exists) {
  if (!exists) {
    Fail(state, BDK_CODE_NOT_FOUND,
         "there is no file at " + params.file.AsUTF8Unsafe());
    return;
  }
  InstallCrxPackage(state, params);
}

void InstallCrx(const StatePtr& state) {
  CrxParams params;
  params.file = base::FilePath::FromUTF8Unsafe(state->source.path);
  params.record.kind = "crx";
  extensions::GetExtensionFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&base::PathExists, params.file),
      base::BindOnce(&OnPackageExists, state, params));
}

// ---- An update manifest and the package it names ----

const extensions::UpdateManifestResult* NewestOf(
    const extensions::UpdateManifestResults& results,
    const std::string& id) {
  const extensions::UpdateManifestResult* newest = nullptr;
  auto grouped = results.GroupSuccessfulByID();
  auto it = grouped.find(id);
  if (it == grouped.end()) {
    return nullptr;
  }
  for (const extensions::UpdateManifestResult* candidate : it->second) {
    if (!candidate->crx_url.is_valid()) {
      continue;
    }
    if (!newest ||
        base::Version(candidate->version) > base::Version(newest->version)) {
      newest = candidate;
    }
  }
  return newest;
}

struct Offer {
  GURL crx_url;
  std::string version;
  std::string hash;
};

void OnOfferParsed(
    StatePtr state,
    std::string id,
    base::OnceCallback<void(StatePtr, std::optional<Offer>)> next,
    std::unique_ptr<extensions::UpdateManifestResults> results,
    const std::optional<extensions::ManifestParseFailure>& failure) {
  const extensions::UpdateManifestResult* newest =
      results ? NewestOf(*results, id) : nullptr;
  if (!newest) {
    std::move(next).Run(state, std::nullopt);
    return;
  }
  Offer offer;
  offer.crx_url = newest->crx_url;
  offer.version = newest->version;
  offer.hash = newest->package_hash;
  std::move(next).Run(state, std::move(offer));
}

// Fetches and parses the update manifest at `url` and hands over what it
// offers for `id`.
void FetchOffer(StatePtr state,
                const GURL& url,
                const std::string& id,
                base::OnceCallback<void(StatePtr, std::optional<Offer>)> next) {
  Profile* profile = state->profile.get();
  if (!profile) {
    std::move(next).Run(state, std::nullopt);
    return;
  }
  FetchString(
      profile, url, kMaxUpdateManifestSize,
      base::BindOnce(
          [](StatePtr state, std::string id,
             base::OnceCallback<void(StatePtr, std::optional<Offer>)> next,
             std::optional<std::string> body) {
            if (!body) {
              std::move(next).Run(state, std::nullopt);
              return;
            }
            extensions::ParseUpdateManifest(
                *body,
                base::BindOnce(&OnOfferParsed, state, id, std::move(next)));
          },
          state, id, std::move(next)));
}

void OnOfferForInstall(StatePtr state, std::optional<Offer> offer) {
  if (!offer) {
    Fail(state, BDK_CODE_NOT_FOUND,
         "the update manifest could not be fetched or offers nothing for " +
             state->source.id);
    return;
  }
  Profile* profile = state->profile.get();
  if (!profile) {
    Fail(state, BDK_CODE_CANCELLED, "the profile was closed");
    return;
  }
  const bool store = state->source.kind == "store";
  FetchToFile(
      profile, offer->crx_url,
      base::BindOnce(
          [](StatePtr state, Offer offer, bool store, base::FilePath file) {
            if (file.empty()) {
              Fail(state, BDK_CODE_NOT_FOUND,
                   "the package at " + offer.crx_url.spec() +
                       " could not be fetched");
              return;
            }
            CrxParams params;
            params.file = file;
            params.delete_source = true;
            params.expected_id = state->source.id;
            params.expected_hash = offer.hash;
            params.expected_version = offer.version;
            params.from_store = store;
            params.record.kind = store ? "store" : "updateUrl";
            params.record.store = state->source.store;
            params.record.update_url = state->source.update_url;
            InstallCrxPackage(state, params);
          },
          state, *offer, store));
}

void InstallFromUpdateUrl(const StatePtr& state) {
  const GURL url(state->source.update_url);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    Fail(state, BDK_CODE_INVALID_ARGUMENTS,
         "the update URL is not an http or https URL", BDK_DOMAIN_BROKER);
    return;
  }
  FetchOffer(state, url, state->source.id, base::BindOnce(&OnOfferForInstall));
}

// What the store's update service is asked: the package of one extension,
// answered with a redirect to it.
GURL StorePackageUrl(const std::string& id) {
  GURL url = extension_urls::GetWebstoreUpdateUrl();
  url = net::AppendQueryParameter(url, "response", "redirect");
  url = net::AppendQueryParameter(url, "acceptformat", "crx3");
  url = net::AppendQueryParameter(
      url, "prodversion", std::string(version_info::GetVersionNumber()));
  return net::AppendQueryParameter(url, "x",
                                   "id=" + id + "&installsource=ondemand&uc");
}

void InstallFromStore(const StatePtr& state) {
  Profile* profile = state->profile.get();
  if (!profile) {
    Fail(state, BDK_CODE_CANCELLED, "the profile was closed");
    return;
  }
  state->source.store = kChromeWebStore;
  FetchToFile(profile, StorePackageUrl(state->source.id),
              base::BindOnce(
                  [](StatePtr state, base::FilePath file) {
                    if (file.empty()) {
                      Fail(state, BDK_CODE_NOT_FOUND,
                           "the store has no package for " + state->source.id);
                      return;
                    }
                    CrxParams params;
                    params.file = file;
                    params.delete_source = true;
                    params.expected_id = state->source.id;
                    params.from_store = true;
                    params.record.kind = "store";
                    params.record.store = kChromeWebStore;
                    InstallCrxPackage(state, params);
                  },
                  state));
}

// ---- Updates ----

struct UpdateState {
  base::WeakPtr<Profile> profile;
  std::vector<extensions::ExtensionId> ids;
  size_t next = 0;
  UpdateProgress progress;
  UpdatesDone done;
  UpdateReport report;
  std::list<extensions::ExtensionId> store_ids;
  std::map<extensions::ExtensionId, base::Version> store_versions;
};
using UpdateStatePtr = std::shared_ptr<UpdateState>;

void Step(UpdateStatePtr state);

void Finish(const UpdateStatePtr& state) {
  if (state->done) {
    std::move(state->done).Run(std::move(state->report));
  }
}

void RefuseUpdate(const UpdateStatePtr& state,
                  const std::string& code,
                  const std::string& subject,
                  const std::string& message) {
  state->report.refused.push_back(
      bdkcore::ext::Finding{"blocking", code, subject, message});
}

void OnUpdateInstalled(
    UpdateStatePtr state,
    extensions::ExtensionId id,
    scoped_refptr<CrxInstaller> installer,
    const std::optional<extensions::CrxInstallError>& error) {
  Profile* profile = state->profile.get();
  if (!error) {
    state->report.updated.push_back(id);
  } else if (profile) {
    std::optional<Evaluation> evaluation = TakeEvaluation(profile, id);
    if (evaluation && evaluation->blocking) {
      state->report.refused.push_back(*evaluation->blocking);
    } else if (evaluation && evaluation->refused) {
      RefuseUpdate(state, "source.updateUrl.notAllowed", id,
                   evaluation->message);
    }
  }
  Step(state);
}

void OnUpdatePackage(UpdateStatePtr state,
                     extensions::ExtensionId id,
                     Offer offer,
                     base::FilePath file) {
  Profile* profile = state->profile.get();
  const Extension* installed =
      profile ? ExtensionRegistry::Get(profile)->GetInstalledExtension(id)
              : nullptr;
  if (!installed || file.empty()) {
    Step(state);
    return;
  }
  // The installer Chromium's updater makes for an update (extension_updater.cc
  // CreateUpdateInstaller): silent, so a permission increase disables the
  // extension for approval instead of asking.
  scoped_refptr<CrxInstaller> installer = CrxInstaller::CreateSilent(profile);
  installer->set_is_update(true);
  installer->set_expected_id(id);
  installer->set_expected_hash(offer.hash);
  installer->set_install_source(installed->location());
  installer->InitializeCreationFlagsForUpdate(installed, Extension::NO_FLAGS);
  const base::Version version(offer.version);
  if (version.IsValid()) {
    installer->set_expected_version(version,
                                    /*fail_install_if_unexpected=*/true);
  }
  installer->set_delete_source(true);
  installer->AddInstallerCallback(
      base::BindOnce(&OnUpdateInstalled, state, id, installer));
  extensions::CRXFileInfo info(file, crx_file::VerifierFormat::CRX3);
  info.extension_id = id;
  info.expected_hash = offer.hash;
  info.expected_version = version;
  installer->InstallCrxFile(info);
}

void OnUpdateOffer(UpdateStatePtr state,
                   extensions::ExtensionId id,
                   StatePtr fetch_state,
                   std::optional<Offer> offer) {
  Profile* profile = state->profile.get();
  const Extension* installed =
      profile ? ExtensionRegistry::Get(profile)->GetInstalledExtension(id)
              : nullptr;
  if (!installed || !offer ||
      !(base::Version(offer->version) > installed->version())) {
    Step(state);
    return;
  }
  FetchToFile(profile, offer->crx_url,
              base::BindOnce(&OnUpdatePackage, state, id, *offer));
}

// Chromium's updater, for the extensions the store serves.
void UpdateStoreExtensions(const UpdateStatePtr& state) {
  Profile* profile = state->profile.get();
  extensions::ExtensionUpdater* updater =
      profile ? extensions::ExtensionUpdater::Get(profile) : nullptr;
  if (!updater || state->store_ids.empty()) {
    Finish(state);
    return;
  }
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile);
  for (const extensions::ExtensionId& id : state->store_ids) {
    if (const Extension* extension = registry->GetInstalledExtension(id)) {
      state->store_versions[id] = extension->version();
    }
  }
  extensions::ExtensionUpdater::CheckParams params;
  params.ids = state->store_ids;
  params.install_immediately = true;
  params.fetch_priority = extensions::DownloadFetchPriority::kForeground;
  params.callback = base::BindOnce(
      [](UpdateStatePtr state) {
        Profile* profile = state->profile.get();
        if (profile) {
          ExtensionRegistry* registry = ExtensionRegistry::Get(profile);
          for (const auto& [id, before] : state->store_versions) {
            const Extension* now = registry->GetInstalledExtension(id);
            if (now && now->version() > before) {
              state->report.updated.push_back(id);
            }
          }
        }
        Finish(state);
      },
      state);
  updater->CheckNow(std::move(params));
}

void Step(UpdateStatePtr state) {
  Profile* profile = state->profile.get();
  if (!profile) {
    Finish(state);
    return;
  }
  if (state->next >= state->ids.size()) {
    state->progress.Run(1.0, "Checked for updates");
    UpdateStoreExtensions(state);
    return;
  }
  const extensions::ExtensionId id = state->ids[state->next++];
  const Extension* extension =
      ExtensionRegistry::Get(profile)->GetInstalledExtension(id);
  std::shared_ptr<bdkcore::Runtime> runtime = CurrentRuntime();
  if (!extension || !runtime) {
    Step(state);
    return;
  }
  state->progress.Run(static_cast<double>(state->next - 1) /
                          static_cast<double>(state->ids.size()),
                      "Checking " + extension->name());
  if (SourceKindOf(profile, *extension) == "store") {
    // Store extensions are updated by Chromium's updater, and only while the
    // store is enabled: nothing is asked of a store the application did not
    // enable.
    if (StoreEnabled(runtime->extensions, kChromeWebStore)) {
      state->store_ids.push_back(id);
    }
    Step(state);
    return;
  }
  const std::string update_url = UpdateUrlOf(profile, *extension);
  if (update_url.empty()) {
    Step(state);
    return;
  }
  // The update URL policy holds on every update, not only at install.
  const std::string why = bdkcore::ext::source_refusal(
      runtime->extensions, bdkcore::ext::Source::kUpdateUrl, update_url, "");
  if (!why.empty()) {
    RefuseUpdate(state, "source.updateUrl.notAllowed", update_url, why);
    Step(state);
    return;
  }
  auto fetch_state = std::make_shared<State>();
  fetch_state->profile = state->profile;
  FetchOffer(fetch_state, GURL(update_url), id,
             base::BindOnce(&OnUpdateOffer, state, id));
}

}  // namespace

void StartInstall(Profile* profile,
                  base::WeakPtr<EngineContents> page,
                  const InstallSource& source,
                  bool dry_run,
                  FlowDone done) {
  auto state = std::make_shared<State>();
  state->profile = ExtensionProfile(profile)->GetWeakPtr();
  state->page = std::move(page);
  state->source = source;
  state->dry_run = dry_run;
  state->trace->dry_run = dry_run;
  state->done = std::move(done);

  // The source policy comes before Chromium sees the package.
  std::shared_ptr<bdkcore::Runtime> runtime = CurrentRuntime();
  bdkcore::ext::Source kind;
  if (!runtime || !bdkcore::ext::parse_source(source.kind, &kind)) {
    Fail(state, BDK_CODE_INVALID_ARGUMENTS,
         "unknown extension source \"" + source.kind + "\"", BDK_DOMAIN_BROKER);
    return;
  }
  if (kind == bdkcore::ext::Source::kBundled) {
    Fail(state, BDK_CODE_SOURCE_NOT_ALLOWED,
         "bundled extensions are declared in bdk.toml, not installed");
    return;
  }
  const std::string refusal = bdkcore::ext::source_refusal(
      runtime->extensions, kind, source.update_url, source.store);
  if (!refusal.empty()) {
    Fail(state, BDK_CODE_SOURCE_NOT_ALLOWED, refusal);
    return;
  }
  switch (kind) {
    case bdkcore::ext::Source::kUnpacked:
      InstallUnpacked(state);
      return;
    case bdkcore::ext::Source::kCrx:
      InstallCrx(state);
      return;
    case bdkcore::ext::Source::kUpdateUrl:
      InstallFromUpdateUrl(state);
      return;
    case bdkcore::ext::Source::kStore:
      InstallFromStore(state);
      return;
    case bdkcore::ext::Source::kBundled:
      return;
  }
}

void CheckForUpdates(Profile* profile,
                     std::optional<extensions::ExtensionId> id,
                     UpdateProgress progress,
                     UpdatesDone done) {
  profile = ExtensionProfile(profile);
  auto state = std::make_shared<UpdateState>();
  state->profile = profile->GetWeakPtr();
  state->progress = std::move(progress);
  state->done = std::move(done);
  if (id) {
    state->ids.push_back(*id);
  } else {
    for (const extensions::ExtensionId& each : VisibleExtensionIds(profile)) {
      state->ids.push_back(each);
    }
  }
  // Always answered later, like every other command.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&Step, state));
}

}  // namespace bdk
