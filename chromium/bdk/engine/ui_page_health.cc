// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_page_health.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/blocked_content/popup_blocker_tab_helper.h"
#include "components/blocked_content/url_list_manager.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "content/public/common/result_codes.h"
#include "ui/base/mojom/window_open_disposition.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {
namespace {

// At most this many popups get a button of their own in the default sheet.
constexpr size_t kPopupButtons = 3;

class PageHealth : public content::WebContentsUserData<PageHealth>,
                   public content::WebContentsObserver,
                   public blocked_content::UrlListManager::Observer {
 public:
  explicit PageHealth(content::WebContents* contents)
      : content::WebContentsUserData<PageHealth>(*contents),
        content::WebContentsObserver(contents) {
    Watch();
  }
  ~PageHealth() override {
    EndUnresponsive();
    EndNotice();
  }

  // The popup blocker of the page's tab, once the tab has one. It is a
  // feature of the tab, which goes before the page's WebContents does: the
  // observation ends when the tab is about to be deleted.
  void Watch() {
    if (popup_observation_.IsObserving()) {
      return;
    }
    tabs::TabInterface* tab =
        tabs::TabInterface::MaybeGetFromContents(web_contents());
    blocked_content::PopupBlockerTabHelper* helper =
        blocked_content::PopupBlockerTabHelper::FromWebContents(web_contents());
    if (!tab || !helper) {
      return;
    }
    popup_observation_.Observe(helper->manager());
    will_detach_ = tab->RegisterWillDetach(base::BindRepeating(
        [](base::WeakPtr<PageHealth> self, tabs::TabInterface*,
           tabs::TabInterface::DetachReason reason) {
          if (self && reason == tabs::TabInterface::DetachReason::kDelete) {
            self->StopWatching();
          }
        },
        weak_factory_.GetWeakPtr()));
  }

  void StopWatching() {
    popup_observation_.Reset();
    will_detach_ = {};
  }

  // ---- an unresponsive page ----

  void ShowUnresponsive(content::RenderWidgetHost* host,
                        base::RepeatingClosure restarter) {
    restarter_ = std::move(restarter);
    hung_process_ = host->GetProcess()->GetDeprecatedID();
    if (unresponsive_ || unresponsive_dismiss_) {
      return;  // one request per hang
    }
    EngineContents* page = EngineContents::FromWebContents(web_contents());
    if (!page) {
      return;
    }
    base::DictValue payload;
    payload.Set("kind", "unresponsive");
    payload.Set("origin", OriginOf(web_contents()));
    payload.Set("pageTitle", base::UTF16ToUTF8(web_contents()->GetTitle()));
    unresponsive_ =
        RaiseUiRequest(*page, BDK_SURFACE_PAGE_HEALTH, payload,
                       base::BindOnce(&PageHealth::OnUnresponsiveAnswer,
                                      weak_factory_.GetWeakPtr()));
  }

  void HideUnresponsive() {
    EndUnresponsive();
    restarter_.Reset();
  }

  bool IsShowingUnresponsive() const {
    return unresponsive_ || unresponsive_dismiss_;
  }

  // ---- blocked popups ----

  // blocked_content::UrlListManager::Observer:
  void BlockedUrlAdded(int32_t id, const GURL& url) override {
    blocked_.emplace_back(base::NumberToString(id), url.spec());
    // A new blocked popup withdraws the notice and raises one with the whole
    // list.
    EndNotice();
    EngineContents* page = EngineContents::FromWebContents(web_contents());
    if (!page) {
      return;
    }
    base::DictValue payload;
    payload.Set("kind", "blockedPopups");
    payload.Set("origin", OriginOf(web_contents()));
    base::ListValue popups;
    for (const auto& [popup_id, popup_url] : blocked_) {
      popups.Append(
          base::DictValue().Set("id", popup_id).Set("url", popup_url));
    }
    payload.Set("popups", std::move(popups));
    notice_ = RaiseUiRequest(*page, BDK_SURFACE_PAGE_HEALTH, payload,
                             base::BindOnce(&PageHealth::OnNoticeAnswer,
                                            weak_factory_.GetWeakPtr()));
  }

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    Watch();
    if (!handle->IsInPrimaryMainFrame() || !handle->HasCommitted() ||
        handle->IsSameDocument()) {
      return;
    }
    // Chromium forgets the popups a page blocked when it moves on.
    EndNotice();
    blocked_.clear();
  }

