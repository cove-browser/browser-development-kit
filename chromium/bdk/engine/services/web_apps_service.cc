// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The webApps.* commands on the engine (web-apps.md 3, T-PWA-04). The core
// refuses first what no runtime may do (web apps off, a private profile:
// runtime/web_apps.cc); these handlers see only calls it lets through.

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/services/page_call.h"
#include "bdk/engine/services/service_call.h"
#include "bdk/engine/services/services.h"
#include "bdk/engine/web_apps.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/web_applications/web_app_command_scheduler.h"
#include "chrome/browser/web_applications/web_app_install_info.h"
#include "chrome/browser/web_applications/web_app_install_params.h"
#include "chrome/browser/web_applications/web_app_provider.h"
#include "chrome/browser/web_applications/web_app_registrar.h"
#include "components/webapps/browser/install_result_code.h"
#include "components/webapps/browser/installable/installable_data.h"
#include "components/webapps/browser/installable/installable_logging.h"
#include "components/webapps/browser/installable/installable_manager.h"
#include "components/webapps/browser/installable/installable_params.h"
#include "components/webapps/browser/uninstall_result_code.h"
#include "components/webapps/common/web_app_id.h"
#include "content/public/browser/browsing_data_filter_builder.h"
#include "content/public/browser/browsing_data_remover.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/mojom/manifest/manifest.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {

namespace {

web_app::WebAppProvider* ProviderOf(const std::shared_ptr<ServiceCall>& call) {
  return call->profile()
             ? web_app::WebAppProvider::GetForWebApps(call->profile())
             : nullptr;
}

// Waits for the profile's app registry, then runs `run`.
void WithRegistry(const std::shared_ptr<ServiceCall>& call,
                  base::OnceCallback<void(web_app::WebAppProvider&)> run) {
  web_app::WebAppProvider* provider = ProviderOf(call);
  if (!provider) {
    call->Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_FOUND,
                 "this profile has no web apps");
    return;
  }
  provider->on_registry_ready().Post(
      FROM_HERE,
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> call, Profile* profile,
             base::OnceCallback<void(web_app::WebAppProvider&)> run) {
            if (call->cancelled()) {
              return;
            }
            if (web_app::WebAppProvider* provider =
                    web_app::WebAppProvider::GetForWebApps(profile)) {
              std::move(run).Run(*provider);
            }
          },
          call, call->profile(), std::move(run)));
}

void HandleList(const std::shared_ptr<ServiceCall>& call) {
  WithRegistry(call, base::BindOnce(
                         [](std::shared_ptr<ServiceCall> call,
                            web_app::WebAppProvider& provider) {
                           base::ListValue apps;
                           for (const webapps::AppId& id :
                                provider.registrar_unsafe().GetAppIds()) {
                             if (std::optional<base::DictValue> app =
                                     InstalledAppInfo(call->profile(), id)) {
                               apps.Append(std::move(*app));
                             }
                           }
                           call->Resolve(ToJson(apps));
                         },
                         call));
}

void HandleGet(const std::shared_ptr<ServiceCall>& call) {
  const std::string id = call->StringOr("id", "");
  WithRegistry(call, base::BindOnce(
                         [](std::shared_ptr<ServiceCall> call, std::string id,
                            web_app::WebAppProvider&) {
                           if (std::optional<base::DictValue> app =
                                   InstalledAppInfo(call->profile(), id)) {
                             call->Resolve(ToJson(*app));
                             return;
                           }
                           call->Reject(
                               BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_FOUND,
                               "no web app '" + id + "' in this profile");
                         },
                         call, id));
}

// What the page would install: Chromium's installability check.
void Installability(content::WebContents* page,
                    base::OnceCallback<void(base::DictValue)> done) {
  webapps::InstallableManager::CreateForWebContents(page);
  webapps::InstallableManager* manager =
      webapps::InstallableManager::FromWebContents(page);
  webapps::InstallableParams params;
  params.valid_primary_icon = true;
  params.installable_criteria =
      webapps::InstallableCriteria::kValidManifestWithIcons;
  params.check_eligibility = true;
  Profile* profile = Profile::FromBrowserContext(page->GetBrowserContext());
  manager->GetData(
      params,
      base::BindOnce(
          [](Profile* profile, base::OnceCallback<void(base::DictValue)> done,
             const webapps::InstallableData& data) {
            base::DictValue result;
            const bool installable =
                data.installable_check_passed && data.errors.empty();
            result.Set("isInstallable", installable);
            base::ListValue errors;
            for (webapps::InstallableStatusCode code : data.errors) {
              errors.Append(webapps::GetErrorMessage(code));
            }
            result.Set("errors", std::move(errors));
            const blink::mojom::Manifest& manifest = *data.manifest;
            const bool has_manifest = !manifest.start_url.is_empty();
            if (has_manifest) {
              base::DictValue app = ManifestAppInfo(profile, manifest);
              const std::string id = *app.FindString("id");
              std::optional<base::DictValue> installed =
                  InstalledAppInfo(profile, id);
              result.Set("installedId",
                         installed ? base::Value(id) : base::Value());
              result.Set("manifest", std::move(app));
            } else {
              result.Set("installedId", base::Value());
              result.Set("manifest", base::Value());
            }
            std::move(done).Run(std::move(result));
          },
          profile, std::move(done)));
}

