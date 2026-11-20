// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_notifications.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/notifications/notification_display_service_factory.h"
#include "chrome/browser/notifications/notification_display_service_impl.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/notifications/notification_operation.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/message_center/public/cpp/notification.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {

namespace {

bool IsShown(NotificationHandler::Type type) {
  return type == NotificationHandler::Type::WEB_NON_PERSISTENT ||
         type == NotificationHandler::Type::WEB_PERSISTENT ||
         type == NotificationHandler::Type::EXTENSION;
}

// The page a notification is raised for: a page of the profile that is at the
// notification's origin, else any page of the profile. A notification has no
// page of its own (it may outlive the one that made it).
EngineContents* PageFor(Profile* profile, const GURL& origin) {
  EngineContents* fallback = nullptr;
  for (EngineContents* page : EngineContents::All()) {
    std::shared_ptr<bdkcore::Contents> core = page->Core();
    if (!core || !core->profile || !core->profile->data ||
        ProfileOf(*core)->GetOriginalProfile() !=
            profile->GetOriginalProfile()) {
      continue;
    }
    if (!fallback) {
      fallback = page;
    }
    if (url::Origin::Create(page->web_contents()->GetLastCommittedURL()) ==
        url::Origin::Create(origin)) {
      return page;
    }
  }
  return fallback;
}

class BdkNotificationService : public NotificationDisplayServiceImpl {
 public:
  explicit BdkNotificationService(Profile* profile)
      : NotificationDisplayServiceImpl(profile), profile_(profile) {}

  ~BdkNotificationService() override {
    for (auto& [id, shown] : shown_) {
      Remove(shown);
    }
  }

  // NotificationDisplayServiceImpl:
  void Display(
      NotificationHandler::Type type,
      const message_center::Notification& notification,
      std::unique_ptr<NotificationCommon::Metadata> metadata) override {
    if (!IsShown(type)) {
      return;
    }
    const std::string id = notification.id();
    Close(type, id);  // a notification with the same id is replaced
    EngineContents* page = PageFor(profile_, notification.origin_url());
    if (!page) {
      // No page of the profile is open to show it in.
      Process(NotificationOperation::kClose, type, notification.origin_url(),
              id, std::nullopt, /*by_user=*/false);
      return;
    }
    base::DictValue details;
    details.Set("title", base::UTF16ToUTF8(notification.title()));
    details.Set("body", base::UTF16ToUTF8(notification.message()));
    details.Set("origin",
                url::Origin::Create(notification.origin_url()).Serialize());
    details.Set("tag", id);
    base::ListValue actions;
    Shown shown;
    for (const message_center::ButtonInfo& button : notification.buttons()) {
      actions.Append(base::UTF16ToUTF8(button.title));
      shown.actions.push_back(base::UTF16ToUTF8(button.title));
    }
    details.Set("actions", std::move(actions));
    details.Set("requireInteraction", notification.never_timeout());
    base::DictValue payload;
    payload.Set("notification", std::move(details));

    shown.type = type;
    shown.origin = notification.origin_url();
    shown.page = page->GetWeakPtr();
    base::DictValue copy = payload.Clone();
    shown.request = RaiseUiRequest(
        page->Core(), BDK_SURFACE_NOTIFICATIONS, payload,
        base::BindOnce(&BdkNotificationService::OnEnded,
                       weak_factory_.GetWeakPtr(), id, std::move(copy)),
        &page->host_view());
    shown_[id] = std::move(shown);
    // The page's onshow.
    if (NotificationHandler* handler = GetNotificationHandler(type)) {
      handler->OnShow(profile_, id);
    }
  }

  void Close(NotificationHandler::Type type, const std::string& id) override {
    auto it = shown_.find(id);
    if (it == shown_.end()) {
      return;
    }
    Shown shown = std::move(it->second);
    shown_.erase(it);
    Remove(shown);
  }

  void GetDisplayed(DisplayedNotificationsCallback callback) override {
    std::set<std::string> ids;
    for (const auto& [id, shown] : shown_) {
      ids.insert(id);
    }
    std::move(callback).Run(std::move(ids), /*supports_synchronization=*/true);
  }

  void GetDisplayedForOrigin(const GURL& origin,
                             DisplayedNotificationsCallback callback) override {
    std::set<std::string> ids;
    for (const auto& [id, shown] : shown_) {
      if (url::Origin::Create(shown.origin) == url::Origin::Create(origin)) {
        ids.insert(id);
      }
    }
    std::move(callback).Run(std::move(ids), /*supports_synchronization=*/true);
  }

