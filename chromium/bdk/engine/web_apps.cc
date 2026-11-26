// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/web_apps.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/services/events.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/badging/badge_manager.h"
#include "chrome/browser/badging/badge_manager_delegate.h"
#include "chrome/browser/badging/badge_manager_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/push_messaging/embedder_push_endpoint.h"  // nogncheck
#include "chrome/browser/ui/web_applications/embedder_web_app_install_dialog.h"  // nogncheck
#include "chrome/browser/web_applications/os_integration/embedder_web_app_os_integration.h"  // nogncheck
#include "chrome/browser/web_applications/web_app_helpers.h"
#include "chrome/browser/web_applications/web_app_install_info.h"
#include "chrome/browser/web_applications/web_app_install_manager.h"
#include "chrome/browser/web_applications/web_app_install_manager_observer.h"
#include "chrome/browser/web_applications/web_app_provider.h"
#include "chrome/browser/web_applications/web_app_registrar.h"
#include "components/version_info/channel.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/manifest/manifest.mojom.h"
#include "third_party/skia/include/core/SkColor.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {

namespace {

std::string ColorString(std::optional<SkColor> color) {
  return color ? base::StringPrintf("#%02x%02x%02x", SkColorGetR(*color),
                                    SkColorGetG(*color), SkColorGetB(*color))
               : std::string();
}

void SetColor(base::DictValue& app, const char* key, std::optional<SkColor> c) {
  if (c) {
    app.Set(key, ColorString(c));
  } else {
    app.Set(key, base::Value());
  }
}

const char* DisplayModeName(blink::mojom::DisplayMode mode) {
  return mode == blink::mojom::DisplayMode::kMinimalUi ? "minimalUi"
                                                       : "standalone";
}

const char* PurposeName(apps::IconInfo::Purpose purpose) {
  switch (purpose) {
    case apps::IconInfo::Purpose::kMonochrome:
      return "monochrome";
    case apps::IconInfo::Purpose::kMaskable:
      return "maskable";
    case apps::IconInfo::Purpose::kAny:
      break;
  }
  return "any";
}

base::ListValue IconsJson(const std::vector<apps::IconInfo>& icons) {
  base::ListValue list;
  for (const apps::IconInfo& icon : icons) {
    base::DictValue entry;
    entry.Set("url", icon.url.spec());
    if (icon.square_size_px) {
      entry.Set("width", *icon.square_size_px);
      entry.Set("height", *icon.square_size_px);
    }
    entry.Set("purpose", PurposeName(icon.purpose));
    list.Append(std::move(entry));
  }
  return list;
}

base::ListValue ShortcutsJson(
    const std::vector<web_app::WebAppShortcutsMenuItemInfo>& shortcuts) {
  base::ListValue list;
  for (const web_app::WebAppShortcutsMenuItemInfo& shortcut : shortcuts) {
    base::DictValue entry;
    entry.Set("name", base::UTF16ToUTF8(shortcut.name));
    entry.Set("url", shortcut.url.spec());
    base::ListValue icons;
    for (const auto& icon :
         shortcut.GetShortcutIconInfosForPurpose(web_app::IconPurpose::ANY)) {
      icons.Append(base::DictValue()
                       .Set("url", icon.url.spec())
                       .Set("width", icon.square_size_px)
                       .Set("height", icon.square_size_px)
                       .Set("purpose", "any"));
    }
    entry.Set("icons", std::move(icons));
    list.Append(std::move(entry));
  }
  return list;
}

std::string ProfileId(Profile* profile) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  std::shared_ptr<bdkcore::Profile> p =
      rt ? BdkProfileOf(*rt, profile) : nullptr;
  return p ? p->id : std::string();
}

base::Value BadgeJson(Profile* profile, const std::string& app_id) {
  badging::BadgeManager* badges =
      badging::BadgeManagerFactory::GetForProfile(profile);
  std::optional<badging::BadgeManager::BadgeValue> badge =
      badges ? badges->GetBadgeValue(app_id) : std::nullopt;
  if (!badge) {
    return base::Value();
  }
  base::DictValue value;
  if (*badge) {
    value.Set("kind", "count");
    value.Set("count", static_cast<int>(**badge));
  } else {
    value.Set("kind", "flag");
  }
  return base::Value(std::move(value));
}

