// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/bundled_extensions.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/barrier_closure.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/one_shot_event.h"
#include "base/task/thread_pool.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/core/runtime/extension_policy.h"
#include "bdk/engine/extensions/extension_support.h"
#include "chrome/browser/profiles/profile.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/unpacked_installer.h"
#include "extensions/common/extension.h"

namespace bdk {
namespace {

// The bundled extensions by the directory Chromium loads them from, as
// declared and as resolved (Chromium records the resolved one).
std::map<base::FilePath, BundledExtension>& Known() {
  static base::NoDestructor<std::map<base::FilePath, BundledExtension>> known;
  return *known;
}

struct Entry {
  BundledExtension extension;
  base::FilePath declared;
  base::FilePath resolved;  // empty when the directory is missing
};

std::vector<Entry> Resolve(std::vector<Entry> entries) {
  for (Entry& entry : entries) {
    entry.resolved = base::DirectoryExists(entry.declared)
                         ? base::MakeAbsoluteFilePath(entry.declared)
                         : base::FilePath();
  }
  return entries;
}

void Load(base::WeakPtr<Profile> profile,
          std::vector<Entry> entries,
          base::OnceClosure done) {
  if (!profile) {
    std::move(done).Run();
    return;
  }
  base::RepeatingClosure one_done =
      base::BarrierClosure(entries.size(), std::move(done));
  for (const Entry& entry : entries) {
    if (entry.resolved.empty()) {
      LOG(ERROR) << "BDK: the bundled extension \"" << entry.extension.name
                 << "\" has no directory at " << entry.declared;
      one_done.Run();
      continue;
    }
    scoped_refptr<extensions::UnpackedInstaller> installer =
        extensions::UnpackedInstaller::Create(profile.get());
    installer->set_be_noisy_on_failure(false);
    installer->set_completion_callback(base::BindOnce(
        [](std::string name, base::RepeatingClosure one_done,
           const extensions::Extension* extension, const base::FilePath&,
           const std::u16string& error) {
          if (!extension) {
            LOG(ERROR) << "BDK: the bundled extension \"" << name
                       << "\" did not load: " << error;
          }
          one_done.Run();
        },
        entry.extension.name, one_done));
    installer->Load(entry.resolved);
  }
}

}  // namespace

void LoadBundledExtensions(Profile* profile, base::OnceClosure done) {
  std::shared_ptr<bdkcore::Runtime> runtime = CurrentRuntime();
  if (!runtime || !runtime->extensions.enabled ||
      runtime->extensions.bundled.empty()) {
    std::move(done).Run();
    return;
  }
  std::vector<Entry> entries;
  for (const bdkcore::ExtensionConfig::Bundled& bundled :
       runtime->extensions.bundled) {
    Entry entry;
    entry.extension.name = bundled.name;
    entry.extension.required = bundled.required;
    entry.declared = base::FilePath::FromUTF8Unsafe(
        bdkcore::ext::bundled_directory(*runtime, bundled));
    entries.push_back(std::move(entry));
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&Resolve, std::move(entries)),
      base::BindOnce(
          [](base::WeakPtr<Profile> profile, base::OnceClosure done,
             std::vector<Entry> entries) {
            for (const Entry& entry : entries) {
              Known()[entry.declared] = entry.extension;
              if (!entry.resolved.empty()) {
                Known()[entry.resolved] = entry.extension;
              }
            }
            extensions::ExtensionSystem* system =
                profile ? extensions::ExtensionSystem::Get(profile.get())
                        : nullptr;
            if (!system) {
              std::move(done).Run();
              return;
            }
            // Chromium's unpacked loader needs the profile's extension
            // service, which is ready a little after the profile.
            system->ready().Post(
                FROM_HERE, base::BindOnce(&Load, profile, std::move(entries),
                                          std::move(done)));
          },
          profile->GetWeakPtr(), std::move(done)));
}

const BundledExtension* FindBundledExtension(const base::FilePath& path) {
  auto it = Known().find(path);
  return it == Known().end() ? nullptr : &it->second;
}

}  // namespace bdk
