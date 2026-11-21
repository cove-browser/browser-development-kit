// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_events.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "bdk/engine/extensions/extension_info.h"
#include "bdk/engine/extensions/management_policy.h"
#include "bdk/engine/services/service_call.h"
#include "chrome/browser/extensions/error_console/error_console.h"
#include "chrome/browser/profiles/profile.h"
#include "extensions/browser/extension_error.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_prefs_observer.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "extensions/browser/ui_util.h"
#include "extensions/browser/uninstall_reason.h"
#include "extensions/common/extension.h"

namespace bdk {

namespace {

using extensions::Extension;
using extensions::ExtensionRegistry;

class ExtensionEvents : public ServiceEvents,
                        public extensions::ExtensionRegistryObserver,
                        public extensions::ExtensionPrefsObserver,
                        public extensions::ErrorConsole::Observer {
 public:
  ExtensionEvents(Profile* profile, std::weak_ptr<bdkcore::Profile> bdk_profile)
      : profile_(profile),
        bdk_profile_(std::move(bdk_profile)),
        hooks_(CreateExtensionPolicyHooks(profile)) {
    registry_observation_.Observe(ExtensionRegistry::Get(profile));
    prefs_observation_.Observe(extensions::ExtensionPrefs::Get(profile));
    if (extensions::ErrorConsole* console =
            extensions::ErrorConsole::Get(profile)) {
      console_observation_.Observe(console);
    }
  }

  // extensions::ExtensionRegistryObserver:
  void OnExtensionLoaded(content::BrowserContext*,
                         const Extension* extension) override {
    if (Visible(*extension)) {
      EmitEvent(bdk_profile_, "extensions.loaded", IdPayload(extension->id()));
    }
  }

  void OnExtensionUnloaded(
      content::BrowserContext*,
      const Extension* extension,
      extensions::UnloadedExtensionReason reason) override {
    if (Visible(*extension)) {
      EmitEvent(bdk_profile_, "extensions.unloaded",
                "{\"id\":" + JsonString(extension->id()) + ",\"reason\":" +
                    JsonString(UnloadReasonName(reason)) + "}");
    }
  }

  void OnExtensionWillBeInstalled(content::BrowserContext*,
                                  const Extension* extension,
                                  bool is_update,
                                  const std::string& old_name) override {
    if (!is_update) {
      return;
    }
    // The registry still has the version the update replaces.
    if (const Extension* old =
            ExtensionRegistry::Get(profile_)->GetInstalledExtension(
                extension->id())) {
      previous_versions_[extension->id()] = old->GetVersionForDisplay();
    }
  }

  void OnExtensionInstalled(content::BrowserContext*,
                            const Extension* extension,
                            bool is_update) override {
    if (std::optional<SourceRecord> record =
            TakeInstallSource(profile_, extension->id())) {
      RecordSource(profile_, extension->id(), *record);
    }
    if (!Visible(*extension)) {
      return;
    }
    std::string previous;
    if (is_update) {
      previous = previous_versions_[extension->id()];
    }
    previous_versions_.erase(extension->id());
    // The announcement carries the extension's icon, which loads first.
    BuildExtensionInfos(
        profile_, {extension->id()}, /*all=*/false, /*include_errors=*/false,
        base::BindOnce(
            [](base::WeakPtr<ExtensionEvents> self, bool is_update,
               std::string previous, std::vector<std::string> infos) {
              if (!self || infos.empty()) {
                return;
              }
              if (is_update) {
                EmitEvent(self->bdk_profile_, "extensions.updated",
                          "{\"extension\":" + infos[0] +
                              ",\"previousVersion\":" + JsonString(previous) +
                              "}");
              } else {
                EmitEvent(self->bdk_profile_, "extensions.installed",
                          "{\"extension\":" + infos[0] + "}");
              }
            },
            weak_factory_.GetWeakPtr(), is_update, std::move(previous)));
  }

  void OnExtensionUninstalled(content::BrowserContext*,
                              const Extension* extension,
                              extensions::UninstallReason) override {
    if (Visible(*extension)) {
      EmitEvent(bdk_profile_, "extensions.uninstalled",
                IdPayload(extension->id()));
    }
  }

  void OnShutdown(ExtensionRegistry*) override {
    registry_observation_.Reset();
  }

  // extensions::ExtensionPrefsObserver:
  void OnExtensionStateChanged(const extensions::ExtensionId& id,
                               bool enabled) override {
    const Extension* extension =
        ExtensionRegistry::Get(profile_)->GetExtensionById(
            id, ExtensionRegistry::EVERYTHING);
    if (!extension || !Visible(*extension)) {
      return;
    }
    EmitEvent(bdk_profile_, "extensions.enabledChanged",
              "{\"id\":" + JsonString(id) +
                  ",\"isEnabled\":" + (enabled ? "true" : "false") +
                  ",\"disableReasons\":" + DisableReasonsJson(profile_, id) +
                  "}");
  }

  void OnExtensionPrefsWillBeDestroyed(
      extensions::ExtensionPrefs* prefs) override {
    prefs_observation_.Reset();
  }

  // extensions::ErrorConsole::Observer:
  void OnErrorAdded(const extensions::ExtensionError* error) override {
    const Extension* extension =
        ExtensionRegistry::Get(profile_)->GetExtensionById(
            error->extension_id(), ExtensionRegistry::EVERYTHING);
    if (!extension || !Visible(*extension)) {
      return;
    }
    EmitEvent(bdk_profile_, "extensions.errorAdded",
              "{\"id\":" + JsonString(error->extension_id()) +
                  ",\"error\":" + ExtensionErrorJson(*error) + "}");
  }

  void OnErrorConsoleDestroyed() override { console_observation_.Reset(); }

 private:
  // Extensions a person sees (the ones chrome://extensions lists), not the
  // component extensions that are Chromium's own parts.
  static bool Visible(const Extension& extension) {
    return extension.is_extension() &&
           extensions::ui_util::ShouldDisplayInExtensionSettings(extension);
  }

  static std::string IdPayload(const std::string& id) {
    return "{\"id\":" + JsonString(id) + "}";
  }

  raw_ptr<Profile> profile_;
  std::weak_ptr<bdkcore::Profile> bdk_profile_;
  std::unique_ptr<ExtensionPolicyHooks> hooks_;
  // The version an update replaces, from its WillBeInstalled to its Installed.
  std::map<std::string, std::string> previous_versions_;
  base::ScopedObservation<ExtensionRegistry,
                          extensions::ExtensionRegistryObserver>
      registry_observation_{this};
  base::ScopedObservation<extensions::ExtensionPrefs,
                          extensions::ExtensionPrefsObserver>
      prefs_observation_{this};
  base::ScopedObservation<extensions::ErrorConsole,
                          extensions::ErrorConsole::Observer>
      console_observation_{this};
  base::WeakPtrFactory<ExtensionEvents> weak_factory_{this};
};

}  // namespace

std::unique_ptr<ServiceEvents> CreateExtensionEvents(
    Profile* profile,
    std::weak_ptr<bdkcore::Profile> bdk_profile) {
  if (profile->IsOffTheRecord()) {
    return nullptr;
  }
  return std::make_unique<ExtensionEvents>(profile, std::move(bdk_profile));
}

}  // namespace bdk