void HandleInstallability(const std::shared_ptr<ServiceCall>& call) {
  content::WebContents* page = LivePage(call);
  if (!page) {
    call->InvalidArguments("webApps.installability needs the page to check");
    return;
  }
  Installability(
      page, base::BindOnce(
                [](std::shared_ptr<ServiceCall> call, base::DictValue result) {
                  call->Resolve(ToJson(result));
                },
                call));
}

// Answers webApps.install with the app, or with why it did not install.
void ResolveInstall(const std::shared_ptr<ServiceCall>& call,
                    const webapps::AppId& app_id,
                    webapps::InstallResultCode code) {
  if (webapps::IsSuccess(code)) {
    if (std::optional<base::DictValue> app =
            InstalledAppInfo(call->profile(), app_id)) {
      call->Resolve(ToJson(*app));
      return;
    }
  }
  if (code == webapps::InstallResultCode::kUserInstallDeclined) {
    call->Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_CANCELLED,
                 "the install was not confirmed");
    return;
  }
  call->Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_INSTALLABLE,
               "the page could not be installed (code " +
                   base::NumberToString(static_cast<int>(code)) + ")");
}

// webApps.install {url} (web-apps.md 10.3): the URL loads in a hidden page
// of the profile, which gives the manifest and its id; Chromium then reads
// what to install in a hidden page of its own and installs it, so nothing
// depends on a page being on screen. It owns itself until it answers.
class UrlInstall : public content::WebContentsObserver {
 public:
  static void Start(std::shared_ptr<ServiceCall> call, GURL url) {
    (new UrlInstall(std::move(call), std::move(url)))->Load();
  }

  UrlInstall(const UrlInstall&) = delete;
  UrlInstall& operator=(const UrlInstall&) = delete;
  ~UrlInstall() override = default;

 private:
  UrlInstall(std::shared_ptr<ServiceCall> call, GURL url)
      : call_(std::move(call)), url_(std::move(url)) {}

