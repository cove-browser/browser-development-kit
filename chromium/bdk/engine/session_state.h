// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SESSION_STATE_H_
#define BDK_ENGINE_SESSION_STATE_H_

#include <memory>
#include <string>
#include <vector>

#include "base/containers/span.h"

namespace content {
class BrowserContext;
class NavigationController;
class NavigationEntry;
}  // namespace content

namespace bdk {

// A page's navigation history, serialised for the application to store
// (architecture.md section 4.4). The format is BDK's own envelope around
// Chromium's session-restore records, so a newer format is recognised and
// refused rather than misread.
std::string SerializeNavigation(content::NavigationController& controller);

enum class ParseResult { kOk, kTooNew, kMalformed };

struct RestoredNavigation {
  RestoredNavigation();
  RestoredNavigation(RestoredNavigation&&);
  RestoredNavigation& operator=(RestoredNavigation&&);
  ~RestoredNavigation();

  int current = -1;
  std::vector<std::unique_ptr<content::NavigationEntry>> entries;
};

ParseResult ParseNavigation(base::span<const uint8_t> data,
                            content::BrowserContext* context,
                            RestoredNavigation* out);

}  // namespace bdk

#endif  // BDK_ENGINE_SESSION_STATE_H_
