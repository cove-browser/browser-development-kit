// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_STORE_SOURCE_H_
#define BDK_ENGINE_EXTENSIONS_STORE_SOURCE_H_

#include <string>

namespace content {
class WebContents;
}  // namespace content

namespace bdk {

// The store as a source (T-EXT-09, decision D4). Installs from the store
// work only when [extensions.install] stores lists it: the management policy
// refuses a store package otherwise (management_policy.h). When it is
// enabled, the store's install button says "Add to <browser name>".
//
// The store page decides its own button text; Chromium sends it no browser
// name (webstorePrivate has no such call), so BDK relabels the button in the
// page: a script in an isolated world of the store's origin replaces the
// text "Add to Chrome" inside a button and keeps watching for changes. It
// leaves any other text alone, and does nothing if it finds no such button,
// so installing still works whatever the store renders.

// The origin of the store's own pages.
inline constexpr char kStoreOrigin[] = "https://chromewebstore.google.com";

// The script that relabels, for the label text (pure; tested without a
// store).
std::string StoreRelabelScript(const std::string& label);

// Starts relabelling the store's pages in `contents` when the store is
// enabled. Called for every page the engine creates; calling it again does
// nothing.
void InstallStoreRelabeler(content::WebContents* contents);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_STORE_SOURCE_H_
