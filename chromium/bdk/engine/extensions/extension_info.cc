// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_info.h"

#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "bdk/engine/extensions/bundled_extensions.h"
#include "bdk/engine/extensions/extension_support.h"
#include "bdk/engine/json_util.h"
#include "chrome/browser/extensions/error_console/error_console.h"
#include "chrome/browser/extensions/extension_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"
#include "extensions/browser/disable_reason.h"
#include "extensions/browser/extension_error.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_util.h"
#include "extensions/browser/image_loader.h"
#include "extensions/browser/install_prompt_permissions.h"
#include "extensions/browser/permissions_manager.h"
#include "extensions/browser/ui_util.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_urls.h"
#include "extensions/common/icons/extension_icon_set.h"
#include "extensions/common/manifest.h"
#include "extensions/common/manifest_handlers/icons_handler.h"
#include "extensions/common/manifest_handlers/manifest_url_handlers.h"
#include "extensions/common/manifest_handlers/options_page_info.h"
#include "extensions/common/permissions/api_permission.h"
#include "extensions/common/permissions/api_permission_set.h"
#include "extensions/common/permissions/permission_set.h"
#include "extensions/common/permissions/permissions_data.h"
#include "extensions/common/stack_frame.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image.h"
#include "url/gurl.h"

namespace bdk {

namespace {

using extensions::Extension;
using extensions::ExtensionPrefs;
using extensions::ExtensionRegistry;

// The keys of BDK's records in an extension's preferences.
constexpr char kSourceKey[] = "bdk_source";
constexpr char kStoreKey[] = "bdk_store";
constexpr char kUpdateUrlKey[] = "bdk_update_url";
constexpr char kPinnedKey[] = "bdk_pinned";

base::ListValue StringList(const std::vector<std::string>& strings) {
  base::ListValue list;
  for (const std::string& s : strings) {
    list.Append(s);
  }
  return list;
}

std::string ReadPref(Profile* profile,
                     const extensions::ExtensionId& id,
                     const char* key) {
  std::string value;
  ExtensionPrefs::Get(profile)->ReadPrefAsString(id, key, &value);
  return value;
}

// The disable reason names of one Chromium reason, or null for a reason that
// is not worth a name of its own ("other" covers it).
const char* ReasonName(extensions::disable_reason::DisableReason reason) {
  switch (reason) {
    case extensions::disable_reason::DISABLE_USER_ACTION:
      return "user";
    case extensions::disable_reason::DISABLE_PERMISSIONS_INCREASE:
      return "permissionsIncreased";
    case extensions::disable_reason::DISABLE_UNSUPPORTED_MANIFEST_VERSION:
    case extensions::disable_reason::DISABLE_UNSUPPORTED_REQUIREMENT:
      return "incompatible";
    case extensions::disable_reason::DISABLE_GREYLIST:
    // BDK's only reason to keep an extension disabled: [extensions.install]
    // blocklist (management_policy.cc).
    case extensions::disable_reason::DISABLE_BLOCKED_BY_POLICY:
      return "blocklisted";
    case extensions::disable_reason::DISABLE_CORRUPTED:
      return "corrupted";
    default:
      return "other";
  }
}

// The access the extension has to sites, as the interface names it, and the
// sites a person granted when it is `onSpecificSites`.
std::string SiteAccessOf(Profile* profile,
                         const Extension& extension,
                         base::ListValue& patterns) {
  extensions::PermissionsManager* manager =
      extensions::PermissionsManager::Get(profile);
  if (!manager->CanAffectExtension(extension) ||
      !manager->HasWithheldHostPermissions(extension)) {
    return "onAllSites";
  }
  std::unique_ptr<const extensions::PermissionSet> granted =
      ExtensionPrefs::Get(profile)->GetRuntimeGrantedPermissions(
          extension.id());
  if (!granted || granted->effective_hosts().is_empty()) {
    return "onClick";
  }
  if (granted->ShouldWarnAllHosts(/*include_api_permissions=*/false)) {
    return "onAllSites";
  }
  for (const std::string& pattern :
       granted->effective_hosts().ToStringVector()) {
    patterns.Append(pattern);
  }
  return "onSpecificSites";
}

base::DictValue PermissionsOf(const Extension& extension) {
  const extensions::PermissionSet& active =
      extension.permissions_data()->active_permissions();
  base::ListValue apis;
  for (const extensions::APIPermission* permission : active.apis()) {
    apis.Append(std::string(permission->name()));
  }
  base::ListValue hosts;
  for (const std::string& pattern : active.explicit_hosts().ToStringVector()) {
    hosts.Append(pattern);
  }
  for (const std::string& pattern :
       active.scriptable_hosts().ToStringVector()) {
    hosts.Append(pattern);
  }
  extensions::InstallPromptPermissions messages;
  messages.LoadFromPermissionSet(&active, extension.GetType());
  base::ListValue warnings;
  for (const std::u16string& warning : messages.permissions) {
    warnings.Append(base::UTF16ToUTF8(warning));
  }
  return base::DictValue()
      .Set("apiPermissions", std::move(apis))
      .Set("hostPatterns", std::move(hosts))
      .Set("warnings", std::move(warnings));
}

bool IsPinned(Profile* profile, const extensions::ExtensionId& id) {
  ToolbarActionsModel* model = ToolbarActionsModel::Get(profile);
  if (model && model->HasAction(id)) {
    return model->IsActionPinned(id);
  }
  bool pinned = false;
  ExtensionPrefs::Get(profile)->ReadPrefAsBoolean(id, kPinnedKey, &pinned);
  return pinned;
}

void FinishIcon(base::OnceCallback<void(std::string)> callback,
                const gfx::Image& image) {
  std::move(callback).Run(IconDataUrl(image));
}

struct Batch {
  std::vector<extensions::ExtensionId> ids;
  std::vector<std::string> icons;
  size_t remaining = 0;
  bool include_errors = false;
  base::OnceCallback<void(std::vector<std::string>)> done;
};

void FinishBatch(Profile* profile, const std::shared_ptr<Batch>& batch) {
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile);
  std::vector<std::string> infos;
  for (size_t i = 0; i < batch->ids.size(); ++i) {
    const Extension* extension = registry->GetExtensionById(
        batch->ids[i], ExtensionRegistry::EVERYTHING);
    if (extension) {
      infos.push_back(ExtensionInfoJson(profile, *extension, batch->icons[i],
                                        batch->include_errors));
    }
  }
  std::move(batch->done).Run(std::move(infos));
}

}  // namespace