base::DictValue AppInfo(Profile* profile,
                        const std::string& app_id,
                        const std::string& name,
                        const std::string& short_name,
                        const GURL& start_url,
                        const GURL& scope,
                        blink::mojom::DisplayMode display_mode,
                        std::optional<SkColor> theme_color,
                        std::optional<SkColor> background_color,
                        base::ListValue icons,
                        base::ListValue shortcuts) {
  base::DictValue app;
  app.Set("id", app_id);
  app.Set("profile", ProfileId(profile));
  app.Set("name", name);
  if (short_name.empty()) {
    app.Set("shortName", base::Value());
  } else {
    app.Set("shortName", short_name);
  }
  app.Set("startUrl", start_url.spec());
  app.Set("scope", scope.spec());
  app.Set("origin", url::Origin::Create(start_url).Serialize());
  app.Set("displayMode", DisplayModeName(display_mode));
  SetColor(app, "themeColor", theme_color);
  SetColor(app, "backgroundColor", background_color);
  app.Set("icons", std::move(icons));
  app.Set("shortcuts", std::move(shortcuts));
  app.Set("badge", base::Value());
  app.Set("installedAt", 0);
  app.Set("hasPendingUpdate", false);
  return app;
}

web_app::WebAppProvider* ProviderOf(Profile* profile) {
  return profile ? web_app::WebAppProvider::GetForWebApps(profile) : nullptr;
}

// The install prompt of a page's prompt() (the patch's presenter).
void PresentPageInstall(content::WebContents* initiator,
                        std::unique_ptr<web_app::WebAppInstallInfo> info,
                        web_app::WebAppInstallationAcceptanceCallback accept) {
  web_app::WebAppInstallInfo* raw = info.get();
  AskToInstall(Profile::FromBrowserContext(initiator->GetBrowserContext()),
               initiator, *raw, "page",
               base::BindOnce(
                   [](std::unique_ptr<web_app::WebAppInstallInfo> info,
                      web_app::WebAppInstallationAcceptanceCallback accept,
                      bool installed) {
                     // BDK opens no window of Chrome's after the install.
                     std::move(accept).Run(installed, std::move(info),
                                           base::DoNothing());
                   },
                   std::move(info), std::move(accept)));
}

// Emits a profile's app events, and reports its badges.
class WebAppEvents : public ServiceEvents,
                     public web_app::WebAppInstallManagerObserver {
 public:
  WebAppEvents(Profile* profile, std::weak_ptr<bdkcore::Profile> bdk_profile)
      : profile_(profile), bdk_profile_(std::move(bdk_profile)) {
    web_app::WebAppProvider* provider = ProviderOf(profile_);
    if (!provider) {
      return;
    }
    observation_.Observe(&provider->install_manager());
    if (badging::BadgeManager* badges =
            badging::BadgeManagerFactory::GetForProfile(profile_)) {
      badges->SetDelegate(
          std::make_unique<BadgeDelegate>(profile_, badges, bdk_profile_));
    }
  }

  // web_app::WebAppInstallManagerObserver:
  void OnWebAppInstalled(const webapps::AppId& app_id) override {
    Emit("webApps.installed", app_id);
  }
  void OnWebAppManifestUpdated(const webapps::AppId& app_id) override {
    Emit("webApps.updated", app_id);
  }
  void OnWebAppUninstalled(
      const webapps::AppId& app_id,
      webapps::WebappUninstallSource uninstall_source) override {
    EmitEvent(bdk_profile_, "webApps.uninstalled",
              ToJson(base::DictValue().Set("id", app_id)));
  }
  void OnWebAppInstallManagerDestroyed() override { observation_.Reset(); }

 private:
  class BadgeDelegate : public badging::BadgeManagerDelegate {
   public:
    BadgeDelegate(Profile* profile,
                  badging::BadgeManager* badges,
                  std::weak_ptr<bdkcore::Profile> bdk_profile)
        : badging::BadgeManagerDelegate(profile, badges),
          bdk_profile_(std::move(bdk_profile)) {}

    void OnAppBadgeUpdated(const webapps::AppId& app_id) override {
      EmitEvent(bdk_profile_, "webApps.badgeChanged",
                ToJson(base::DictValue()
                           .Set("id", app_id)
                           .Set("badge", BadgeJson(profile(), app_id))));
    }

   private:
    std::weak_ptr<bdkcore::Profile> bdk_profile_;
  };

  void Emit(const char* name, const webapps::AppId& app_id) {
    if (std::optional<base::DictValue> app =
            InstalledAppInfo(profile_, app_id)) {
      EmitEvent(bdk_profile_, name,
                ToJson(base::DictValue().Set("app", std::move(*app))));
    }
  }

  raw_ptr<Profile> profile_;
  std::weak_ptr<bdkcore::Profile> bdk_profile_;
  base::ScopedObservation<web_app::WebAppInstallManager,
                          web_app::WebAppInstallManagerObserver>
      observation_{this};
};

}  // namespace

