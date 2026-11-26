// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The extensions.* commands (interface/extensions.json) for installation,
// management, policy and the store (T-EXT-05, T-EXT-06, T-EXT-09):
// extensions.actions, openOptions and inspectView are the actions and window
// work's, and answer broker.notImplemented until it arrives.

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/scoped_refptr.h"
#include "base/one_shot_event.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/bundled_extensions.h"
#include "bdk/engine/extensions/extension_info.h"
#include "bdk/engine/extensions/extension_support.h"
#include "bdk/engine/extensions/install_flow.h"
#include "bdk/engine/extensions/install_prompts.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/services/service_call.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/extensions/commands/command_service.h"
#include "chrome/browser/extensions/error_console/error_console.h"
#include "chrome/browser/extensions/extension_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"
#include "extensions/browser/disable_reason.h"
#include "extensions/browser/error_map.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registrar.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/extension_util.h"
#include "extensions/browser/permissions/permissions_updater.h"
#include "extensions/browser/permissions/scripting_permissions_modifier.h"
#include "extensions/browser/permissions_manager.h"
#include "extensions/common/api/commands/commands_handler.h"
#include "extensions/common/command.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest.h"
#include "extensions/common/permissions/api_permission_set.h"
#include "extensions/common/permissions/manifest_permission_set.h"
#include "extensions/common/permissions/permission_set.h"
#include "extensions/common/url_pattern.h"
#include "extensions/common/url_pattern_set.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace bdk {

namespace {

using Call = std::shared_ptr<ServiceCall>;
using extensions::Extension;
using extensions::ExtensionRegistry;

void RejectExtension(const Call& call, const char* code, const std::string& m) {
  call->Reject(BDK_DOMAIN_EXTENSION, code, m);
}

// Runs `then` once the profile's extension system has started (a command
// can arrive while Chromium is still loading the profile's extensions).
void WhenReady(const Call& call, base::OnceClosure then) {
  extensions::ExtensionSystem* system =
      extensions::ExtensionSystem::Get(ExtensionProfile(call->profile()));
  if (!system) {
    RejectExtension(call, BDK_CODE_SURFACE_DISABLED,
                    "extensions are not available for this profile");
    return;
  }
  system->ready().Post(FROM_HERE, std::move(then));
}

// The extension the call names, or null after rejecting it.
const Extension* FindExtension(const Call& call, const std::string& id) {
  const Extension* extension =
      ExtensionRegistry::Get(ExtensionProfile(call->profile()))
          ->GetExtensionById(id, ExtensionRegistry::EVERYTHING);
  if (!extension) {
    RejectExtension(call, BDK_CODE_NOT_FOUND,
                    "no extension \"" + id + "\" is installed");
  }
  return extension;
}

base::WeakPtr<EngineContents> CallerPage(const Call& call) {
  const std::shared_ptr<bdkcore::Contents>& contents = call->builtin().contents;
  EngineContents* page = contents ? EngineContents::From(*contents) : nullptr;
  return page ? page->GetWeakPtr() : nullptr;
}

std::string JoinJson(const std::vector<std::string>& items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    out += (i ? "," : "") + items[i];
  }
  return out + "]";
}

void ResolveInfo(const Call& call, const std::string& id, bool errors) {
  BuildExtensionInfos(
      call->profile(), {id}, /*all=*/false, errors,
      base::BindOnce(
          [](Call call, std::string id, std::vector<std::string> infos) {
            if (infos.empty()) {
              RejectExtension(call, BDK_CODE_NOT_FOUND,
                              "no extension \"" + id + "\" is installed");
              return;
            }
            call->Resolve(infos[0]);
          },
          call, id));
}

void List(const Call& call) {
  WhenReady(call, base::BindOnce(
                      [](Call call) {
                        BuildExtensionInfos(
                            call->profile(), {}, /*all=*/true, false,
                            base::BindOnce(
                                [](Call call, std::vector<std::string> infos) {
                                  call->Resolve(JoinJson(infos));
                                },
                                call));
                      },
                      call));
}

void Get(const Call& call) {
  const std::string id = *call->String("id");
  WhenReady(call, base::BindOnce(
                      [](Call call, std::string id) {
                        if (FindExtension(call, id)) {
                          ResolveInfo(call, id, /*errors=*/true);
                        }
                      },
                      call, id));
}