void RecordSource(Profile* profile,
                  const extensions::ExtensionId& id,
                  const SourceRecord& record) {
  ExtensionPrefs* prefs = ExtensionPrefs::Get(ExtensionProfile(profile));
  prefs->UpdateExtensionPref(id, kSourceKey, base::Value(record.kind));
  prefs->UpdateExtensionPref(id, kStoreKey,
                             record.store.empty()
                                 ? std::nullopt
                                 : std::optional<base::Value>(record.store));
  prefs->UpdateExtensionPref(
      id, kUpdateUrlKey,
      record.update_url.empty()
          ? std::nullopt
          : std::optional<base::Value>(record.update_url));
}

namespace {

// Installations in progress, by profile and extension.
std::map<std::pair<Profile*, extensions::ExtensionId>, SourceRecord>&
PendingSources() {
  static base::NoDestructor<
      std::map<std::pair<Profile*, extensions::ExtensionId>, SourceRecord>>
      pending;
  return *pending;
}

}  // namespace

void NoteInstallSource(Profile* profile,
                       const extensions::ExtensionId& id,
                       const SourceRecord& record) {
  PendingSources()[{ExtensionProfile(profile), id}] = record;
}

std::optional<SourceRecord> TakeInstallSource(
    Profile* profile,
    const extensions::ExtensionId& id) {
  auto it = PendingSources().find({ExtensionProfile(profile), id});
  if (it == PendingSources().end()) {
    return std::nullopt;
  }
  SourceRecord record = std::move(it->second);
  PendingSources().erase(it);
  return record;
}

void RecordPinned(Profile* profile,
                  const extensions::ExtensionId& id,
                  bool pinned) {
  ExtensionPrefs::Get(ExtensionProfile(profile))
      ->UpdateExtensionPref(id, kPinnedKey, base::Value(pinned));
}

std::string SourceKindOf(Profile* profile, const Extension& extension) {
  if (extensions::Manifest::IsComponentLocation(extension.location()) ||
      FindBundledExtension(extension.path())) {
    return "bundled";
  }
  if (extensions::Manifest::IsUnpackedLocation(extension.location())) {
    return "unpacked";
  }
  const std::string recorded =
      ReadPref(ExtensionProfile(profile), extension.id(), kSourceKey);
  if (!recorded.empty()) {
    return recorded;
  }
  if (extension.from_webstore()) {
    return "store";
  }
  const GURL& update_url = extensions::ManifestURL::GetUpdateURL(&extension);
  return !update_url.is_empty() &&
                 !extension_urls::IsWebstoreUpdateUrl(update_url)
             ? "updateUrl"
             : "crx";
}

std::string StoreOf(Profile* profile, const Extension& extension) {
  const std::string recorded =
      ReadPref(ExtensionProfile(profile), extension.id(), kStoreKey);
  if (!recorded.empty()) {
    return recorded;
  }
  return extension.from_webstore() ? "chrome-web-store" : std::string();
}

