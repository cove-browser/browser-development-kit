// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_BROWSING_DATA_H_
#define BDK_ENGINE_SERVICES_BROWSING_DATA_H_

#include <cstdint>
#include <memory>

#include "base/functional/callback.h"
#include "base/time/time.h"

class Profile;

namespace content {
class BrowsingDataFilterBuilder;
}

namespace bdk {

// Removes data through the profile's BrowsingDataRemover and runs `done` when
// the removal has finished. A null `filter` removes for every site; a null
// `end` removes up to now and beyond.
void RemoveBrowsingData(
    Profile* profile,
    base::Time begin,
    base::Time end,
    uint64_t data_types,
    std::unique_ptr<content::BrowsingDataFilterBuilder> filter,
    base::OnceClosure done);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_BROWSING_DATA_H_
