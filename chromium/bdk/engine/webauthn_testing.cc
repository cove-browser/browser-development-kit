// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/webauthn_testing.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/scoped_authenticator_environment_for_testing.h"
#include "device/fido/fido_device_discovery.h"
#include "device/fido/fido_discovery_factory.h"
#include "device/fido/public/fido_transport_protocol.h"
#include "device/fido/virtual_ctap2_device.h"
#include "device/fido/virtual_fido_device.h"
#include "device/fido/virtual_fido_device_authenticator.h"

namespace bdk {

namespace {

// One virtual key, found as soon as a request looks for USB keys. It is
// pressed as soon as it is asked to be: the person touches it at once.
class VirtualKeyDiscovery : public device::FidoDeviceDiscovery {
 public:
  VirtualKeyDiscovery(scoped_refptr<device::VirtualFidoDevice::State> state,
                      device::VirtualCtap2Device::Config config)
      : FidoDeviceDiscovery(
            device::FidoTransportProtocol::kUsbHumanInterfaceDevice),
        state_(std::move(state)),
        config_(std::move(config)) {}

 private:
  void StartInternal() override {
    AddAuthenticator(std::make_unique<device::VirtualFidoDeviceAuthenticator>(
        std::make_unique<device::VirtualCtap2Device>(state_, config_)));
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&VirtualKeyDiscovery::NotifyDiscoveryStarted,
                                  weak_factory_.GetWeakPtr(), true));
  }

  scoped_refptr<device::VirtualFidoDevice::State> state_;
  const device::VirtualCtap2Device::Config config_;
  base::WeakPtrFactory<VirtualKeyDiscovery> weak_factory_{this};
};

// Only the virtual key: no phone, no platform authenticator.
class VirtualKeyFactory : public device::FidoDiscoveryFactory {
 public:
  VirtualKeyFactory(scoped_refptr<device::VirtualFidoDevice::State> state,
                    device::VirtualCtap2Device::Config config)
      : state_(std::move(state)), config_(std::move(config)) {}

  std::vector<std::unique_ptr<device::FidoDiscoveryBase>> Create(
      device::FidoTransportProtocol transport) override {
    if (transport != device::FidoTransportProtocol::kUsbHumanInterfaceDevice) {
      return {};
    }
    return SingleDiscovery(
        std::make_unique<VirtualKeyDiscovery>(state_, config_));
  }

 private:
  scoped_refptr<device::VirtualFidoDevice::State> state_;
  const device::VirtualCtap2Device::Config config_;
};

struct VirtualKey {
  scoped_refptr<device::VirtualFidoDevice::State> state;
  std::unique_ptr<content::ScopedAuthenticatorEnvironmentForTesting>
      environment;
};

std::optional<VirtualKey>& Key() {
  static base::NoDestructor<std::optional<VirtualKey>> key;
  return *key;
}

}  // namespace

bool RunWebAuthnTestingCommand(const std::string& name,
                               const base::DictValue& args,
                               base::DictValue& result) {
  if (name == "webauthn.virtual") {
    // {"pin"?, "residentKeys"?, "uv"?}: every request from now on sees one
    // virtual CTAP2 security key, with that PIN set (a PIN makes it ask for
    // one), holding passkeys, verifying its user itself.
    Key().reset();  // the old environment goes before the new one is made
    auto state = base::MakeRefCounted<device::VirtualFidoDevice::State>();
    device::VirtualCtap2Device::Config config;
    if (const std::string* pin = args.FindString("pin")) {
      config.pin_support = true;
      state->pin = *pin;
    }
    config.resident_key_support = args.FindBool("residentKeys").value_or(false);
    if (args.FindBool("uv").value_or(false)) {
      // A key that verifies its user itself (a fingerprint), as a key that
      // signs in with a discoverable credential must.
      config.internal_uv_support = true;
      state->fingerprints_enrolled = true;
    }
    auto environment =
        std::make_unique<content::ScopedAuthenticatorEnvironmentForTesting>(
            std::make_unique<VirtualKeyFactory>(state, config));
    Key() = VirtualKey{std::move(state), std::move(environment)};
    return true;
  }
  if (name == "webauthn.inject") {
    // {"rpId", "userId", "name", "displayName"}: a passkey on the virtual key.
    const std::string* rp = args.FindString("rpId");
    const std::string* user = args.FindString("userId");
    if (!Key() || !rp || !user) {
      return false;
    }
    const std::string* user_name = args.FindString("name");
    const std::string* display = args.FindString("displayName");
    const std::vector<uint8_t> credential_id(user->begin(), user->end());
    result.Set(
        "injected",
        Key()->state->InjectResidentKey(
            base::span(credential_id), *rp, base::as_byte_span(*user),
            user_name ? std::optional<std::string>(*user_name) : std::nullopt,
            display ? std::optional<std::string>(*display) : std::nullopt));
    return true;
  }
  if (name == "webauthn.state") {
    // -> {"installed", "credentials"}: the passkeys the virtual key holds.
    result.Set("installed", Key().has_value());
    result.Set(
        "credentials",
        Key() ? static_cast<int>(Key()->state->registrations.size()) : 0);
    return true;
  }
  if (name == "webauthn.clear") {
    Key().reset();
    return true;
  }
  return false;
}

}  // namespace bdk