std::string UpdateUrlOf(Profile* profile, const Extension& extension) {
  const std::string recorded =
      ReadPref(ExtensionProfile(profile), extension.id(), kUpdateUrlKey);
  if (!recorded.empty()) {
    return recorded;
  }
  const GURL& update_url = extensions::ManifestURL::GetUpdateURL(&extension);
  return !update_url.is_empty() &&
                 !extension_urls::IsWebstoreUpdateUrl(update_url)
             ? update_url.spec()
             : std::string();
}

std::string IconDataUrl(const gfx::Image& image) {
  if (image.IsEmpty()) {
    return std::string();
  }
  scoped_refptr<base::RefCountedMemory> png = image.As1xPNGBytes();
  if (!png || png->size() == 0) {
    return std::string();
  }
  return "data:image/png;base64," +
         base::Base64Encode(base::span<const uint8_t>(*png));
}

void LoadIconDataUrl(Profile* profile,
                     const Extension& extension,
                     base::OnceCallback<void(std::string)> callback) {
  extensions::ExtensionResource resource =
      extensions::IconsInfo::GetIconResource(
          &extension, extension_misc::EXTENSION_ICON_LARGE,
          ExtensionIconSet::Match::kBigger);
  extensions::ImageLoader* loader =
      extensions::ImageLoader::Get(ExtensionProfile(profile));
  if (resource.empty() || !loader) {
    // Answered later, so every caller sees the same asynchronous shape.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), std::string()));
    return;
  }
  loader->LoadImageAsync(&extension, resource,
                         gfx::Size(extension_misc::EXTENSION_ICON_LARGE,
                                   extension_misc::EXTENSION_ICON_LARGE),
                         base::BindOnce(&FinishIcon, std::move(callback)));
}

std::vector<std::string> DisableReasonNames(Profile* profile,
                                            const extensions::ExtensionId& id) {
  profile = ExtensionProfile(profile);
  std::vector<std::string> names;
  auto add = [&names](const char* name) {
    for (const std::string& existing : names) {
      if (existing == name) {
        return;
      }
    }
    names.emplace_back(name);
  };
  if (ExtensionRegistry::Get(profile)->blocklisted_extensions().Contains(id)) {
    add("blocklisted");
  }
  for (extensions::disable_reason::DisableReason reason :
       ExtensionPrefs::Get(profile)->GetDisableReasons(id)) {
    add(ReasonName(reason));
  }
  return names;
}

std::string DisableReasonsJson(Profile* profile,
                               const extensions::ExtensionId& id) {
  return ToJson(StringList(DisableReasonNames(profile, id)));
}

std::string ExtensionErrorJson(const extensions::ExtensionError& error) {
  std::string kind = "runtime";
  std::string message = base::UTF16ToUTF8(error.message());
  std::string source = base::UTF16ToUTF8(error.source());
  std::optional<int> line;
  switch (error.type()) {
    case extensions::ExtensionError::Type::kManifestError:
      kind = "manifest";
      source =
          static_cast<const extensions::ManifestError&>(error).manifest_key();
      break;
    case extensions::ExtensionError::Type::kRuntimeError: {
      const extensions::StackTrace& trace =
          static_cast<const extensions::RuntimeError&>(error).stack_trace();
      if (!trace.empty()) {
        line = static_cast<int>(trace.front().line_number);
      }
      break;
    }
    default:
      if (message.starts_with(kUnavailableApiPrefix)) {
        kind = "unavailableApi";
        message.erase(0, sizeof(kUnavailableApiPrefix) - 1);
      }
      break;
  }
  base::DictValue json;
  json.Set("kind", kind);
  json.Set("message", message);
  if (!source.empty()) {
    json.Set("source", source);
  }
  if (line) {
    json.Set("line", *line);
  }
  json.Set("count", static_cast<int>(error.occurrences()));
  return ToJson(json);
}