InstallSource SourceOf(const base::DictValue& source) {
  auto text = [&source](const char* key) {
    const std::string* value = source.FindString(key);
    return value ? *value : std::string();
  };
  InstallSource out;
  out.kind = text("kind");
  out.path = text("path");
  out.id = text("id");
  out.update_url = text("updateUrl");
  out.store = text("store");
  return out;
}

// An install source of a call must carry what its kind needs.
bool SourceComplete(const Call& call, const InstallSource& source) {
  const bool ok =
      source.kind == "unpacked" || source.kind == "crx" ? !source.path.empty()
      : source.kind == "updateUrl"
          ? !source.id.empty() && !source.update_url.empty()
      : source.kind == "store" ? !source.id.empty() && !source.store.empty()
                               : true;
  if (!ok) {
    call->InvalidArguments("the source lacks what its kind needs");
  }
  return ok;
}

void Inspect(const Call& call) {
  const std::string id = call->StringOr("id", std::string());
  const base::DictValue* source = call->Dict("source");
  if (!id.empty()) {
    WhenReady(call,
              base::BindOnce(
                  [](Call call, std::string id) {
                    const Extension* extension = FindExtension(call, id);
                    std::shared_ptr<bdkcore::Runtime> runtime =
                        CurrentRuntime();
                    if (extension && runtime) {
                      call->Resolve(ReportOf(*runtime, *extension).json());
                    }
                  },
                  call, id));
    return;
  }
  if (!source) {
    call->InvalidArguments("inspect needs a source or an id");
    return;
  }
  const InstallSource install_source = SourceOf(*source);
  if (!SourceComplete(call, install_source)) {
    return;
  }
  StartInstall(call->profile(), CallerPage(call), install_source,
               /*dry_run=*/true,
               base::BindOnce(
                   [](Call call, FlowResult result) {
                     if (result.ok) {
                       call->Resolve(result.report_json);
                     } else {
                       call->Reject(result.domain.c_str(), result.code.c_str(),
                                    result.message);
                     }
                   },
                   call));
}

void Install(const Call& call) {
  const base::DictValue* source = call->Dict("source");
  const InstallSource install_source = SourceOf(*source);
  if (!SourceComplete(call, install_source)) {
    return;
  }
  WhenReady(call,
            base::BindOnce(
                [](Call call, InstallSource install_source) {
                  StartInstall(
                      call->profile(), CallerPage(call), install_source,
                      /*dry_run=*/false,
                      base::BindOnce(
                          [](Call call, FlowResult result) {
                            if (result.ok) {
                              ResolveInfo(call, result.extension_id, false);
                            } else {
                              call->Reject(result.domain.c_str(),
                                           result.code.c_str(), result.message);
                            }
                          },
                          call));
                },
                call, install_source));
}

void Uninstall(const Call& call) {
  WhenReady(call,
            base::BindOnce(
                [](Call call) {
                  const Extension* extension =
                      FindExtension(call, *call->String("id"));
                  if (!extension) {
                    return;
                  }
                  if (FindBundledExtension(extension->path())) {
                    RejectExtension(call, BDK_CODE_NOT_REMOVABLE,
                                    "the extension " + extension->id() +
                                        " ships with the application "
                                        "([extensions.bundled])");
                    return;
                  }
                  ConfirmAndUninstall(
                      ExtensionProfile(call->profile()), CallerPage(call),
                      base::WrapRefCounted(extension),
                      base::BindOnce(
                          [](Call call, UninstallOutcome outcome,
                             const std::string& error) {
                            if (outcome == UninstallOutcome::kUninstalled) {
                              call->ResolveNull();
                            } else {
                              RejectExtension(
                                  call, BDK_CODE_CANCELLED,
                                  outcome == UninstallOutcome::kCancelled
                                      ? "the uninstall was not approved"
                                      : error);
                            }
                          },
                          call));
                },
                call));
}