void InstallWebAppHooks() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  embedder_web_app_install_dialog::SetPresenter(&PresentPageInstall);
  // Operating-system integration is not in v1 (web-apps.md 8).
  embedder_web_app_os_integration::TurnOff();
  // An unbranded build has no channel, and would give pages the push service's
  // staging endpoint, which no push reaches (web-apps.md 7, T-PWA-05).
  embedder_push_endpoint::SetChannel(version_info::Channel::STABLE);
}

bool WebAppsEnabled() {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  return rt && rt->web_apps.enabled;
}

std::optional<base::DictValue> InstalledAppInfo(Profile* profile,
                                                const std::string& app_id) {
  web_app::WebAppProvider* provider = ProviderOf(profile);
  if (!provider) {
    return std::nullopt;
  }
  const web_app::WebAppRegistrar& registrar = provider->registrar_unsafe();
  if (!registrar.AppMatches(app_id,
                            web_app::WebAppFilter::InstalledInChrome())) {
    return std::nullopt;
  }
  base::DictValue app =
      AppInfo(profile, app_id, registrar.GetAppShortName(app_id), std::string(),
              registrar.GetAppStartUrl(app_id), registrar.GetAppScope(app_id),
              registrar.GetAppEffectiveDisplayMode(app_id),
              registrar.GetAppThemeColor(app_id),
              registrar.GetAppBackgroundColor(app_id),
              IconsJson(registrar.GetAppIconInfos(app_id)),
              ShortcutsJson(registrar.GetAppShortcutsMenuItemInfos(app_id)));
  app.Set("badge", BadgeJson(profile, app_id));
  app.Set(
      "installedAt",
      registrar.GetAppFirstInstallTime(app_id).InMillisecondsFSinceUnixEpoch());
  return app;
}

base::DictValue ManifestAppInfo(Profile* profile,
                                const blink::mojom::Manifest& manifest) {
  std::vector<apps::IconInfo> icons;
  for (const auto& image : manifest.icons) {
    apps::IconInfo icon;
    icon.url = image.src;
    if (!image.sizes.empty() && image.sizes.front().width() > 0) {
      icon.square_size_px = image.sizes.front().width();
    }
    if (!image.purpose.empty() &&
        image.purpose.front() ==
            blink::mojom::ManifestImageResource_Purpose::MASKABLE) {
      icon.purpose = apps::IconInfo::Purpose::kMaskable;
    }
    icons.push_back(std::move(icon));
  }
  base::ListValue shortcuts;
  for (const auto& shortcut : manifest.shortcuts) {
    shortcuts.Append(base::DictValue()
                         .Set("name", base::UTF16ToUTF8(shortcut.name))
                         .Set("url", shortcut.url.spec())
                         .Set("icons", base::ListValue()));
  }
  const std::string name =
      base::UTF16ToUTF8(manifest.name.value_or(std::u16string()));
  const std::string app_id = manifest.id.is_valid()
                                 ? web_app::GenerateAppIdFromManifestId(
                                       webapps::ManifestId(manifest.id))
                                 : std::string();
  return AppInfo(profile, app_id, name,
                 base::UTF16ToUTF8(manifest.short_name.value_or(u"")),
                 manifest.start_url, manifest.scope, manifest.display,
                 manifest.theme_color
                     ? std::optional<SkColor>(*manifest.theme_color)
                     : std::nullopt,
                 manifest.background_color
                     ? std::optional<SkColor>(*manifest.background_color)
                     : std::nullopt,
                 IconsJson(icons), std::move(shortcuts));
}