std::string ExtensionInfoJson(Profile* profile,
                              const Extension& extension,
                              const std::string& icon_data_url,
                              bool include_errors) {
  profile = ExtensionProfile(profile);
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile);
  const extensions::ExtensionId& id = extension.id();
  std::shared_ptr<bdkcore::Runtime> runtime = CurrentRuntime();

  base::DictValue json;
  json.Set("id", id);
  json.Set("name", extension.name());
  json.Set("version", extension.GetVersionForDisplay());
  const std::string* description =
      extension.manifest()->value()->FindString("description");
  json.Set("description", description ? *description : std::string());
  if (!icon_data_url.empty()) {
    json.Set("icon", icon_data_url);
  }
  json.Set("manifestVersion", extension.manifest_version());
  json.Set("source", SourceKindOf(profile, extension));
  const std::string store = StoreOf(profile, extension);
  if (!store.empty()) {
    json.Set("store", store);
  }
  json.Set("isEnabled", registry->enabled_extensions().Contains(id));
  base::ListValue reasons;
  for (const std::string& name : DisableReasonNames(profile, id)) {
    reasons.Append(name);
  }
  json.Set("disableReasons", std::move(reasons));
  json.Set("permissions", PermissionsOf(extension));
  base::ListValue patterns;
  json.Set("siteAccess", SiteAccessOf(profile, extension, patterns));
  json.Set("siteAccessPatterns", std::move(patterns));
  json.Set("allowedInPrivate",
           !(runtime && runtime->extensions.private_never) &&
               extensions::util::IsIncognitoEnabled(id, profile));
  json.Set("allowedFileAccess", extensions::util::AllowFileAccess(id, profile));
  json.Set("isPinned", IsPinned(profile, id));
  base::ListValue surfaces;
  for (const std::string& surface :
       bdkcore::ext::manifest_surfaces(ManifestJsonOf(extension))) {
    surfaces.Append(surface);
  }
  json.Set("surfaces", std::move(surfaces));
  const GURL& options = extensions::OptionsPageInfo::GetOptionsPage(&extension);
  if (options.is_valid()) {
    json.Set("optionsPage", options.spec());
  }
  if (include_errors) {
    base::ListValue errors;
    if (extensions::ErrorConsole* console =
            extensions::ErrorConsole::Get(profile)) {
      for (const auto& error : console->GetErrorsForExtension(id)) {
        if (std::optional<base::DictValue> parsed =
                ParseJsonDict(ExtensionErrorJson(*error))) {
          errors.Append(std::move(*parsed));
        }
      }
    }
    json.Set("errors", std::move(errors));
  }
  std::optional<base::DictValue> compatibility =
      ParseJsonDict(runtime ? ReportOf(*runtime, extension).json()
                            : "{\"verdict\":\"compatible\",\"findings\":[]}");
  json.Set("compatibility",
           compatibility ? std::move(*compatibility) : base::DictValue());
  return ToJson(json);
}

std::vector<extensions::ExtensionId> VisibleExtensionIds(Profile* profile) {
  std::vector<extensions::ExtensionId> ids;
  const extensions::ExtensionSet set =
      ExtensionRegistry::Get(ExtensionProfile(profile))
          ->GenerateInstalledExtensionsSet(ExtensionRegistry::ENABLED |
                                           ExtensionRegistry::DISABLED |
                                           ExtensionRegistry::BLOCKLISTED);
  for (const scoped_refptr<const Extension>& extension : set) {
    if (extension->is_extension() &&
        extensions::ui_util::ShouldDisplayInExtensionSettings(*extension)) {
      ids.push_back(extension->id());
    }
  }
  return ids;
}

void BuildExtensionInfos(
    Profile* profile,
    std::vector<extensions::ExtensionId> ids,
    bool all,
    bool include_errors,
    base::OnceCallback<void(std::vector<std::string>)> done) {
  profile = ExtensionProfile(profile);
  if (all) {
    ids = VisibleExtensionIds(profile);
  }
  auto batch = std::make_shared<Batch>();
  batch->ids = std::move(ids);
  batch->icons.resize(batch->ids.size());
  batch->remaining = batch->ids.size();
  batch->include_errors = include_errors;
  batch->done = std::move(done);
  if (batch->ids.empty()) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](std::shared_ptr<Batch> batch) {
                         std::move(batch->done).Run({});
                       },
                       batch));
    return;
  }
  ExtensionRegistry* registry = ExtensionRegistry::Get(profile);
  for (size_t i = 0; i < batch->ids.size(); ++i) {
    const Extension* extension = registry->GetExtensionById(
        batch->ids[i], ExtensionRegistry::EVERYTHING);
    auto loaded = base::BindOnce(
        [](Profile* profile, std::shared_ptr<Batch> batch, size_t index,
           std::string icon) {
          batch->icons[index] = std::move(icon);
          if (--batch->remaining == 0) {
            FinishBatch(profile, batch);
          }
        },
        profile, batch, i);
    if (!extension) {
      std::move(loaded).Run(std::string());
      continue;
    }
    LoadIconDataUrl(profile, *extension, std::move(loaded));
  }
}

const char* UnloadReasonName(extensions::UnloadedExtensionReason reason) {
  using extensions::UnloadedExtensionReason;
  switch (reason) {
    case UnloadedExtensionReason::DISABLE:
      return "disabled";
    case UnloadedExtensionReason::UNINSTALL:
      return "uninstalled";
    case UnloadedExtensionReason::UPDATE:
      return "updated";
    case UnloadedExtensionReason::TERMINATE:
      return "terminated";
    case UnloadedExtensionReason::BLOCKLIST:
      return "blocklisted";
    default:
      return "other";
  }
}

}  // namespace bdk