void SetEnabled(const Call& call) {
  WhenReady(
      call,
      base::BindOnce(
          [](Call call) {
            Profile* profile = ExtensionProfile(call->profile());
            const std::string id = *call->String("id");
            const Extension* extension = FindExtension(call, id);
            if (!extension) {
              return;
            }
            extensions::ExtensionRegistrar* registrar =
                extensions::ExtensionRegistrar::Get(profile);
            if (!*call->Bool("enabled")) {
              const BundledExtension* bundled =
                  FindBundledExtension(extension->path());
              if (bundled && bundled->required) {
                RejectExtension(call, BDK_CODE_NOT_REMOVABLE,
                                "the extension " + id +
                                    " is required by the application "
                                    "([extensions.bundled] required = true)");
                return;
              }
              registrar->DisableExtension(
                  id, {extensions::disable_reason::DISABLE_USER_ACTION});
              call->ResolveNull();
              return;
            }
            if (ExtensionRegistry::Get(profile)->enabled_extensions().Contains(
                    id)) {
              call->ResolveNull();
              return;
            }
            extensions::ExtensionPrefs* prefs =
                extensions::ExtensionPrefs::Get(profile);
            const bool needs_consent =
                prefs->HasDisableReason(
                    id,
                    extensions::disable_reason::DISABLE_PERMISSIONS_INCREASE) ||
                prefs->HasDisableReason(
                    id, extensions::disable_reason::DISABLE_REMOTE_INSTALL);
            if (!needs_consent) {
              for (extensions::disable_reason::DisableReason reason :
                   prefs->GetDisableReasons(id)) {
                if (reason != extensions::disable_reason::DISABLE_USER_ACTION &&
                    reason != extensions::disable_reason::DISABLE_RELOAD) {
                  RejectExtension(
                      call,
                      reason == extensions::disable_reason::
                                    DISABLE_UNSUPPORTED_MANIFEST_VERSION
                          ? BDK_CODE_INCOMPATIBLE
                          : BDK_CODE_MANIFEST_INVALID,
                      "the extension cannot be enabled: " +
                          DisableReasonsJson(profile, id));
                  return;
                }
              }
              registrar->EnableExtension(id);
              call->ResolveNull();
              return;
            }
            // Enabling after a permissions increase asks again (kind
            // permissionsIncrease, or reEnable for a remote install), and
            // grants the new permissions only if it is approved.
            InstallPromptContext context;
            context.page = CallerPage(call);
            PromptForExtension(
                profile, context, extension,
                ExtensionInstallPrompt::GetReEnablePromptTypeForExtension(
                    profile, extension),
                nullptr,
                base::BindOnce(
                    [](Call call, std::string id,
                       ExtensionInstallPrompt::DoneCallbackPayload payload) {
                      Profile* profile = ExtensionProfile(call->profile());
                      const Extension* extension =
                          ExtensionRegistry::Get(profile)
                              ->GetInstalledExtension(id);
                      if (payload.result !=
                              ExtensionInstallPrompt::Result::ACCEPTED &&
                          payload.result !=
                              ExtensionInstallPrompt::Result::
                                  ACCEPTED_WITH_WITHHELD_PERMISSIONS) {
                        RejectExtension(call, BDK_CODE_CANCELLED,
                                        "enabling was not approved");
                      } else if (!extension) {
                        RejectExtension(call, BDK_CODE_NOT_FOUND,
                                        "the extension was removed meanwhile");
                      } else {
                        extensions::ExtensionRegistrar::Get(profile)
                            ->GrantPermissionsAndEnableExtension(*extension);
                        call->ResolveNull();
                      }
                    },
                    call, id));
          },
          call));
}

void SetSiteAccess(const Call& call) {
  WhenReady(
      call,
      base::BindOnce(
          [](Call call) {
            Profile* profile = ExtensionProfile(call->profile());
            const Extension* extension =
                FindExtension(call, *call->String("id"));
            if (!extension) {
              return;
            }
            extensions::PermissionsManager* manager =
                extensions::PermissionsManager::Get(profile);
            if (!manager->CanAffectExtension(*extension)) {
              call->InvalidArguments(
                  "the extension has no host permissions to change");
              return;
            }
            const std::string access = *call->String("access");
            extensions::ScriptingPermissionsModifier modifier(
                profile, base::WrapRefCounted(extension));
            if (access == "onAllSites") {
              modifier.SetWithholdHostPermissions(false);
            } else {
              modifier.SetWithholdHostPermissions(true);
              modifier.RemoveAllGrantedHostPermissions();
              if (access == "onSpecificSites") {
                extensions::URLPatternSet hosts;
                const base::ListValue* patterns = call->List("patterns");
                const base::ListValue no_patterns;
                for (const base::Value& value :
                     patterns ? *patterns : no_patterns) {
                  URLPattern pattern(URLPattern::SCHEME_ALL);
                  if (!value.is_string() ||
                      pattern.Parse(value.GetString()) !=
                          URLPattern::ParseResult::kSuccess) {
                    call->InvalidArguments("\"" +
                                           (value.is_string()
                                                ? value.GetString()
                                                : std::string("?")) +
                                           "\" is not a match pattern");
                    return;
                  }
                  hosts.AddPattern(pattern);
                }
                if (!hosts.is_empty()) {
                  extensions::PermissionsUpdater(profile)
                      .GrantRuntimePermissions(
                          *extension,
                          extensions::PermissionSet(
                              extensions::APIPermissionSet(),
                              extensions::ManifestPermissionSet(),
                              std::move(hosts), extensions::URLPatternSet()),
                          base::DoNothing());
                }
              }
            }
            call->ResolveNull();
          },
          call));
}

