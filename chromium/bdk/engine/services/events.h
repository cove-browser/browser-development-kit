// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The built-in service events (services.md section 7): what Chromium's history,
// bookmark and download services report, and what command handlers announce,
// delivered through the core's emit_builtin_event to the subscriptions for the
// event's name and profile. Payloads are those of interface/*.json.

#ifndef BDK_ENGINE_SERVICES_EVENTS_H_
#define BDK_ENGINE_SERVICES_EVENTS_H_

#include <memory>
#include <string>

#include "bdk/core/runtime/core_internal.h"

class GURL;
class Profile;

namespace bdk {

// Observes one profile's history, bookmarks and downloads and emits their
// events for it, until destroyed (with the profile's engine data).
class ServiceEvents {
 public:
  virtual ~ServiceEvents() = default;
};

std::unique_ptr<ServiceEvents> CreateServiceEvents(
    Profile* profile,
    std::weak_ptr<bdkcore::Profile> bdk_profile);

// Emits a built-in event for the profile (null for none) on the UI thread.
void EmitEvent(const std::shared_ptr<bdkcore::Profile>& bdk_profile,
               const std::string& name,
               const std::string& payload_json);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_EVENTS_H_
