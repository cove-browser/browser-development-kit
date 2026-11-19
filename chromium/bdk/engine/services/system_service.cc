// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// system.defaultBrowser.*: the operating system answers through the platform
// adapter (system_platform.h).

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "bdk/engine/services/services.h"
#include "bdk/engine/services/system_platform.h"

namespace bdk {

namespace {

void Status(const std::shared_ptr<ServiceCall>& c) {
  GetDefaultBrowserStatus(base::BindOnce(
      [](std::shared_ptr<ServiceCall> c, DefaultBrowserStatus status) {
        c->Resolve(std::string("{\"isDefault\":") +
                   (status.is_default ? "true" : "false") +
                   ",\"canBeDefault\":" +
                   (status.can_be_default ? "true" : "false") + "}");
      },
      c));
}

void Request(const std::shared_ptr<ServiceCall>& c) {
  GetDefaultBrowserStatus(base::BindOnce(
      [](std::shared_ptr<ServiceCall> c, DefaultBrowserStatus status) {
        if (!status.can_be_default) {
          c->Reject(BDK_DOMAIN_PLATFORM, "cannotBeDefault",
                    "the application does not declare http and https");
          return;
        }
        RequestDefaultBrowser(base::BindOnce(
            [](std::shared_ptr<ServiceCall> c, std::string error) {
              if (error.empty()) {
                c->ResolveNull();
              } else {
                c->Reject(BDK_DOMAIN_PLATFORM, "defaultBrowserFailed", error);
              }
            },
            c));
      },
      c));
}

}  // namespace

void AddSystemHandlers(HandlerTable& table) {
  table["system.defaultBrowser.status"] = &Status;
  table["system.defaultBrowser.request"] = &Request;
}

}  // namespace bdk
