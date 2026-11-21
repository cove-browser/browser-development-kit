// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/management_policy.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/extensions/extension_info.h"
#include "bdk/engine/extensions/extension_support.h"
#include "chrome/browser/extensions/error_console/error_console.h"
#include "chrome/browser/extensions/extension_util.h"
#include "chrome/browser/profiles/profile.h"
#include "extensions/browser/extension_error.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/extension_util.h"
#include "extensions/browser/management_policy.h"
#include "extensions/browser/permissions/permissions_updater.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_urls.h"
#include "extensions/common/logging_constants.h"
#include "extensions/common/manifest.h"
#include "extensions/common/manifest_handlers/manifest_url_handlers.h"
#include "extensions/common/mojom/api_permission_id.mojom.h"
#include "extensions/common/permissions/api_permission_set.h"
#include "extensions/common/permissions/manifest_permission_set.h"
#include "extensions/common/permissions/permission_set.h"
#include "extensions/common/permissions/permissions_data.h"
#include "extensions/common/url_pattern_set.h"

namespace bdk {

namespace {

using extensions::Extension;

constexpr char kChromeWebStore[] = "chrome-web-store";

// What the provider saw, by profile and extension.
std::map<std::pair<Profile*, extensions::ExtensionId>, Evaluation>&
Evaluations() {
  static base::NoDestructor<
      std::map<std::pair<Profile*, extensions::ExtensionId>, Evaluation>>
      evaluations;
  return *evaluations;
}

// The APIs of surfaces the configuration can disable. An extension that has
// the permission loses it for as long as the surface is disabled; Chromium's
// feature system then leaves the API out of the extension's chrome object, so
// feature detection works (extensions.md section 6).
struct SurfaceApi {
  extensions::mojom::APIPermissionID permission;
  uint32_t surface;
  const char* name;
  const char* key;  // the bdk.toml key that disabled it
};
constexpr SurfaceApi kSurfaceApis[] = {
    {extensions::mojom::APIPermissionID::kSidePanel,
     BDK_SURFACE_EXTENSION_SIDE_PANELS, "sidePanel",
     "[extensions.ui] side_panels"},
    {extensions::mojom::APIPermissionID::kContextMenus,
     BDK_SURFACE_EXTENSION_CONTEXT_MENUS, "contextMenus",
     "[extensions.ui] context_menus"},
};

class Provider : public extensions::ManagementPolicy::Provider {
 public:
  explicit Provider(Profile* profile) : profile_(profile) {}

  std::string GetDebugPolicyProviderName() const override {
    return "BDK extension policy";
  }

  void UserMayInstall(
      scoped_refptr<const Extension> extension,
      base::OnceCallback<void(extensions::ManagementPolicy::Decision)> callback)
      const override {
    Evaluation evaluation = EvaluateExtension(profile_, *extension);
    std::u16string error;
    if (evaluation.refused) {
      error =
          base::UTF8ToUTF16(std::string(evaluation.code == BDK_CODE_INCOMPATIBLE
                                            ? kRefusalIncompatible
                                            : kRefusalSource) +
                            evaluation.message);
    }
    const bool allowed = !evaluation.refused;
    Evaluations()[{profile_.get(), extension->id()}] = std::move(evaluation);
    std::move(callback).Run({allowed, std::move(error)});
  }

