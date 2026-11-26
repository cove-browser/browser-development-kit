// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_WEB_APPS_H_
#define BDK_ENGINE_WEB_APPS_H_

#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback_forward.h"
#include "base/values.h"
#include "third_party/blink/public/mojom/manifest/display_mode.mojom-shared.h"

class GURL;
class Profile;

namespace bdkcore {
struct Profile;
}  // namespace bdkcore

namespace blink::mojom {
class Manifest;
}  // namespace blink::mojom

namespace content {
class WebContents;
}  // namespace content

namespace web_app {
struct WebAppInstallInfo;
}  // namespace web_app

namespace bdk {

class ServiceEvents;

// Installable web apps on the engine (web-apps.md, T-PWA-04). Chromium's
// web-app system is used as it is: its registry, installability, installs,
// manifest updates and badges. BDK supplies what is views or operating-system
// UI in Chrome: the install prompt (a web_app_installs request, patch
// bdk-let-an-embedder-present-the-web-app-install-dialog), the app's window
// (a new-window request of kind webApp), and no operating-system integration
// (patch bdk-let-an-embedder-turn-off-web-app-os-integration).

// Installs the hooks, once, as Chromium starts.
void InstallWebAppHooks();

// Whether [web_apps] enabled is set for the running runtime.
bool WebAppsEnabled();

// The installed app `app_id` of `profile` as WebAppInfo JSON (web-apps.md 2),
// or nullopt when the profile has no such app.
std::optional<base::DictValue> InstalledAppInfo(Profile* profile,
                                                const std::string& app_id);
// What a manifest would install, as WebAppInfo (installedAt 0).
base::DictValue ManifestAppInfo(Profile* profile,
                                const blink::mojom::Manifest& manifest);
// What an install info would install, as WebAppInfo (installedAt 0).
base::DictValue InstallInfoAppInfo(Profile* profile,
                                   const web_app::WebAppInstallInfo& info);

// Raises the web_app_installs request for `info` (source "page" or
// "command") on `page_contents` and calls `done` with whether the person
// installed. Default presenter: a sheet with the name, the origin, Install
// and Cancel (Cancel first).
void AskToInstall(content::WebContents* page_contents,
                  const web_app::WebAppInstallInfo& info,
                  const std::string& source,
                  base::OnceCallback<void(bool)> done);

// The observers of a profile's apps: webApps.installed, .updated,
// .uninstalled and .badgeChanged, and the badge delegate.
std::unique_ptr<ServiceEvents> CreateWebAppEvents(
    Profile* profile,
    std::weak_ptr<bdkcore::Profile> bdk_profile);

// An app's page (web-apps.md 4.2): the display mode it reports, and whether
// `url` is in the app's scope.
blink::mojom::DisplayMode AppDisplayMode(Profile* profile,
                                         const std::string& app_id);
bool IsInAppScope(Profile* profile, const std::string& app_id, const GURL& url);

}  // namespace bdk

#endif  // BDK_ENGINE_WEB_APPS_H_