base::DictValue InstallInfoAppInfo(Profile* profile,
                                   const web_app::WebAppInstallInfo& info) {
  return AppInfo(
      profile, web_app::GenerateAppIdFromManifestId(info.manifest_id()),
      base::UTF16ToUTF8(info.title.value()), std::string(), info.start_url(),
      info.scope, info.display_mode, info.theme_color, info.background_color,
      IconsJson(info.manifest_icons),
      ShortcutsJson(info.shortcuts_menu_item_infos));
}

void AskToInstall(Profile* profile,
                  content::WebContents* page_contents,
                  const web_app::WebAppInstallInfo& info,
                  const std::string& source,
                  base::OnceCallback<void(bool)> done) {
  EngineContents* page =
      page_contents ? EngineContents::FromWebContents(page_contents) : nullptr;
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  std::shared_ptr<bdkcore::Profile> bdk_profile =
      rt && profile ? BdkProfileOf(*rt, profile) : nullptr;
  if ((!page && !bdk_profile) || !WebAppsEnabled()) {
    std::move(done).Run(false);
    return;
  }
  base::DictValue app = InstallInfoAppInfo(profile, info);
  base::DictValue payload;
  payload.Set("app", app.Clone());
  payload.Set("source", source);
  payload.Set("screenshots", base::ListValue());
  base::WeakPtr<EngineContents> weak_page =
      page ? page->GetWeakPtr() : base::WeakPtr<EngineContents>();
  UiFinish finish = base::BindOnce(
      [](base::WeakPtr<EngineContents> page, base::DictValue app,
         base::OnceCallback<void(bool)> done, const UiResult& result) {
        if (result.outcome == bdkcore::UiOutcome::kCompleted) {
          std::move(done).Run(result.completion == "grant" ||
                              result.completion == "install");
          return;
        }
        if (result.outcome != bdkcore::UiOutcome::kDefaulted || !page) {
          std::move(done).Run(false);
          return;
        }
        // The default presenter (web-apps.md 4.1): the name and the
        // origin, which the site does not choose, with Cancel first.
        Alert alert;
        alert.title = base::StringPrintf(
            "Install “%s”?",
            app.FindString("name") ? app.FindString("name")->c_str() : "");
        alert.message = base::StringPrintf(
            "From %s",
            app.FindString("origin") ? app.FindString("origin")->c_str() : "");
        alert.buttons = {{"cancel", "Cancel"}, {"install", "Install"}};
        UiPresenters::Get()
            .ShowAlert("web_app_installs", &page->host_view(), std::move(alert),
                       base::BindOnce(
                           [](base::OnceCallback<void(bool)> done, int button) {
                             std::move(done).Run(button == 1);
                           },
                           std::move(done)))
            .Reset();
      },
      weak_page, std::move(app), std::move(done));
  // A page's request is anchored on it; an install from a URL that no page
  // asked for belongs to the profile, and its default (no page to show a
  // sheet on) installs nothing.
  std::shared_ptr<bdkcore::Request> request =
      page ? RaiseUiRequest(*page, BDK_SURFACE_WEB_APP_INSTALLS, payload,
                            std::move(finish))
           : RaiseUiRequest(bdk_profile, BDK_SURFACE_WEB_APP_INSTALLS, payload,
                            std::move(finish));
  (void)request;
}

std::unique_ptr<ServiceEvents> CreateWebAppEvents(
    Profile* profile,
    std::weak_ptr<bdkcore::Profile> bdk_profile) {
  if (!profile || profile->IsOffTheRecord() || !ProviderOf(profile)) {
    return nullptr;
  }
  return std::make_unique<WebAppEvents>(profile, std::move(bdk_profile));
}

blink::mojom::DisplayMode AppDisplayMode(Profile* profile,
                                         const std::string& app_id) {
  web_app::WebAppProvider* provider = ProviderOf(profile);
  if (!provider) {
    return blink::mojom::DisplayMode::kBrowser;
  }
  blink::mojom::DisplayMode mode =
      provider->registrar_unsafe().GetAppEffectiveDisplayMode(app_id);
  return mode == blink::mojom::DisplayMode::kMinimalUi
             ? blink::mojom::DisplayMode::kMinimalUi
             : blink::mojom::DisplayMode::kStandalone;
}

bool IsInAppScope(Profile* profile,
                  const std::string& app_id,
                  const GURL& url) {
  web_app::WebAppProvider* provider = ProviderOf(profile);
  return provider && provider->registrar_unsafe().IsUrlInAppScope(url, app_id);
}

}  // namespace bdk