 private:
  raw_ptr<Profile> profile_;
};

class Hooks : public ExtensionPolicyHooks,
              public extensions::ExtensionRegistryObserver {
 public:
  explicit Hooks(Profile* profile) : profile_(profile), provider_(profile) {
    if (extensions::ExtensionSystem* system =
            extensions::ExtensionSystem::Get(profile)) {
      if (extensions::ManagementPolicy* policy = system->management_policy()) {
        policy->RegisterProvider(&provider_);
        registered_ = true;
      }
    }
    // Developer mode lets Chromium accept an unpacked extension and records
    // the extensions' errors (ErrorConsole); BDK refuses unpacked packages
    // itself where the configuration says so (the provider and the install
    // flow), so it is on for the profile as the engine's own switch.
    extensions::util::SetDeveloperModeForProfile(profile, true);
    registry_observation_.Observe(extensions::ExtensionRegistry::Get(profile));
    // What loaded before BDK was watching.
    std::vector<scoped_refptr<const Extension>> loaded;
    for (const scoped_refptr<const Extension>& extension :
         extensions::ExtensionRegistry::Get(profile)->enabled_extensions()) {
      loaded.push_back(extension);
    }
    for (const scoped_refptr<const Extension>& extension : loaded) {
      Enforce(*extension);
    }
  }

  ~Hooks() override {
    if (registered_) {
      if (extensions::ExtensionSystem* system =
              extensions::ExtensionSystem::Get(profile_)) {
        if (extensions::ManagementPolicy* policy =
                system->management_policy()) {
          policy->UnregisterProvider(&provider_);
        }
      }
    }
    for (auto it = Evaluations().begin(); it != Evaluations().end();) {
      it = it->first.first == profile_.get() ? Evaluations().erase(it) : ++it;
    }
  }

  // extensions::ExtensionRegistryObserver:
  void OnExtensionLoaded(content::BrowserContext*,
                         const Extension* extension) override {
    Enforce(*extension);
  }

  void OnShutdown(extensions::ExtensionRegistry*) override {
    registry_observation_.Reset();
  }

 private:
  void Enforce(const Extension& extension) {
    if (!extension.is_extension() ||
        extensions::Manifest::IsComponentLocation(extension.location())) {
      return;
    }
    std::shared_ptr<bdkcore::Runtime> runtime = CurrentRuntime();
    if (!runtime) {
      return;
    }
    WithholdDisabledApis(*runtime, extension);
    if (runtime->extensions.private_never &&
        extensions::util::IsIncognitoEnabled(extension.id(), profile_)) {
      // The setting is applied by reloading the extension, which is not done
      // from inside the load that announced it.
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<Hooks> self, extensions::ExtensionId id) {
                if (self) {
                  extensions::util::SetIsIncognitoEnabled(id, self->profile_,
                                                          false);
                }
              },
              weak_factory_.GetWeakPtr(), extension.id()));
    }
  }

  void WithholdDisabledApis(const bdkcore::Runtime& runtime,
                            const Extension& extension) {
    extensions::APIPermissionSet apis;
    std::vector<const SurfaceApi*> withheld;
    for (const SurfaceApi& api : kSurfaceApis) {
      if (!bdkcore::ui_surface_available(runtime, api.surface) &&
          extension.permissions_data()->HasAPIPermission(api.permission)) {
        apis.insert(api.permission);
        withheld.push_back(&api);
      }
    }
    if (withheld.empty()) {
      return;
    }
    const extensions::PermissionSet to_remove(
        std::move(apis), extensions::ManifestPermissionSet(),
        extensions::URLPatternSet(), extensions::URLPatternSet());
    extensions::PermissionsUpdater(profile_).RemovePermissionsUnsafe(&extension,
                                                                     to_remove);
    // Counted where Chromium reports an extension's errors, so a developer
    // view can list what the extension lost.
    extensions::ErrorConsole* console = extensions::ErrorConsole::Get(profile_);
    if (!console) {
      return;
    }
    console->SetReportingAllForExtension(extension.id(), true);
    for (const SurfaceApi* api : withheld) {
      console->ReportError(std::make_unique<extensions::InternalError>(
          extension.id(),
          base::UTF8ToUTF16(std::string(kUnavailableApiPrefix) + "chrome." +
                            api->name + " is not available: " + api->key +
                            " disables it"),
          extensions::kMinimumSeverityToReportError));
    }
  }

  raw_ptr<Profile> profile_;
  Provider provider_;
  bool registered_ = false;
  base::ScopedObservation<extensions::ExtensionRegistry,
                          extensions::ExtensionRegistryObserver>
      registry_observation_{this};
  base::WeakPtrFactory<Hooks> weak_factory_{this};
};

}  // namespace

Evaluation EvaluateExtension(Profile* profile, const Extension& extension) {
  Evaluation evaluation;
  if (!extension.is_extension() ||
      extensions::Manifest::IsComponentLocation(extension.location())) {
    return evaluation;
  }
  std::shared_ptr<bdkcore::Runtime> runtime = CurrentRuntime();
  if (!runtime) {
    return evaluation;
  }
  const bdkcore::ExtensionConfig& config = runtime->extensions;
  auto refuse = [&evaluation](const char* code, const std::string& why) {
    evaluation.refused = true;
    evaluation.code = code;
    evaluation.message = why;
    return evaluation;
  };

  // The store: its installs and updates need the store enabled.
  if (extension.from_webstore() && !StoreEnabled(config, kChromeWebStore)) {
    return refuse(BDK_CODE_SOURCE_NOT_ALLOWED,
                  std::string("the store \"") + kChromeWebStore +
                      "\" is not enabled ([extensions.install] stores)");
  }
  // A package that is not installed yet comes from a source the configuration
  // must allow (a source switched off does not disable what is installed, so
  // only new ids are held to it).
  if (extensions::Manifest::IsUnpackedLocation(extension.location()) &&
      !config.unpacked &&
      !extensions::ExtensionRegistry::Get(ExtensionProfile(profile))
           ->GetInstalledExtension(extension.id())) {
    return refuse(BDK_CODE_SOURCE_NOT_ALLOWED,
                  "unpacked extensions are not allowed ([extensions.install] "
                  "unpacked = false)");
  }
  // Every update, and the install of a package that names one, must come from
  // an update URL the configuration allows.
  const GURL& update_url = extensions::ManifestURL::GetUpdateURL(&extension);
  if (!extension.from_webstore() && !update_url.is_empty() &&
      !extension_urls::IsWebstoreUpdateUrl(update_url)) {
    const std::string why = bdkcore::ext::source_refusal(
        config, bdkcore::ext::Source::kUpdateUrl, update_url.spec(), "");
    if (!why.empty()) {
      return refuse(BDK_CODE_SOURCE_NOT_ALLOWED, why);
    }
  }
  const bdkcore::ext::Report report = ReportOf(*runtime, extension);
  evaluation.report_json = report.json();
  std::string message;
  if (bdkcore::ext::install_refused(config, report, &message)) {
    evaluation.blocking = *report.first_blocking();
    return refuse(BDK_CODE_INCOMPATIBLE, message);
  }
  return evaluation;
}

std::optional<Evaluation> TakeEvaluation(Profile* profile,
                                         const extensions::ExtensionId& id) {
  auto it = Evaluations().find({ExtensionProfile(profile), id});
  if (it == Evaluations().end()) {
    return std::nullopt;
  }
  Evaluation evaluation = std::move(it->second);
  Evaluations().erase(it);
  return evaluation;
}

std::unique_ptr<ExtensionPolicyHooks> CreateExtensionPolicyHooks(
    Profile* profile) {
  return std::make_unique<Hooks>(profile);
}

bool StoreEnabled(const bdkcore::ExtensionConfig& config,
                  const std::string& store) {
  return std::find(config.stores.begin(), config.stores.end(), store) !=
         config.stores.end();
}

}  // namespace bdk
