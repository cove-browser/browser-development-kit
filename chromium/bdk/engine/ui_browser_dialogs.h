// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_BROWSER_DIALOGS_H_
#define BDK_ENGINE_UI_BROWSER_DIALOGS_H_

#include <memory>

#include "base/functional/callback_forward.h"
#include "net/ssl/client_cert_identity.h"

namespace content {
class BluetoothDelegate;
class BrowserContext;
class ClientCertificateDelegate;
class HidDelegate;
class SerialDelegate;
class UsbDelegate;
class WebContents;
}  // namespace content

namespace net {
class SSLCertRequestInfo;
}

namespace bdk {

// Chromium's other browser dialogs on BDK pages (ui-requests.md 8.2,
// T-UIR-10). Chrome shows them as tab-modal views dialogs or bubbles, which
// need a views widget to parent to, and a BDK page's window is not one: each
// ended the process at a check. None of their surfaces is routed to the
// application yet (S3); until then each has a native sheet of BDK's own, is
// refused as the surface's `disabled` mode would refuse it, or is turned off
// at start (chromium_main_mac.mm):
//
// - External protocols (zoommtg:, maps:): Chrome still decides whether to
//   ask (mailto: opens without asking, as in Chrome); the question is a
//   sheet, and the application opens only on "Open".
// - Tab-modal dialog models (chrome::ShowTabModal: File System Access's
//   "view files?", restricted-directory and dangerous-file questions,
//   DevTools' folder permission, extension notices): the model is shown as a
//   sheet and its own callbacks run. A model with a field a sheet cannot show
//   (a text field, a combobox) is closed at once, as a dismissed dialog.
// - Client certificates: a sheet with one button per certificate, after
//   Chrome's automatic selection.
// - Device choosers (WebUSB, WebHID, Web Serial, Web Bluetooth): refused, so
//   requestDevice() and requestPort() reject as if the person cancelled.
// - Web application installs from a page (beforeinstallprompt.prompt()):
//   off, by the default of Chrome's "install by user" preference.
//
// Installs the hooks the patches add. Called once, from InstallPromptHooks.
void InstallBrowserDialogHooks();

// Changes the default of the preferences the hooks need, for every profile.
// Called before Chromium creates its first profile (BdkBrowserMainExtraParts).
void InstallBrowserDialogPrefDefaults();

// The device delegates of BdkContentBrowserClient: Chrome's, with the chooser
// refused.
std::unique_ptr<content::UsbDelegate> CreateUsbDelegate();
std::unique_ptr<content::HidDelegate> CreateHidDelegate();
std::unique_ptr<content::SerialDelegate> CreateSerialDelegate();
std::unique_ptr<content::BluetoothDelegate> CreateBluetoothDelegate();

// BdkContentBrowserClient::SelectClientCertificate: Chrome's automatic
// selection, then the sheet.
base::OnceClosure SelectClientCertificate(
    content::BrowserContext* browser_context,
    content::WebContents* web_contents,
    net::SSLCertRequestInfo* cert_request_info,
    net::ClientCertIdentityList client_certs,
    std::unique_ptr<content::ClientCertificateDelegate> delegate);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_BROWSER_DIALOGS_H_
