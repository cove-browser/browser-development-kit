// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_PAGE_CALL_H_
#define BDK_ENGINE_SERVICES_PAGE_CALL_H_

#include <memory>

#include "bdk/engine/services/service_call.h"

namespace content {
class WebContents;
}

namespace bdk {

// The page a call addresses (the invoke options' `contents`), for the commands
// that act on one. Null, with the call rejected, when there is none
// (broker.invalidArguments), the page is gone (contents.notFound) or it is an
// internal page (contents.internalPage: BDK never scripts or debugs those).
content::WebContents* LivePage(const std::shared_ptr<ServiceCall>& c);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_PAGE_CALL_H_