  void Load() {
    hidden_ = content::WebContents::Create(
        content::WebContents::CreateParams(call_->profile()));
    Observe(hidden_.get());
    hidden_->GetController().LoadURLWithParams(
        content::NavigationController::LoadURLParams(url_));
  }

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (navigation->IsInPrimaryMainFrame() &&
        (!navigation->HasCommitted() || navigation->IsErrorPage())) {
      Fail(BDK_CODE_NOT_INSTALLABLE, url_.spec() + " could not be loaded");
    }
  }

  void DidStopLoading() override {
    if (asked_manifest_ || finished_) {
      return;
    }
    asked_manifest_ = true;
    webapps::InstallableManager::CreateForWebContents(hidden_.get());
    webapps::InstallableParams params;
    params.valid_primary_icon = true;
    params.installable_criteria =
        webapps::InstallableCriteria::kValidManifestWithIcons;
    params.check_eligibility = true;
    webapps::InstallableManager::FromWebContents(hidden_.get())
        ->GetData(params, base::BindOnce(&UrlInstall::OnManifest,
                                         weak_factory_.GetWeakPtr()));
  }

  void OnManifest(const webapps::InstallableData& data) {
    if (finished_) {
      return;
    }
    web_app::WebAppProvider* provider = ProviderOf(call_);
    if (!data.installable_check_passed || !data.errors.empty() ||
        data.manifest->start_url.is_empty() || !provider) {
      std::string why;
      for (webapps::InstallableStatusCode code : data.errors) {
        why += (why.empty() ? ": " : "; ") + webapps::GetErrorMessage(code);
      }
      Fail(BDK_CODE_NOT_INSTALLABLE, url_.spec() + " is not installable" + why);
      return;
    }
    provider->scheduler().FetchInstallInfoFromInstallUrl(
        webapps::ManifestId(data.manifest->id), url_,
        base::BindOnce(&UrlInstall::OnInstallInfo, weak_factory_.GetWeakPtr()));
  }

  void OnInstallInfo(std::unique_ptr<web_app::WebAppInstallInfo> info) {
    if (finished_) {
      return;
    }
    if (!info) {
      Fail(BDK_CODE_NOT_INSTALLABLE,
           "what " + url_.spec() + " installs could not be read");
      return;
    }
    if (call_->builtin().confirmed) {
      // The application asked its user already (web-apps.md 2).
      Install(std::move(info));
      return;
    }
    web_app::WebAppInstallInfo* raw = info.get();
    AskToInstall(call_->profile(), NamedPage(), *raw, "command",
                 base::BindOnce(&UrlInstall::OnAnswer,
                                weak_factory_.GetWeakPtr(), std::move(info)));
  }

  // The page the command named, if it is still open: the request's anchor.
  content::WebContents* NamedPage() const {
    const std::shared_ptr<bdkcore::Contents>& contents =
        call_->builtin().contents;
    if (!contents || contents->dead || contents->closed || !contents->data) {
      return nullptr;
    }
    return EngineContents::From(*contents)->web_contents();
  }

  void OnAnswer(std::unique_ptr<web_app::WebAppInstallInfo> info,
                bool installed) {
    if (finished_) {
      return;
    }
    if (!installed) {
      Fail(BDK_CODE_CANCELLED, "the install was not confirmed");
      return;
    }
    Install(std::move(info));
  }

  void Install(std::unique_ptr<web_app::WebAppInstallInfo> info) {
    web_app::WebAppProvider* provider = ProviderOf(call_);
    if (!provider || call_->cancelled()) {
      Fail(BDK_CODE_NOT_INSTALLABLE, "the profile has gone");
      return;
    }
    provider->scheduler().InstallFromInfoWithParams(
        std::move(info), /*overwrite_existing_manifest_fields=*/true,
        webapps::WebappInstallSource::MENU_BROWSER_TAB,
        base::BindOnce(&UrlInstall::OnInstalled, weak_factory_.GetWeakPtr()),
        web_app::WebAppInstallParams());
  }

  void OnInstalled(const webapps::AppId& app_id,
                   webapps::InstallResultCode code) {
    ResolveInstall(call_, app_id, code);
    Finish();
  }

  void Fail(const char* code, const std::string& message) {
    call_->Reject(BDK_DOMAIN_WEB_APPS, code, message);
    Finish();
  }

  // The hidden page may be in one of its own callbacks: it goes later.
  void Finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    Observe(nullptr);
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  std::shared_ptr<ServiceCall> call_;
  const GURL url_;
  std::unique_ptr<content::WebContents> hidden_;
  bool asked_manifest_ = false;
  bool finished_ = false;
  base::WeakPtrFactory<UrlInstall> weak_factory_{this};
};

void HandleInstall(const std::shared_ptr<ServiceCall>& call) {
  if (const std::optional<std::string> url = call->String("url")) {
    const GURL install_url(*url);
    if (!install_url.is_valid() || !install_url.SchemeIsHTTPOrHTTPS()) {
      call->InvalidArguments("webApps.install needs an http or https url");
      return;
    }
    if (!ProviderOf(call)) {
      call->Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_FOUND,
                   "this profile has no web apps");
      return;
    }
    UrlInstall::Start(call, install_url);
    return;
  }
  content::WebContents* page = LivePage(call);
  web_app::WebAppProvider* provider = ProviderOf(call);
  if (!page || !provider) {
    call->InvalidArguments("webApps.install needs a page, or a url");
    return;
  }
  const bool confirmed = call->builtin().confirmed;
  provider->scheduler().FetchManifestAndInstall(
      webapps::WebappInstallSource::MENU_BROWSER_TAB, page->GetWeakPtr(),
      base::BindOnce(
          [](bool confirmed, base::WeakPtr<web_app::WebAppScreenshotFetcher>,
             content::WebContents* initiator,
             std::unique_ptr<web_app::WebAppInstallInfo> info,
             web_app::WebAppInstallationAcceptanceCallback accept) {
            if (confirmed) {
              // The application asked its user already (web-apps.md 2).
              std::move(accept).Run(true, std::move(info), base::DoNothing());
              return;
            }
            web_app::WebAppInstallInfo* raw = info.get();
            AskToInstall(
                Profile::FromBrowserContext(initiator->GetBrowserContext()),
                initiator, *raw, "command",
                base::BindOnce(
                    [](std::unique_ptr<web_app::WebAppInstallInfo> info,
                       web_app::WebAppInstallationAcceptanceCallback accept,
                       bool installed) {
                      std::move(accept).Run(installed, std::move(info),
                                            base::DoNothing());
                    },
                    std::move(info), std::move(accept)));
          },
          confirmed),
      base::BindOnce(&ResolveInstall, call),
      web_app::FallbackBehavior::kCraftedManifestOnly);
}

