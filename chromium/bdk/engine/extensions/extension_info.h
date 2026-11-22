// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSION_INFO_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSION_INFO_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "extensions/browser/unloaded_extension_reason.h"
#include "extensions/common/extension_id.h"

class Profile;

namespace extensions {
class Extension;
class ExtensionError;
}  // namespace extensions

namespace gfx {
class Image;
}  // namespace gfx

namespace bdk {

// The JSON of the values of interface/extensions.json that describe an
// installed extension: ExtensionInfo, ExtensionError and the rest, read from
// Chromium's registry, preferences, permissions and error console.

// What BDK records about where an extension came from, in the extension's own
// preferences (they go with it and are deleted with it). Chromium knows the
// location (unpacked, internal) and whether it is from the web store; the
// rest is BDK's.
struct SourceRecord {
  std::string kind;        // ExtensionSourceKind: crx, updateUrl, store
  std::string store;       // the named store, for `store`
  std::string update_url;  // the update manifest, for `updateUrl`
};

// Writes `record` to the extension's preferences. The extension must be
// installed: a preference entry for one that is not looks like a damaged
// installation to Chromium.
void RecordSource(Profile* profile,
                  const extensions::ExtensionId& id,
                  const SourceRecord& record);

// An installation in progress notes where its package came from, and the
// observer that sees the extension installed records it (and announces it,
// so the announcement already has its source). Take returns it once.
void NoteInstallSource(Profile* profile,
                       const extensions::ExtensionId& id,
                       const SourceRecord& record);
std::optional<SourceRecord> TakeInstallSource(
    Profile* profile,
    const extensions::ExtensionId& id);

// The user's pin state of an extension, which BDK keeps itself (the toolbar
// model has the state of an extension that has an action, and
// extensions.setPinned updates it as well).
void RecordPinned(Profile* profile,
                  const extensions::ExtensionId& id,
                  bool pinned);

// The ExtensionSourceKind of an installed extension, and the update manifest
// BDK updates it from ("" when there is none).
std::string SourceKindOf(Profile* profile, const extensions::Extension& ext);
std::string StoreOf(Profile* profile, const extensions::Extension& ext);
std::string UpdateUrlOf(Profile* profile, const extensions::Extension& ext);

// A `data:image/png;base64,...` URL of the image, or "" when it is empty.
std::string IconDataUrl(const gfx::Image& image);

// Loads the extension's icon asynchronously: its data URL, or "" when it has
// none. The callback is not run if the profile goes away first.
void LoadIconDataUrl(Profile* profile,
                     const extensions::Extension& extension,
                     base::OnceCallback<void(std::string)> callback);

// The ExtensionInfo of an extension Chromium holds (enabled, disabled or
// blocklisted). `errors` adds its error list (extensions.get).
std::string ExtensionInfoJson(Profile* profile,
                              const extensions::Extension& extension,
                              const std::string& icon_data_url,
                              bool include_errors);

// The ExtensionInfo of each extension of `ids` that is still installed when
// its icon has loaded, in order, answered once. With no `ids` it is every
// extension a person can see (the ones chrome://extensions lists).
void BuildExtensionInfos(
    Profile* profile,
    std::vector<extensions::ExtensionId> ids,
    bool all,
    bool include_errors,
    base::OnceCallback<void(std::vector<std::string>)> done);

// The extensions a person can see, enabled or not.
std::vector<extensions::ExtensionId> VisibleExtensionIds(Profile* profile);

// The ExtensionDisableReason names of an extension's disable reasons, without
// repeats ("blocklisted" for a blocklisted one).
std::vector<std::string> DisableReasonNames(Profile* profile,
                                            const extensions::ExtensionId& id);
std::string DisableReasonsJson(Profile* profile,
                               const extensions::ExtensionId& id);

// An ExtensionError of the interface. An InternalError that starts with
// kUnavailableApiPrefix is an `unavailableApi` error, the one BDK records when
// it withholds an API (management_policy.h).
std::string ExtensionErrorJson(const extensions::ExtensionError& error);
inline constexpr char kUnavailableApiPrefix[] = "[bdk:unavailableApi] ";

// The ExtensionUnloadReason name of Chromium's unload reason.
const char* UnloadReasonName(extensions::UnloadedExtensionReason reason);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSION_INFO_H_