 private:
  friend class content::WebContentsUserData<PageHealth>;

  static std::string OriginOf(content::WebContents* contents) {
    const url::Origin origin =
        url::Origin::Create(contents->GetLastCommittedURL());
    return origin.opaque() ? std::string() : origin.Serialize();
  }

  void EndUnresponsive() {
    if (unresponsive_) {
      bdkcore::withdraw_ui_request(*unresponsive_);
      unresponsive_.reset();
    }
    if (unresponsive_dismiss_) {
      std::move(unresponsive_dismiss_).Run();
    }
  }

  void EndNotice() {
    if (notice_) {
      bdkcore::withdraw_ui_request(*notice_);
      notice_.reset();
    }
    if (notice_dismiss_) {
      std::move(notice_dismiss_).Run();
    }
  }

  void OnUnresponsiveAnswer(const UiResult& result) {
    if (result.outcome == bdkcore::UiOutcome::kWithdrawn) {
      return;  // the renderer answered, or the page went
    }
    unresponsive_.reset();
    switch (result.outcome) {
      case bdkcore::UiOutcome::kCompleted: {
        const std::string* action = result.args.FindString("action");
        Act(result.completion == "accept" && action && *action == "terminate");
        return;
      }
      case bdkcore::UiOutcome::kDefaulted:
        ShowUnresponsiveSheet();
        return;
      default:
        // Rejected, dropped or withdrawn: a request never ends a page unless
        // someone chose to.
        Act(/*terminate=*/false);
        return;
    }
  }

  void ShowUnresponsiveSheet() {
    EngineContents* page = EngineContents::FromWebContents(web_contents());
    if (!page) {
      return;
    }
    const std::string title = base::UTF16ToUTF8(web_contents()->GetTitle());
    Alert alert;
    alert.title = "Page Unresponsive";
    alert.message = (title.empty() ? std::string("This page") : title) +
                    " is not responding. You can wait for it to become "
                    "responsive or exit the page.";
    alert.buttons.push_back({"terminate", "Exit page"});
    alert.buttons.push_back({"wait", "Wait"});
    unresponsive_dismiss_ = UiPresenters::Get().ShowAlert(
        "page_health", &page->host_view(), std::move(alert),
        base::BindOnce(
            [](base::WeakPtr<PageHealth> self, int index) {
              if (self) {
                self->unresponsive_dismiss_.Reset();
                self->Act(/*terminate=*/index == 0);
              }
            },
            weak_factory_.GetWeakPtr()));
  }

  void Act(bool terminate) {
    if (terminate) {
      if (content::RenderProcessHost* process =
              content::RenderProcessHost::FromID(hung_process_)) {
        process->Shutdown(content::RESULT_CODE_HUNG);
      }
      return;
    }
    if (restarter_) {
      restarter_.Run();
    }
  }

  void OnNoticeAnswer(const UiResult& result) {
    if (result.outcome == bdkcore::UiOutcome::kWithdrawn) {
      return;  // replaced by a notice with a longer list, or the page moved on
    }
    notice_.reset();
    if (result.outcome == bdkcore::UiOutcome::kDefaulted) {
      ShowNoticeSheet();
      return;
    }
    if (result.outcome != bdkcore::UiOutcome::kCompleted ||
        result.completion != "grant") {
      blocked_.clear();  // dismissed: the next blocked popup starts a new list
      return;
    }
    if (result.args.FindBool("always").value_or(false)) {
      AllowAlways();
    } else if (const std::string* id = result.args.FindString("popup")) {
      Open(*id);
    }
    blocked_.clear();
  }