void HandleUninstall(const std::shared_ptr<ServiceCall>& call) {
  const std::string id = call->StringOr("id", "");
  const bool clear_site_data = call->Bool("clearSiteData").value_or(false);
  WithRegistry(
      call,
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> call, std::string id,
             bool clear_site_data, web_app::WebAppProvider& provider) {
            std::optional<base::DictValue> app =
                InstalledAppInfo(call->profile(), id);
            if (!app) {
              call->Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_FOUND,
                           "no web app '" + id + "' in this profile");
              return;
            }
            const GURL start_url(*app->FindString("startUrl"));
            provider.scheduler().RemoveUserUninstallableManagements(
                id, webapps::WebappUninstallSource::kAppMenu,
                base::BindOnce(
                    [](std::shared_ptr<ServiceCall> call, GURL start_url,
                       bool clear_site_data,
                       webapps::UninstallResultCode code) {
                      if (!webapps::UninstallSucceeded(code)) {
                        call->Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_FOUND,
                                     "the app could not be uninstalled");
                        return;
                      }
                      if (clear_site_data) {
                        std::unique_ptr<content::BrowsingDataFilterBuilder>
                            filter = content::BrowsingDataFilterBuilder::Create(
                                content::BrowsingDataFilterBuilder::Mode::
                                    kDelete);
                        filter->AddOrigin(url::Origin::Create(start_url));
                        call->profile()
                            ->GetBrowsingDataRemover()
                            ->RemoveWithFilter(
                                base::Time(), base::Time::Max(),
                                content::BrowsingDataRemover::
                                        DATA_TYPE_DOM_STORAGE |
                                    content::BrowsingDataRemover::
                                        DATA_TYPE_COOKIES |
                                    content::BrowsingDataRemover::
                                        DATA_TYPE_CACHE,
                                content::BrowsingDataRemover::
                                    ORIGIN_TYPE_UNPROTECTED_WEB,
                                std::move(filter));
                      }
                      call->ResolveNull();
                    },
                    call, start_url, clear_site_data));
          },
          call, id, clear_site_data));
}

void HandleLaunch(const std::shared_ptr<ServiceCall>& call) {
  const std::string id = call->StringOr("id", "");
  const std::optional<std::string> shortcut = call->String("shortcutUrl");
  WithRegistry(
      call,
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> call, std::string id,
             std::optional<std::string> shortcut, web_app::WebAppProvider&) {
            std::optional<base::DictValue> app =
                InstalledAppInfo(call->profile(), id);
            if (!app) {
              call->Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_FOUND,
                           "no web app '" + id + "' in this profile");
              return;
            }
            GURL url(*app->FindString("startUrl"));
            if (shortcut) {
              bool listed = false;
              if (const base::ListValue* shortcuts =
                      app->FindList("shortcuts")) {
                for (const base::Value& s : *shortcuts) {
                  const std::string* u =
                      s.is_dict() ? s.GetDict().FindString("url") : nullptr;
                  listed = listed || (u && *u == *shortcut);
                }
              }
              if (!listed) {
                call->InvalidArguments("'" + *shortcut +
                                       "' is not one of the app's shortcuts");
                return;
              }
              url = GURL(*shortcut);
            }
            std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
            std::shared_ptr<bdkcore::Profile> profile =
                rt ? BdkProfileOf(*rt, call->profile()) : nullptr;
            EngineContents* page =
                EngineContents::LaunchWebApp(profile, id, url, std::move(*app));
            std::shared_ptr<bdkcore::Contents> core =
                page ? page->Core() : nullptr;
            if (!core) {
              call->Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_FOUND,
                           "the app's page could not be created");
              return;
            }
            call->Resolve(ToJson(base::DictValue().Set(
                "contents", base::NumberToString(core->handle))));
          },
          call, id, shortcut));
}

}  // namespace

void AddWebAppsHandlers(HandlerTable& table) {
  table["webApps.list"] = &HandleList;
  table["webApps.get"] = &HandleGet;
  table["webApps.installability"] = &HandleInstallability;
  table["webApps.install"] = &HandleInstall;
  table["webApps.uninstall"] = &HandleUninstall;
  table["webApps.launch"] = &HandleLaunch;
}

}  // namespace bdk
