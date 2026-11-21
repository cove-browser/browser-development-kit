// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_permissions.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/permissions/permission_prompt.h"
#include "components/permissions/permission_request.h"
#include "components/permissions/permission_request_manager.h"
#include "components/permissions/permission_uma_constants.h"
#include "components/permissions/permission_util.h"
#include "components/permissions/request_type.h"
#include "content/public/browser/web_contents.h"
#include "url/origin.h"

namespace bdk {

namespace {

using permissions::RequestType;

// The kind of a request, as ui-requests.md names them.
std::string KindOf(RequestType type) {
  switch (type) {
    case RequestType::kCameraStream:
      return "camera";
    case RequestType::kCameraPanTiltZoom:
      return "cameraPanTiltZoom";
    case RequestType::kMicStream:
      return "microphone";
    case RequestType::kGeolocation:
      return "geolocation";
    case RequestType::kNotifications:
      return "notifications";
    case RequestType::kClipboard:
      return "clipboardRead";
    case RequestType::kMidiSysex:
      return "midi";
    case RequestType::kStorageAccess:
    case RequestType::kTopLevelStorageAccess:
      return "storageAccess";
    case RequestType::kLocalFonts:
      return "localFonts";
    case RequestType::kWindowManagement:
      return "windowManagement";
    case RequestType::kIdleDetection:
      return "idleDetection";
    case RequestType::kMultipleDownloads:
      return "multipleDownloads";
    case RequestType::kFileSystemAccess:
      return "fileSystemAccess";
    case RequestType::kPointerLock:
      return "pointerLock";
    case RequestType::kKeyboardLock:
      return "keyboardLock";
    case RequestType::kSensors:
      return "sensors";
    case RequestType::kRegisterProtocolHandler:
      return "protocolHandlers";
    default:
      return "other";
  }
}

std::string OriginText(const GURL& url) {
  return url::Origin::Create(url).Serialize();
}

// One prompt of the request manager, shown as a UI request. It lives as long
// as the manager keeps its view: the manager deletes it when the requests are
// decided, the page navigates or closes, or it recreates the view; deleting an
// undecided one withdraws the request.
class BdkPermissionPrompt : public permissions::PermissionPrompt {
 public:
  BdkPermissionPrompt(base::WeakPtr<EngineContents> page, Delegate* delegate)
      : page_(std::move(page)), delegate_(delegate->GetWeakPtr()) {
    if (!page_) {
      return;
    }
    const auto& requests = delegate->Requests();
    base::ListValue kinds;
    std::string message;
    bool one_time = !requests.empty();
    for (const auto& request : requests) {
      const std::string kind = KindOf(request->request_type());
      asked_.insert(kind);
      kinds.Append(kind);
      const std::string fragment =
          base::UTF16ToUTF8(request->GetMessageTextFragment());
      message += (message.empty() ? "" : " and ") + fragment;
      one_time =
          one_time && permissions::PermissionUtil::DoesSupportTemporaryGrants(
                          request->GetContentSettingsType());
    }
    const std::string origin = OriginText(delegate->GetRequestingOrigin());
    payload_.Set("origin", origin);
    const std::string embedding = OriginText(delegate->GetEmbeddingOrigin());
    if (embedding != origin) {
      payload_.Set("embeddingOrigin", embedding);
    }
    payload_.Set("kinds", std::move(kinds));
    // The wording the default presenter shows; a custom presenter is given the
    // same.
    payload_.Set("text", base::DictValue()
                             .Set("title", origin + " wants to")
                             .Set("message", message));
    payload_.Set("offersOneTime", one_time);
    if (asked_.contains("notifications") &&
        !SurfaceAvailable(page_->Core(), BDK_SURFACE_NOTIFICATIONS)) {
      // Notifications are disabled: the API reports permission denied, with
      // nobody asked.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&BdkPermissionPrompt::Decide,
                         weak_factory_.GetWeakPtr(), PermissionChoice::kBlock));
      return;
    }
    request_ = RaiseUiRequest(page_->Core(), BDK_SURFACE_PERMISSIONS, payload_,
                              base::BindOnce(&BdkPermissionPrompt::OnEnded,
                                             weak_factory_.GetWeakPtr()),
                              &page_->host_view());
  }

  ~BdkPermissionPrompt() override {
    // The manager dropped the prompt: the question is gone.
    if (request_) {
      bdkcore::withdraw_ui_request(*request_);
    }
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
  }

  // permissions::PermissionPrompt:
  bool UpdateAnchor() override { return true; }
  TabSwitchingBehavior GetTabSwitchingBehavior() override {
    return TabSwitchingBehavior::kKeepPromptAlive;
  }
  permissions::PermissionPromptDisposition GetPromptDisposition()
      const override {
    return permissions::PermissionPromptDisposition::NOT_APPLICABLE;
  }
  bool IsAskPrompt() const override { return false; }
  std::optional<gfx::Rect> GetViewBoundsInScreen() const override {
    return std::nullopt;
  }
  std::vector<permissions::ElementAnchoredBubbleVariant> GetPromptVariants()
      const override {
    return {};
  }
  std::optional<permissions::feature_params::PermissionElementPromptPosition>
  GetPromptPosition() const override {
    return std::nullopt;
  }

