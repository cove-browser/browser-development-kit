// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/password_prompts.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/services/native_presenter.h"
#include "bdk/engine/services/ui_requests.h"
#include "chrome/browser/ui/passwords/manage_passwords_ui_controller.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/common/password_manager_ui.h"
#include "url/gurl.h"

namespace bdk {

namespace {

using password_manager::ui::State;

ManagePasswordsUIController* ControllerOf(content::WebContents* contents) {
  return contents ? ManagePasswordsUIController::FromWebContents(contents)
                  : nullptr;
}

bool WaitsForAnswer(State state, bool is_update) {
  return state == (is_update ? State::PENDING_PASSWORD_UPDATE_STATE
                             : State::PENDING_PASSWORD_STATE);
}

}  // namespace

BdkTabDialogs::BdkTabDialogs(tabs::TabInterface& tab,
                             content::WebContents* contents)
    : TabDialogsViews(tab, contents), contents_(contents->GetWeakPtr()) {}

BdkTabDialogs::~BdkTabDialogs() {
  if (request_) {
    bdkcore::withdraw_ui_request(*request_);
  }
}

void BdkTabDialogs::ShowManagePasswordsBubble(bool user_action) {
  content::WebContents* contents = contents_.get();
  ManagePasswordsUIController* controller = ControllerOf(contents);
  EngineContents* page = FindEngineContents(contents);
  if (!controller || !page) {
    return;
  }
  // Only the two states that ask a question are a request. The management
  // list, the confirmation and the sign-in states have no counterpart in the
  // surface.
  const State state = controller->GetState();
  const bool is_update = state == State::PENDING_PASSWORD_UPDATE_STATE;
  if (!is_update && state != State::PENDING_PASSWORD_STATE) {
    return;
  }
  if (answered_ || (request_ && !request_->finished)) {
    return;
  }
  const password_manager::PasswordForm& pending =
      controller->GetPendingPassword();
  // The password is not in the request: a presenter never sees it.
  base::DictValue payload;
  payload.Set("origin", controller->GetOrigin().GetURL().spec());
  payload.Set("username", base::UTF16ToUTF8(pending.username_value));
  payload.Set("isUpdate", is_update);
  controller->OnBubbleShown();
  base::WeakPtr<BdkTabDialogs> self = weak_factory_.GetWeakPtr();
  request_ = RaiseUiRequest(*page, BDK_SURFACE_PASSWORDS, payload,
                            [self, is_update](bdkcore::UiOutcome outcome,
                                              const std::string& completion,
                                              const std::string& arguments) {
                              if (self) {
                                self->Finished(is_update, outcome, completion,
                                               arguments);
                              }
                            });
  if (!request_) {
    controller->OnNoInteraction();
    controller->OnBubbleHidden();
  }
}

void BdkTabDialogs::HideManagePasswordsBubble() {
  // Chromium closes its bubble when the page moves on; the request goes with
  // it. The password manager was told already, so nothing is applied.
  if (request_) {
    bdkcore::withdraw_ui_request(*request_);
    request_.reset();
  }
}

void BdkTabDialogs::Finished(bool is_update,
                             bdkcore::UiOutcome outcome,
                             const std::string& completion,
                             const std::string& arguments) {
  request_.reset();
  switch (outcome) {
    case bdkcore::UiOutcome::kCompleted:
      if (completion == "grant") {
        Apply(is_update, Choice::kSave);
      } else if (ArgumentIsTrue(arguments, "never")) {
        Apply(is_update, Choice::kNever);
      } else {
        Apply(is_update, Choice::kNotNow);
      }
      return;
    case bdkcore::UiOutcome::kDefaulted: {
      content::WebContents* contents = contents_.get();
      ManagePasswordsUIController* controller = ControllerOf(contents);
      EngineContents* page = FindEngineContents(contents);
      if (!controller || !page) {
        return;
      }
      const std::string host = controller->GetOrigin().host();
      const std::string username =
          base::UTF16ToUTF8(controller->GetPendingPassword().username_value);
      Alert alert;
      alert.title = is_update ? "Update password?" : "Save password?";
      alert.message = username + " on " + host;
      alert.buttons.push_back({"save", is_update ? "Update" : "Save"});
      alert.buttons.push_back({"notNow", "Not now"});
      if (!is_update) {
        alert.buttons.push_back({"never", "Never for this site"});
      }
      PresentAlert(
          "passwords", std::move(alert), WindowOfPage(*page),
          base::BindOnce(
              [](base::WeakPtr<BdkTabDialogs> self, bool is_update, int index) {
                if (!self) {
                  return;
                }
                self->Apply(is_update, index == 0   ? Choice::kSave
                                       : index == 1 ? Choice::kNotNow
                                       : index == 2 ? Choice::kNever
                                                    : Choice::kNoInteraction);
              },
              weak_factory_.GetWeakPtr(), is_update));
      return;
    }
    case bdkcore::UiOutcome::kWithdrawn:
      return;  // Chromium moved on
    default:   // rejected, dropped, or the surface is disabled
      Apply(is_update, Choice::kNoInteraction);
      return;
  }
}

void BdkTabDialogs::Apply(bool is_update, Choice choice) {
  ManagePasswordsUIController* controller = ControllerOf(contents_.get());
  if (!controller || !WaitsForAnswer(controller->GetState(), is_update)) {
    return;
  }
  switch (choice) {
    case Choice::kSave: {
      const password_manager::PasswordForm& pending =
          controller->GetPendingPassword();
      controller->SavePassword(pending.username_value, pending.password_value);
      break;
    }
    case Choice::kNotNow:
      if (is_update) {
        controller->OnNopeUpdateClicked();
      } else {
        controller->OnNotNowClicked();
      }
      break;
    case Choice::kNever:
      controller->NeverSavePassword();
      break;
    case Choice::kNoInteraction:
      controller->OnNoInteraction();
      break;
  }
  answered_ = true;
  controller->OnBubbleHidden();
}

void SchedulePasswordPromptCheck(content::WebContents* contents) {
  if (auto* dialogs =
          static_cast<BdkTabDialogs*>(TabDialogs::FromWebContents(contents))) {
    dialogs->ResetAnswered();
  }
  for (int milliseconds : {300, 1000, 2500}) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<content::WebContents> weak) {
              content::WebContents* contents = weak.get();
              if (!contents) {
                return;
              }
              auto* dialogs = static_cast<BdkTabDialogs*>(
                  TabDialogs::FromWebContents(contents));
              ManagePasswordsUIController* controller = ControllerOf(contents);
              if (dialogs && controller) {
                dialogs->ShowManagePasswordsBubble(false);
              }
            },
            contents->GetWeakPtr()),
        base::Milliseconds(milliseconds));
  }
}

}  // namespace bdk
