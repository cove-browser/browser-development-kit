// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSION_FETCH_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSION_FETCH_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"

class GURL;
class Profile;

namespace bdk {

// The two downloads an install from an update URL or a store makes: the
// update manifest (small XML) and the package (a CRX, into a temporary file).
// Both go through the profile's network service, as Chromium's own updater
// does, and follow redirects (the store answers a redirect to the package).

// The body, or nothing on any failure (a network error, a non-success status,
// more than `max_size` bytes).
using FetchStringCallback =
    base::OnceCallback<void(std::optional<std::string>)>;
void FetchString(Profile* profile,
                 const GURL& url,
                 size_t max_size,
                 FetchStringCallback callback);

// The temporary file the body was written to, or an empty path on failure.
// The caller owns the file and deletes it.
using FetchFileCallback = base::OnceCallback<void(base::FilePath)>;
void FetchToFile(Profile* profile, const GURL& url, FetchFileCallback callback);

// For tests (bdk_testing_command "extensions.serve"): from now on a fetch of
// `url` answers `body`, or the bytes of the file `file` (copied to the
// temporary file a fetch to a file returns), without the network. An empty
// `body` and `file` removes the entry. Production code never sets one.
void ServeFetchForTesting(const std::string& url,
                          const std::string& body,
                          const base::FilePath& file);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSION_FETCH_H_