  void ShowNoticeSheet() {
    EngineContents* page = EngineContents::FromWebContents(web_contents());
    if (!page || blocked_.empty()) {
      return;
    }
    const std::string host(web_contents()->GetLastCommittedURL().host());
    Alert alert;
    alert.title = "Pop-ups blocked";
    alert.message = "This site tried to open:";
    std::vector<std::string> ids;
    for (const auto& [id, url] : blocked_) {
      alert.message += "\n" + url;
      if (ids.size() < kPopupButtons) {
        ids.push_back(id);
        alert.buttons.push_back(
            {"open:" + id, "Open " + std::string(GURL(url).host())});
      }
    }
    alert.buttons.push_back({"always", "Always allow pop-ups from " + host});
    alert.buttons.push_back({"done", "Done"});
    notice_dismiss_ = UiPresenters::Get().ShowAlert(
        "page_health", &page->host_view(), std::move(alert),
        base::BindOnce(
            [](base::WeakPtr<PageHealth> self, std::vector<std::string> ids,
               int index) {
              if (!self) {
                return;
              }
              self->notice_dismiss_.Reset();
              if (index >= 0 && static_cast<size_t>(index) < ids.size()) {
                self->Open(ids[static_cast<size_t>(index)]);
              } else if (index >= 0 &&
                         static_cast<size_t>(index) == ids.size()) {
                self->AllowAlways();
              }
              self->blocked_.clear();
            },
            weak_factory_.GetWeakPtr(), std::move(ids)));
  }

  void Open(const std::string& id) {
    int popup_id = 0;
    blocked_content::PopupBlockerTabHelper* helper =
        blocked_content::PopupBlockerTabHelper::FromWebContents(web_contents());
    if (!helper || !base::StringToInt(id, &popup_id)) {
      return;
    }
    // As the renderer asked for it; the new page then raises new_windows.
    helper->ShowBlockedPopup(popup_id,
                             ui::mojom::WindowOpenDisposition::kCurrentTab);
  }

  void AllowAlways() {
    Profile* profile =
        Profile::FromBrowserContext(web_contents()->GetBrowserContext());
    HostContentSettingsMap* settings =
        profile ? HostContentSettingsMapFactory::GetForProfile(profile)
                : nullptr;
    if (!settings) {
      return;
    }
    settings->SetContentSettingDefaultScope(
        web_contents()->GetLastCommittedURL(), GURL(),
        ContentSettingsType::POPUPS, CONTENT_SETTING_ALLOW);
  }

  std::shared_ptr<bdkcore::Request> unresponsive_;
  Dismisser unresponsive_dismiss_;
  base::RepeatingClosure restarter_;
  int hung_process_ = 0;

  // The popups blocked since the last notice was answered: id and URL.
  std::vector<std::pair<std::string, std::string>> blocked_;
  std::shared_ptr<bdkcore::Request> notice_;
  Dismisser notice_dismiss_;
  base::ScopedObservation<blocked_content::UrlListManager,
                          blocked_content::UrlListManager::Observer>
      popup_observation_{this};
  base::CallbackListSubscription will_detach_;

  base::WeakPtrFactory<PageHealth> weak_factory_{this};
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(PageHealth);

PageHealth& For(content::WebContents* contents) {
  PageHealth::CreateForWebContents(contents);
  return *PageHealth::FromWebContents(contents);
}

}  // namespace

void ShowUnresponsivePage(content::WebContents* contents,
                          content::RenderWidgetHost* render_widget_host,
                          base::RepeatingClosure hang_monitor_restarter) {
  For(contents).ShowUnresponsive(render_widget_host,
                                 std::move(hang_monitor_restarter));
}

void HideUnresponsivePage(content::WebContents* contents) {
  if (PageHealth* health = PageHealth::FromWebContents(contents)) {
    health->HideUnresponsive();
  }
}

bool IsShowingUnresponsivePage(content::WebContents* contents) {
  PageHealth* health = PageHealth::FromWebContents(contents);
  return health && health->IsShowingUnresponsive();
}

void InstallPageHealth(EngineContents&, content::WebContents* contents) {
  For(contents).Watch();
}

void UninstallPageHealth(content::WebContents* contents) {
  contents->RemoveUserData(PageHealth::UserDataKey());
}

}  // namespace bdk