void SetAllowedInPrivate(const Call& call) {
  std::shared_ptr<bdkcore::Runtime> runtime = CurrentRuntime();
  const bool allowed = *call->Bool("allowed");
  if (runtime && runtime->extensions.private_never) {
    RejectExtension(call, BDK_CODE_PRIVATE_NOT_ALLOWED,
                    "extensions never run in private profiles "
                    "([extensions] allow_in_private = \"never\")");
    return;
  }
  WhenReady(call,
            base::BindOnce(
                [](Call call, bool allowed) {
                  const Extension* extension =
                      FindExtension(call, *call->String("id"));
                  if (!extension) {
                    return;
                  }
                  if (allowed &&
                      !extensions::util::CanBeIncognitoEnabled(extension)) {
                    RejectExtension(
                        call, BDK_CODE_PRIVATE_NOT_ALLOWED,
                        "the extension's manifest does not allow incognito");
                    return;
                  }
                  extensions::util::SetIsIncognitoEnabled(
                      extension->id(), ExtensionProfile(call->profile()),
                      allowed);
                  call->ResolveNull();
                },
                call, allowed));
}

void SetAllowedFileAccess(const Call& call) {
  WhenReady(call, base::BindOnce(
                      [](Call call) {
                        const Extension* extension =
                            FindExtension(call, *call->String("id"));
                        if (!extension) {
                          return;
                        }
                        extensions::util::SetAllowFileAccess(
                            extension->id(), ExtensionProfile(call->profile()),
                            *call->Bool("allowed"));
                        call->ResolveNull();
                      },
                      call));
}

void SetPinned(const Call& call) {
  WhenReady(call, base::BindOnce(
                      [](Call call) {
                        Profile* profile = ExtensionProfile(call->profile());
                        const Extension* extension =
                            FindExtension(call, *call->String("id"));
                        if (!extension) {
                          return;
                        }
                        const bool pinned = *call->Bool("pinned");
                        RecordPinned(profile, extension->id(), pinned);
                        ToolbarActionsModel* model =
                            ToolbarActionsModel::Get(profile);
                        if (model && model->HasAction(extension->id()) &&
                            model->IsActionPinned(extension->id()) != pinned) {
                          model->SetActionVisibility(extension->id(), pinned);
                        }
                        call->ResolveNull();
                      },
                      call));
}

void Reload(const Call& call) {
  WhenReady(call, base::BindOnce(
                      [](Call call) {
                        const Extension* extension =
                            FindExtension(call, *call->String("id"));
                        if (!extension) {
                          return;
                        }
                        if (!extensions::Manifest::IsUnpackedLocation(
                                extension->location())) {
                          RejectExtension(
                              call, BDK_CODE_NOT_UNPACKED,
                              "only an unpacked extension can be reloaded");
                          return;
                        }
                        extensions::ExtensionRegistrar::Get(
                            ExtensionProfile(call->profile()))
                            ->ReloadExtension(extension->id());
                        call->ResolveNull();
                      },
                      call));
}

void CheckForUpdatesCommand(const Call& call) {
  WhenReady(
      call,
      base::BindOnce(
          [](Call call) {
            CheckForUpdates(
                call->profile(), call->String("id"),
                base::BindRepeating(
                    [](Call call, double fraction, const std::string& message) {
                      call->Progress(fraction, message);
                    },
                    call),
                base::BindOnce(
                    [](Call call, UpdateReport report) {
                      base::ListValue updated;
                      for (const std::string& id : report.updated) {
                        updated.Append(id);
                      }
                      base::ListValue refused;
                      for (const bdkcore::ext::Finding& f : report.refused) {
                        refused.Append(base::DictValue()
                                           .Set("severity", f.severity)
                                           .Set("code", f.code)
                                           .Set("subject", f.subject)
                                           .Set("message", f.message));
                      }
                      call->Resolve(
                          ToJson(base::DictValue()
                                     .Set("updated", std::move(updated))
                                     .Set("refused", std::move(refused))));
                    },
                    call));
          },
          call));
}

