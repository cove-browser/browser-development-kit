// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/password_prompts.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/ui/passwords/credential_manager_dialog_controller.h"
#include "chrome/browser/ui/passwords/embedder_credential_manager_prompts.h"
#include "chrome/browser/ui/passwords/manage_passwords_ui_controller.h"
#include "chrome/browser/ui/passwords/password_dialog_prompts.h"
#include "chrome/grit/generated_resources.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/common/credential_manager_types.h"
#include "components/password_manager/core/common/password_manager_ui.h"
#include "components/strings/grit/components_strings.h"
#include "components/url_formatter/elide_url.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/strings/grit/ui_strings.h"
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
  if (dismiss_) {
    std::move(dismiss_).Run();
  }
}

void BdkTabDialogs::ShowManagePasswordsBubble(bool user_action) {
  content::WebContents* contents = contents_.get();
  ManagePasswordsUIController* controller = ControllerOf(contents);
  EngineContents* page = EngineContents::FromWebContents(contents);
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
  if (answered_ || (request_ && !request_->finished) || dismiss_) {
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
                            base::BindOnce(
                                [](base::WeakPtr<BdkTabDialogs> self,
                                   bool is_update, const UiResult& result) {
                                  if (self) {
                                    self->Finished(is_update, result);
                                  }
                                },
                                self, is_update));
  if (!request_) {
    controller->OnNoInteraction();
    controller->OnBubbleHidden();
  }
}

void BdkTabDialogs::OnPageNavigated() {
  // A new page may ask a new question.
  answered_ = false;
  // The password manager may already hold one, about the credentials the
  // person typed into the page that was left: it keeps that "save?" state
  // across the navigation (its fallback) but only ever shows an icon for it,
  // which a BDK window has none of. Ask it now, once the password manager has
  // handled the navigation itself.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&BdkTabDialogs::ShowManagePasswordsBubble,
                                weak_factory_.GetWeakPtr(), false));
}

void BdkTabDialogs::HideManagePasswordsBubble() {
  // Chromium closes its bubble when the page moves on; the request goes with
  // it. The password manager was told already, so nothing is applied.
  if (request_) {
    bdkcore::withdraw_ui_request(*request_);
    request_.reset();
  }
  if (dismiss_) {
    std::move(dismiss_).Run();
  }
}

