// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_DEVICE_CHOOSER_H_
#define BDK_ENGINE_UI_DEVICE_CHOOSER_H_

#include <memory>

#include "base/functional/callback_forward.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace permissions {
class ChooserController;
}  // namespace permissions

namespace bdk {

// The device choosers of WebUSB, WebHID, Web Serial and Web Bluetooth
// (ui-requests.md 5.16, T-UIR-13). Chrome finds the devices with its own
// chooser controller and would show them in a views dialog
// (chrome::ShowDeviceChooserDialog), which no BDK window can parent; BDK shows
// the controller's list as a device_choosers request of the page instead.
//
// The request carries {kind, origin, devices: [{id, name, paired?,
// connected?}], adapterEnabled, scanning}; while it is open each change of
// the list comes as an update with the same fields but kind and origin. A
// device's id stays the same while the chooser is open. The completion
// "select" with {"device": id} (which needs the person's gesture) connects
// the page to that device; any other end cancels, so the page's request
// rejects. The default presenter is BDK's own sheet, which follows the list.
//
// `kind` is "usb", "hid", "serial" or "bluetooth". Returns the closure that
// takes the chooser down, as chrome::ShowDeviceChooserDialog does; running it
// after the chooser ended does nothing.
base::OnceClosure ShowDeviceChooser(
    const char* kind,
    content::RenderFrameHost* frame,
    std::unique_ptr<permissions::ChooserController> controller);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_DEVICE_CHOOSER_H_