 private:
  permissions::PermissionPrompt::Delegate* Manager() { return delegate_.get(); }

  // The options the manager wants with a decision.
  ::PromptOptions Options() {
    if (!delegate_->Requests().empty() &&
        delegate_->Requests()[0]->GetContentSettingsType() ==
            ContentSettingsType::GEOLOCATION_WITH_OPTIONS) {
      return ::GeolocationPromptOptions();
    }
    return std::monostate();
  }

  void Decide(PermissionChoice choice) {
    // The delegate is gone when the manager moved on; a decision then
    // belongs to nobody.
    if (!Manager() || decided_) {
      return;
    }
    decided_ = true;
    request_.reset();  // ended; nothing to withdraw
    switch (choice) {
      case PermissionChoice::kAllow:
        Manager()->Accept(Options());
        break;
      case PermissionChoice::kAllowThisVisit:
        Manager()->AcceptThisTime(Options());
        break;
      case PermissionChoice::kBlock:
        Manager()->Deny(Options());
        break;
      case PermissionChoice::kDismiss:
        Manager()->Dismiss(Options());
        break;
    }
  }

  // A decision that persists nothing and does not count against the site.
  void Ignore() {
    if (!Manager() || decided_) {
      return;
    }
    decided_ = true;
    request_.reset();
    Manager()->Ignore(Options());
  }

  void OnEnded(const UiResult& result) {
    if (!Manager()) {
      return;
    }
    switch (result.outcome) {
      case bdkcore::UiOutcome::kCompleted:
        if (result.completion == "grant") {
          Grant(result);
        } else if (result.args.FindBool("dismissed").value_or(false)) {
          Decide(PermissionChoice::kDismiss);
        } else if (result.args.FindBool("persist").value_or(true)) {
          Decide(PermissionChoice::kBlock);
        } else {
          Ignore();
        }
        return;
      case bdkcore::UiOutcome::kDefaulted:
        dismiss_ = UiPresenters::Get().ShowPermission(
            &page_->host_view(), payload_,
            base::BindOnce(&BdkPermissionPrompt::Decide,
                           weak_factory_.GetWeakPtr()));
        return;
      case bdkcore::UiOutcome::kDisabled:
        // The surface is disabled: the web API sees a denial, never a grant.
        Decide(PermissionChoice::kBlock);
        return;
      case bdkcore::UiOutcome::kWithdrawn:
        return;
      default:
        // Rejected, or dropped by the application: no.
        Ignore();
        return;
    }
  }

  void Grant(const UiResult& result) {
    // RaiseUiRequest already turned a grant without the operating system's
    // gesture into a rejection, so this one carried a recent mouse-up or
    // key-down in a window of this application.
    // The manager decides a prompt's requests together, so a grant of fewer
    // kinds than were asked cannot be honoured: it grants nothing.
    std::set<std::string> granted;
    if (const base::ListValue* list = result.args.FindList("kinds")) {
      for (const base::Value& kind : *list) {
        if (kind.is_string()) {
          granted.insert(kind.GetString());
        }
      }
    } else {
      granted = asked_;
    }
    if (granted != asked_) {
      Decide(PermissionChoice::kDismiss);
      return;
    }
    const std::string* duration = result.args.FindString("duration");
    Decide(duration && *duration == "thisVisit"
               ? PermissionChoice::kAllowThisVisit
               : PermissionChoice::kAllow);
  }

  base::WeakPtr<EngineContents> page_;
  base::WeakPtr<Delegate> delegate_;
  base::DictValue payload_;
  std::set<std::string> asked_;
  std::shared_ptr<bdkcore::Request> request_;
  Dismisser dismiss_;
  bool decided_ = false;
  base::WeakPtrFactory<BdkPermissionPrompt> weak_factory_{this};
};

std::unique_ptr<permissions::PermissionPrompt> CreatePrompt(
    base::WeakPtr<EngineContents> page,
    content::WebContents* contents,
    permissions::PermissionPrompt::Delegate* delegate) {
  return std::make_unique<BdkPermissionPrompt>(std::move(page), delegate);
}

}  // namespace

void InstallPermissionPrompts(EngineContents& page,
                              content::WebContents* contents) {
  if (auto* manager =
          permissions::PermissionRequestManager::FromWebContents(contents)) {
    manager->set_view_factory_for_testing(
        base::BindRepeating(&CreatePrompt, page.GetWeakPtr()));
  }
}

}  // namespace bdk