 private:
  struct Shown {
    NotificationHandler::Type type = NotificationHandler::Type::WEB_PERSISTENT;
    GURL origin;
    std::vector<std::string> actions;
    base::WeakPtr<EngineContents> page;
    std::shared_ptr<bdkcore::Request> request;
    Dismisser dismiss;
  };

  // Takes down what is shown of a notification that is closing.
  static void Remove(Shown& shown) {
    if (shown.request) {
      bdkcore::withdraw_ui_request(*shown.request);
    }
    if (shown.dismiss) {
      std::move(shown.dismiss).Run();
    }
  }

  void Process(NotificationOperation operation,
               NotificationHandler::Type type,
               const GURL& origin,
               const std::string& id,
               std::optional<int> action_index,
               bool by_user) {
    ProcessNotificationOperation(operation, type, origin, id, action_index,
                                 std::nullopt, by_user, std::nullopt,
                                 base::DoNothing());
  }

  // The application or the default presenter reports what happened to it.
  void OnEvent(const std::string& id,
               NotificationEvent event,
               const std::optional<std::string>& action) {
    auto it = shown_.find(id);
    if (it == shown_.end()) {
      return;
    }
    Shown shown = std::move(it->second);
    shown_.erase(it);
    shown.request.reset();  // ended
    if (shown.dismiss) {
      std::move(shown.dismiss).Run();
    }
    if (event == NotificationEvent::kClicked) {
      std::optional<int> index;
      if (action) {
        for (size_t i = 0; i < shown.actions.size(); ++i) {
          if (shown.actions[i] == *action) {
            index = static_cast<int>(i);
          }
        }
      }
      Process(NotificationOperation::kClick, shown.type, shown.origin, id,
              index, /*by_user=*/true);
      return;
    }
    Process(NotificationOperation::kClose, shown.type, shown.origin, id,
            std::nullopt, event == NotificationEvent::kClosedByUser);
  }

  void OnEnded(const std::string& id,
               base::DictValue payload,
               const UiResult& result) {
    auto it = shown_.find(id);
    if (it == shown_.end()) {
      return;
    }
    switch (result.outcome) {
      case bdkcore::UiOutcome::kCompleted: {
        if (result.args.FindString("event") &&
            *result.args.FindString("event") == "clicked") {
          const std::string* action = result.args.FindString("action");
          OnEvent(id, NotificationEvent::kClicked,
                  action ? std::optional<std::string>(*action) : std::nullopt);
        } else {
          OnEvent(id,
                  result.args.FindBool("byUser").value_or(true)
                      ? NotificationEvent::kClosedByUser
                      : NotificationEvent::kClosedByTimeout,
                  std::nullopt);
        }
        return;
      }
      case bdkcore::UiOutcome::kDefaulted: {
        EngineContents* page = it->second.page.get();
        Dismisser dismiss = UiPresenters::Get().ShowNotification(
            page ? &page->host_view() : nullptr, payload,
            base::BindRepeating(&BdkNotificationService::OnEvent,
                                weak_factory_.GetWeakPtr(), id));
        // The presenter may have reported an event already.
        it = shown_.find(id);
        if (it != shown_.end()) {
          it->second.dismiss = std::move(dismiss);
        } else if (dismiss) {
          std::move(dismiss).Run();
        }
        return;
      }
      case bdkcore::UiOutcome::kWithdrawn:
        return;  // Close() or the page took it away
      default:
        // Rejected, dropped or disabled: not shown, so closed, not by the
        // person.
        OnEvent(id, NotificationEvent::kClosedByTimeout, std::nullopt);
        return;
    }
  }

  raw_ptr<Profile> profile_;
  std::map<std::string, Shown> shown_;
  base::WeakPtrFactory<BdkNotificationService> weak_factory_{this};
};

std::unique_ptr<KeyedService> Build(content::BrowserContext* context) {
  return std::make_unique<BdkNotificationService>(
      Profile::FromBrowserContext(context));
}

}  // namespace

void InstallNotificationDisplay(Profile* profile) {
  NotificationDisplayServiceFactory::GetInstance()->SetTestingFactory(
      profile, base::BindRepeating(&Build));
}

}  // namespace bdk