void BdkTabDialogs::Finished(bool is_update, const UiResult& result) {
  request_.reset();
  switch (result.outcome) {
    case bdkcore::UiOutcome::kCompleted:
      if (result.completion == "grant") {
        Apply(is_update, Choice::kSave);
      } else if (result.args.FindBool("never").value_or(false)) {
        Apply(is_update, Choice::kNever);
      } else {
        Apply(is_update, Choice::kNotNow);
      }
      return;
    case bdkcore::UiOutcome::kDefaulted: {
      content::WebContents* contents = contents_.get();
      ManagePasswordsUIController* controller = ControllerOf(contents);
      EngineContents* page = EngineContents::FromWebContents(contents);
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
      dismiss_ = UiPresenters::Get().ShowAlert(
          "passwords", &page->host_view(), std::move(alert),
          base::BindOnce(
              [](base::WeakPtr<BdkTabDialogs> self, bool is_update, int index) {
                if (!self) {
                  return;
                }
                self->dismiss_.Reset();
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

void OnPageNavigatedForPasswords(content::WebContents* contents) {
  if (auto* dialogs =
          static_cast<BdkTabDialogs*>(TabDialogs::FromWebContents(contents))) {
    dialogs->OnPageNavigated();
  }
}

namespace {

HostView* ViewOfPage(content::WebContents* contents) {
  EngineContents* page = EngineContents::FromWebContents(contents);
  return page ? &page->host_view() : nullptr;
}

// The account chooser: one button per saved account of the site (the first
// eight; a sheet holds buttons, not a list), then Cancel.
class AccountChooserSheet : public AccountChooserPrompt {
 public:
  AccountChooserSheet(CredentialManagerDialogController* controller,
                      content::WebContents* contents)
      : controller_(controller), contents_(contents->GetWeakPtr()) {}

  void ShowAccountChooser() override {
    constexpr size_t kMaxShown = 8;
    Alert alert;
    alert.title = base::UTF16ToUTF8(controller_->GetAccountChooserTitle());
    alert.message =
        base::UTF16ToUTF8(url_formatter::FormatOriginForSecurityDisplay(
            controller_->GetOrigin()));
    const auto& forms = controller_->GetLocalForms();
    for (size_t i = 0; i < forms.size() && i < kMaxShown; ++i) {
      const std::u16string& name = forms[i]->username_value;
      alert.buttons.push_back(
          {"account" + base::NumberToString(i),
           base::UTF16ToUTF8(name.empty() ? forms[i]->display_name : name)});
    }
    shown_ = std::min(forms.size(), kMaxShown);
    alert.buttons.push_back(
        {"cancel",
         base::UTF16ToUTF8(l10n_util::GetStringUTF16(IDS_APP_CANCEL))});
    dismiss_ = UiPresenters::Get().ShowAlert(
        "passwords", contents_ ? ViewOfPage(contents_.get()) : nullptr,
        std::move(alert),
        base::BindOnce(&AccountChooserSheet::OnAnswer,
                       weak_factory_.GetWeakPtr()));
  }

  void ControllerGone() override {
    controller_ = nullptr;
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
  }

 private:
  void OnAnswer(int index) {
    dismiss_.Reset();
    if (!controller_) {
      return;
    }
    // Each answer may end the controller: OnCloseDialog has its delegate
    // destroy it at once and deletes this prompt only later, without
    // ControllerGone(), so the pointer is dropped first.
    CredentialManagerDialogController* controller = controller_.get();
    controller_ = nullptr;
    if (index >= 0 && static_cast<size_t>(index) < shown_) {
      controller->OnChooseCredentials(
          *controller->GetLocalForms()[index],
          password_manager::CredentialType::CREDENTIAL_TYPE_PASSWORD);
    } else {
      controller->OnCloseDialog();
    }
  }

  raw_ptr<CredentialManagerDialogController> controller_;
  base::WeakPtr<content::WebContents> contents_;
  size_t shown_ = 0;
  Dismisser dismiss_;
  base::WeakPtrFactory<AccountChooserSheet> weak_factory_{this};
};

// The notice after the first automatic sign-in: keep it (OK) or turn it off.
class AutoSigninSheet : public AutoSigninFirstRunPrompt {
 public:
  AutoSigninSheet(CredentialManagerDialogController* controller,
                  content::WebContents* contents)
      : controller_(controller), contents_(contents->GetWeakPtr()) {}

  void ShowAutoSigninPrompt() override {
    Alert alert;
    alert.title = base::UTF16ToUTF8(controller_->GetAutoSigninPromoTitle());
    alert.message = base::UTF16ToUTF8(controller_->GetAutoSigninText());
    alert.buttons.push_back({"ok", base::UTF16ToUTF8(l10n_util::GetStringUTF16(
                                       IDS_AUTO_SIGNIN_FIRST_RUN_OK))});
    alert.buttons.push_back(
        {"turnOff",
         base::UTF16ToUTF8(l10n_util::GetStringUTF16(IDS_TURN_OFF))});
    dismiss_ = UiPresenters::Get().ShowAlert(
        "passwords", contents_ ? ViewOfPage(contents_.get()) : nullptr,
        std::move(alert),
        base::BindOnce(&AutoSigninSheet::OnAnswer, weak_factory_.GetWeakPtr()));
  }

  void ControllerGone() override {
    controller_ = nullptr;
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
  }

 private:
  void OnAnswer(int index) {
    dismiss_.Reset();
    if (!controller_) {
      return;
    }
    // As in the account chooser, each answer may end the controller.
    CredentialManagerDialogController* controller = controller_.get();
    controller_ = nullptr;
    if (index == 0) {
      controller->OnAutoSigninOK();
    } else if (index == 1) {
      controller->OnAutoSigninTurnOff();
    } else {
      controller->OnCloseDialog();
    }
  }

  raw_ptr<CredentialManagerDialogController> controller_;
  base::WeakPtr<content::WebContents> contents_;
  Dismisser dismiss_;
  base::WeakPtrFactory<AutoSigninSheet> weak_factory_{this};
};

}  // namespace

void InstallCredentialManagerPrompts() {
  embedder_credential_manager_prompts::SetFactories(
      base::BindRepeating([](CredentialManagerDialogController* controller,
                             content::WebContents* contents)
                              -> std::unique_ptr<AccountChooserPrompt> {
        return std::make_unique<AccountChooserSheet>(controller, contents);
      }),
      base::BindRepeating([](CredentialManagerDialogController* controller,
                             content::WebContents* contents)
                              -> std::unique_ptr<AutoSigninFirstRunPrompt> {
        return std::make_unique<AutoSigninSheet>(controller, contents);
      }));
}

}  // namespace bdk
