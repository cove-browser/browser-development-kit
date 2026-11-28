// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_CLIENT_CERT_TESTING_H_
#define BDK_ENGINE_CLIENT_CERT_TESTING_H_

#include <memory>
#include <string>

#include "base/values.h"

namespace net {
class ClientCertStore;
}  // namespace net

namespace bdk {

// A client identity for the engine scenarios (T-UIR-13), so that a scenario
// reaches the client_certificates request without a certificate in the
// person's Keychain: "clientcert.identity" with {"certificate", "key"} (PEM)
// makes it the one identity every profile offers, and with {} goes back to
// the Keychain. A testing command (bdk_testing.h), never set by applications.
bool RunClientCertTestingCommand(const std::string& name,
                                 const base::DictValue& args,
                                 base::DictValue& result);

// The store of the identity above, or null when none is set.
std::unique_ptr<net::ClientCertStore> CreateTestingClientCertStore();

}  // namespace bdk

#endif  // BDK_ENGINE_CLIENT_CERT_TESTING_H_
