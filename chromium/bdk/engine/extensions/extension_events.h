// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSION_EVENTS_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSION_EVENTS_H_

#include <memory>

#include "bdk/engine/services/events.h"

class Profile;

namespace bdk {

// The extension events of a persistent profile (interface/extensions.json):
// installed, updated, uninstalled, enabledChanged, loaded, unloaded and
// errorAdded, from Chromium's extension registry, preferences and error
// console, through the core's emit_builtin_event like the history events. It
// also owns what BDK registers with Chromium for the profile's extensions (the
// management policy provider and the load-time enforcement,
// management_policy.h) and removes it all when destroyed with the profile's
// engine data. Null for a private profile: extensions live in the original
// profile, and its events are the original's.
std::unique_ptr<ServiceEvents> CreateExtensionEvents(
    Profile* profile,
    std::weak_ptr<bdkcore::Profile> bdk_profile);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSION_EVENTS_H_