void ClearErrors(const Call& call) {
  WhenReady(call, base::BindOnce(
                      [](Call call) {
                        const Extension* extension =
                            FindExtension(call, *call->String("id"));
                        if (!extension) {
                          return;
                        }
                        if (extensions::ErrorConsole* console =
                                extensions::ErrorConsole::Get(
                                    ExtensionProfile(call->profile()))) {
                          console->RemoveErrors(
                              extensions::ErrorMap::Filter::ErrorsForExtension(
                                  extension->id()));
                        }
                        call->ResolveNull();
                      },
                      call));
}

void Commands(const Call& call) {
  WhenReady(
      call,
      base::BindOnce(
          [](Call call) {
            Profile* profile = ExtensionProfile(call->profile());
            extensions::CommandService* service =
                extensions::CommandService::Get(profile);
            std::vector<std::string> commands;
            for (const extensions::ExtensionId& id :
                 VisibleExtensionIds(profile)) {
              const Extension* extension =
                  ExtensionRegistry::Get(profile)->GetExtensionById(
                      id, ExtensionRegistry::EVERYTHING);
              const ui::CommandMap* named =
                  extension
                      ? extensions::CommandsInfo::GetNamedCommands(extension)
                      : nullptr;
              if (!named) {
                continue;
              }
              for (const auto& [name, declared] : *named) {
                base::DictValue json;
                json.Set("extensionId", id);
                json.Set("name", name);
                json.Set("description",
                         base::UTF16ToUTF8(declared.description()));
                const std::string suggested =
                    ui::Command::AcceleratorToString(declared.accelerator());
                if (!suggested.empty()) {
                  json.Set("suggestedKey", suggested);
                }
                const std::string shortcut = ui::Command::AcceleratorToString(
                    service->FindCommandByName(id, name).accelerator());
                if (!shortcut.empty()) {
                  json.Set("shortcut", shortcut);
                }
                json.Set("isAction",
                         extensions::Command::IsActionRelatedCommand(name));
                commands.push_back(ToJson(json));
              }
            }
            call->Resolve(JoinJson(commands));
          },
          call));
}

void SetCommandShortcut(const Call& call) {
  WhenReady(
      call,
      base::BindOnce(
          [](Call call) {
            Profile* profile = ExtensionProfile(call->profile());
            const std::string id = *call->String("extensionId");
            const std::string name = *call->String("name");
            const Extension* extension = FindExtension(call, id);
            if (!extension) {
              return;
            }
            const ui::CommandMap* named =
                extensions::CommandsInfo::GetNamedCommands(extension);
            if (!named || !named->contains(name)) {
              RejectExtension(
                  call, BDK_CODE_NOT_FOUND,
                  "the extension declares no command \"" + name + "\"");
              return;
            }
            extensions::CommandService* service =
                extensions::CommandService::Get(profile);
            const std::optional<std::string> shortcut =
                call->String("shortcut");
            if (!shortcut || shortcut->empty()) {
              service->RemoveKeybindingPrefs(id, name);
            } else {
              if (extensions::Command::StringToAccelerator(*shortcut, name)
                      .key_code() == ui::VKEY_UNKNOWN) {
                call->InvalidArguments("\"" + *shortcut +
                                       "\" is not a shortcut");
                return;
              }
              service->UpdateKeybindingPrefs(id, name, *shortcut);
            }
            call->ResolveNull();
          },
          call));
}

}  // namespace

void AddExtensionsHandlers(HandlerTable& table) {
  table["extensions.list"] = &List;
  table["extensions.get"] = &Get;
  table["extensions.inspect"] = &Inspect;
  table["extensions.install"] = &Install;
  table["extensions.uninstall"] = &Uninstall;
  table["extensions.setEnabled"] = &SetEnabled;
  table["extensions.setSiteAccess"] = &SetSiteAccess;
  table["extensions.setAllowedInPrivate"] = &SetAllowedInPrivate;
  table["extensions.setAllowedFileAccess"] = &SetAllowedFileAccess;
  table["extensions.setPinned"] = &SetPinned;
  table["extensions.reload"] = &Reload;
  table["extensions.checkForUpdates"] = &CheckForUpdatesCommand;
  table["extensions.clearErrors"] = &ClearErrors;
  table["extensions.commands"] = &Commands;
  table["extensions.setCommandShortcut"] = &SetCommandShortcut;
}

}  // namespace bdk
