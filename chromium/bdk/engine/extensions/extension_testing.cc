// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_testing.h"

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/threading/thread_restrictions.h"
#include "bdk/engine/extensions/extension_fetch.h"
#include "components/crx_file/crx_verifier.h"
#include "extensions/browser/extension_creator.h"

namespace bdk {

bool RunExtensionTestingCommand(const std::string& name,
                                const base::DictValue& args,
                                base::DictValue& result) {
  if (name == "extensions.pack") {
    // {"dir", "crx", "key"}: packs the directory with Chromium's
    // ExtensionCreator into the CRX file, signed with the key in the file
    // `key` (made there when it does not exist), and answers {"id"}. Blocks
    // the UI thread: a test hook.
    const std::string* dir = args.FindString("dir");
    const std::string* crx = args.FindString("crx");
    const std::string* key = args.FindString("key");
    if (!dir || !crx || !key) {
      return false;
    }
    base::ScopedAllowBlockingForTesting allow_blocking;
    const base::FilePath key_path = base::FilePath::FromUTF8Unsafe(*key);
    const bool have_key = base::PathExists(key_path);
    // Chromium reads no file through a path with "..", which the fixtures'
    // path has.
    const base::FilePath source =
        base::MakeAbsoluteFilePath(base::FilePath::FromUTF8Unsafe(*dir));
    if (source.empty()) {
      return false;
    }
    extensions::ExtensionCreator creator;
    if (!creator.Run(source, base::FilePath::FromUTF8Unsafe(*crx),
                     have_key ? key_path : base::FilePath(),
                     have_key ? base::FilePath() : key_path,
                     extensions::ExtensionCreator::kOverwriteCRX)) {
      return false;
    }
    std::string public_key;
    std::string id;
    if (crx_file::Verify(base::FilePath::FromUTF8Unsafe(*crx),
                         crx_file::VerifierFormat::CRX3, {}, {}, &public_key,
                         &id, nullptr) != crx_file::VerifierResult::OK_FULL) {
      return false;
    }
    result.Set("id", id);
    return true;
  }
  if (name == "extensions.serve") {
    // {"url", "body"} or {"url", "file"}: a fetch of the url (an update
    // manifest, a package) is answered from the engine, not the network.
    // Neither removes the entry.
    const std::string* url = args.FindString("url");
    if (!url) {
      return false;
    }
    const std::string* body = args.FindString("body");
    const std::string* file = args.FindString("file");
    ServeFetchForTesting(
        *url, body ? *body : std::string(),
        file ? base::FilePath::FromUTF8Unsafe(*file) : base::FilePath());
    return true;
  }
  return false;
}

}  // namespace bdk
