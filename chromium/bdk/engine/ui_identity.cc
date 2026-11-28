// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_identity.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/webid/identity_request_account.h"
#include "content/public/browser/webid/identity_request_dialog_controller.h"
#include "third_party/blink/public/mojom/webid/federated_request.mojom.h"
#include "ui/base/window_open_disposition.h"
#include "url/origin.h"

namespace bdk {

namespace {

using content::IdentityRequestAccount;
using content::IdentityRequestDialogController;
using DismissReason = IdentityRequestDialogController::DismissReason;

const char* ContextName(blink::mojom::RpContext context) {
  switch (context) {
    case blink::mojom::RpContext::kSignIn:
      return "signin";
    case blink::mojom::RpContext::kSignUp:
      return "signup";
    case blink::mojom::RpContext::kUse:
      return "use";
    case blink::mojom::RpContext::kContinue:
      return "continue";
  }
  return "signin";
}

base::DictValue ProviderJson(const content::IdentityProviderData& idp) {
  return base::DictValue()
      .Set("id", idp.idp_metadata.config_url.spec())
      .Set("configURL", idp.idp_metadata.config_url.spec())
      .Set("name", idp.idp_for_display);
}

class Controller : public IdentityRequestDialogController {
 public:
  explicit Controller(content::WebContents* rp) : rp_(rp->GetWeakPtr()) {}

  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  ~Controller() override { EndStep(); }

  bool ShowAccountsDialog(
      content::RelyingPartyData rp_data,
      const std::vector<scoped_refptr<content::IdentityProviderData>>& idp_list,
      const std::vector<scoped_refptr<IdentityRequestAccount>>& accounts,
      const std::vector<scoped_refptr<IdentityRequestAccount>>&
          filtered_accounts,
      blink::mojom::RpMode rp_mode,
      AccountSelectionCallback on_selected,
      LoginToIdPCallback on_add_account,
      DismissCallback dismiss_callback,
      AccountsDisplayedCallback accounts_displayed_callback) override {
    EndStep();
    on_selected_ = std::move(on_selected);
    login_ = std::move(on_add_account);
    dismiss_ = std::move(dismiss_callback);
    accounts_.clear();
    base::DictValue payload = Step("chooseAccount", rp_mode);
    base::ListValue providers;
    for (const scoped_refptr<content::IdentityProviderData>& idp : idp_list) {
      providers.Append(ProviderJson(*idp));
      login_urls_[idp->idp_metadata.config_url.spec()] =
          idp->idp_metadata.idp_login_url;
      payload.Set("context", ContextName(idp->rp_context));
    }
    payload.Set("identityProviders", std::move(providers));
    base::ListValue list;
    for (const scoped_refptr<IdentityRequestAccount>& account : accounts) {
      const std::string idp =
          account->identity_provider
              ? account->identity_provider->idp_metadata.config_url.spec()
              : std::string();
      const bool returning = account->browser_trusted_login_state ==
                             IdentityRequestAccount::LoginState::kSignIn;
      // Two providers may both offer an account "1": a choice is the pair.
      accounts_.push_back({GURL(idp), account->id, returning});
      base::DictValue entry;
      entry.Set("id", account->id);
      entry.Set("idp", idp);
      entry.Set("name", account->name);
      entry.Set("email", account->email);
      entry.Set("givenName", account->given_name);
      if (account->picture.is_valid()) {
        entry.Set("picture", account->picture.spec());
      }
      entry.Set("isReturning", returning);
      list.Append(std::move(entry));
    }
    payload.Set("accounts", std::move(list));
    Raise(std::move(payload));
    if (accounts_displayed_callback) {
      std::move(accounts_displayed_callback).Run();
    }
    return true;
  }

  bool ShowFailureDialog(
      const content::RelyingPartyData& rp_data,
      const std::string& idp_for_display,
      blink::mojom::RpContext rp_context,
      blink::mojom::RpMode rp_mode,
      const content::IdentityProviderMetadata& idp_metadata,
      const std::vector<scoped_refptr<IdentityRequestAccount>>&
          filtered_accounts,
      DismissCallback dismiss_callback,
      LoginToIdPCallback login_callback) override {
    EndStep();
    dismiss_ = std::move(dismiss_callback);
    login_ = std::move(login_callback);
    login_urls_[idp_metadata.config_url.spec()] = idp_metadata.idp_login_url;
    base::DictValue payload = Step("failure", rp_mode);
    payload.Set("context", ContextName(rp_context));
    payload.Set("identityProviders",
                base::ListValue().Append(
                    base::DictValue()
                        .Set("id", idp_metadata.config_url.spec())
                        .Set("configURL", idp_metadata.config_url.spec())
                        .Set("name", idp_for_display)));
    Raise(std::move(payload));
    return true;
  }

  bool ShowErrorDialog(
      const content::RelyingPartyData& rp_data,
      const std::string& idp_for_display,
      blink::mojom::RpContext rp_context,
      blink::mojom::RpMode rp_mode,
      const content::IdentityProviderMetadata& idp_metadata,
      const std::optional<content::IdentityCredentialTokenError>& error,
      DismissCallback dismiss_callback,
      MoreDetailsCallback more_details_callback) override {
    EndStep();
    dismiss_ = std::move(dismiss_callback);
    more_details_ = std::move(more_details_callback);
    base::DictValue payload = Step("error", rp_mode);
    payload.Set("context", ContextName(rp_context));
    payload.Set("identityProviders",
                base::ListValue().Append(
                    base::DictValue()
                        .Set("id", idp_metadata.config_url.spec())
                        .Set("configURL", idp_metadata.config_url.spec())
                        .Set("name", idp_for_display)));
    base::DictValue detail;
    if (error) {
      detail.Set("code", error->code);
      if (error->url.is_valid()) {
        detail.Set("url", error->url.spec());
      }
    }
    payload.Set("error", std::move(detail));
    Raise(std::move(payload));
    return true;
  }

  bool ShowLoadingDialog(const content::RelyingPartyData& rp_data,
                         const std::string& idp_for_display,
                         blink::mojom::RpContext rp_context,
                         blink::mojom::RpMode rp_mode,
                         DismissCallback dismiss_callback) override {
    EndStep();
    dismiss_ = std::move(dismiss_callback);
    base::DictValue payload = Step("loading", rp_mode);
    payload.Set("context", ContextName(rp_context));
    Raise(std::move(payload));
    return true;
  }

  bool ShowVerifyingDialog(
      const content::RelyingPartyData& rp_data,
      const scoped_refptr<content::IdentityProviderData>& idp_data,
      const scoped_refptr<IdentityRequestAccount>& account,
      IdentityRequestAccount::SignInMode sign_in_mode,
      blink::mojom::RpMode rp_mode,
      AccountsDisplayedCallback accounts_displayed_callback) override {
    EndStep();
    base::DictValue payload = Step("verifying", rp_mode);
    if (idp_data) {
      payload.Set("identityProviders",
                  base::ListValue().Append(ProviderJson(*idp_data)));
    }
    Raise(std::move(payload));
    if (accounts_displayed_callback) {
      std::move(accounts_displayed_callback).Run();
    }
    return true;
  }

  void ShowUrl(LinkType type, const GURL& url) override {
    OpenProviderPage(url);
  }

  content::WebContents* ShowModalDialog(
      const GURL& url,
      blink::mojom::RpMode rp_mode,
      DismissCallback dismiss_callback,
      ShownModalAsyncCallback on_shown_async,
      NativeAppResultCallback native_result_callback) override {
    modal_dismiss_ = std::move(dismiss_callback);
    content::WebContents* page = OpenProviderPage(url);
    if (page && on_shown_async) {
      std::move(on_shown_async).Run(page);
    }
    return page;
  }

  void CloseModalDialog() override {
    if (modal_page_) {
      modal_page_->Close();
    }
    modal_page_ = nullptr;
  }

  content::WebContents* GetRpWebContents() override { return rp_.get(); }

  void RequestIdPRegistrationPermision(
      const url::Origin& origin,
      base::OnceCallback<void(bool accepted)> callback) override {
    EndStep();
    register_ = std::move(callback);
    Raise(Step("registerIdP", blink::mojom::RpMode::kActive));
  }

 private:
  // The fields every step has: which step, and who asks.
  base::DictValue Step(const char* step, blink::mojom::RpMode mode) {
    base::DictValue payload;
    payload.Set("step", step);
    base::DictValue party;
    if (rp_) {
      party.Set(
          "origin",
          rp_->GetPrimaryMainFrame()->GetLastCommittedOrigin().Serialize());
    }
    payload.Set("relyingParty", std::move(party));
    payload.Set("mode",
                mode == blink::mojom::RpMode::kActive ? "active" : "passive");
    return payload;
  }

  void Raise(base::DictValue payload) {
    EngineContents* page =
        rp_ ? EngineContents::FromWebContents(rp_.get()) : nullptr;
    if (!page) {
      Dismiss();
      return;
    }
    page_ = page->GetWeakPtr();
    // A handler may answer at once, inside RaiseUiRequest, and an answer can
    // make Chromium destroy this controller.
    base::WeakPtr<Controller> self = weak_factory_.GetWeakPtr();
    step_ended_ = false;
    std::shared_ptr<bdkcore::Request> request = RaiseUiRequest(
        *page, BDK_SURFACE_IDENTITY, payload,
        base::BindOnce(&Controller::OnEnded, self, payload.Clone()));
    if (self && !step_ended_) {
      request_ = std::move(request);
    }
  }

  // A new step withdraws the open one, as Chromium moves on.
  void EndStep() {
    if (request_) {
      std::shared_ptr<bdkcore::Request> open = std::move(request_);
      bdkcore::withdraw_ui_request(*open);
    }
    if (sheet_) {
      std::move(sheet_).Run();
    }
  }

  void OnEnded(base::DictValue payload, const UiResult& result) {
    step_ended_ = true;
    request_.reset();
    if (result.outcome == bdkcore::UiOutcome::kWithdrawn) {
      return;
    }
    const std::string step = *payload.FindString("step");
    if (result.outcome == bdkcore::UiOutcome::kCompleted) {
      Answer(step, result.completion, result.args);
      return;
    }
    if (result.outcome == bdkcore::UiOutcome::kDefaulted && page_) {
      ShowSheet(step, payload);
      return;
    }
    Answer(step, "deny", base::DictValue());
  }

  void Answer(const std::string& step,
              const std::string& completion,
              const base::DictValue& args) {
    if (completion == "grant" && step == "chooseAccount") {
      const std::string* account = args.FindString("account");
      const AccountChoice* chosen =
          account ? FindAccount(*account, args.FindString("idp")) : nullptr;
      if (chosen && on_selected_) {
        // Running the callback may destroy this controller and its accounts.
        const AccountChoice choice = *chosen;
        std::move(on_selected_).Run(choice.idp, choice.id, choice.returning);
        return;
      }
    }
    if (step == "registerIdP") {
      if (register_) {
        std::move(register_).Run(completion == "grant");
      }
      return;
    }
    if (completion == "accept") {
      const std::string* action = args.FindString("action");
      if (action && *action == "signInToIdP" && login_) {
        const std::string* idp = args.FindString("idp");
        auto it = idp ? login_urls_.find(*idp) : login_urls_.end();
        if (it != login_urls_.end()) {
          login_.Run(GURL(it->first), it->second);
          return;
        }
      }
      if (action && *action == "moreDetails" && more_details_) {
        std::move(more_details_).Run();
        return;
      }
    }
    if (step == "loading" || step == "verifying") {
      return;  // nothing to answer; Chromium moves on by itself
    }
    Dismiss();
  }

  // BDK's sheets (ui-requests.md 5.22): Cancel first, then the choices.
  void ShowSheet(const std::string& step, const base::DictValue& payload) {
    Alert alert;
    std::vector<std::pair<std::string, base::DictValue>> answers;
    const std::string* origin =
        payload.FindDict("relyingParty")
            ? payload.FindDict("relyingParty")->FindString("origin")
            : nullptr;
    const std::string site = origin ? *origin : std::string();
    alert.buttons.push_back({"cancel", "Cancel"});
    answers.emplace_back("deny", base::DictValue());
    if (step == "chooseAccount") {
      alert.title = "Sign in to " + site;
      for (const base::Value& value : *payload.FindList("accounts")) {
        const base::DictValue& account = value.GetDict();
        alert.buttons.push_back({*account.FindString("id"),
                                 "Continue as " + *account.FindString("name") +
                                     " (" + *account.FindString("email") +
                                     ")"});
        answers.emplace_back("grant",
                             base::DictValue()
                                 .Set("account", *account.FindString("id"))
                                 .Set("idp", *account.FindString("idp")));
      }
    } else if (step == "failure") {
      const base::DictValue& idp =
          payload.FindList("identityProviders")->front().GetDict();
      alert.title = "Sign in to " + site + " with " + *idp.FindString("name");
      alert.message =
          "You are not signed in to " + *idp.FindString("name") + ".";
      alert.buttons.push_back({"signInToIdP", "Continue"});
      answers.emplace_back("accept", base::DictValue()
                                         .Set("action", "signInToIdP")
                                         .Set("idp", *idp.FindString("id")));
    } else if (step == "error") {
      alert.title = "Something went wrong signing in to " + site;
      alert.buttons.push_back({"moreDetails", "More details"});
      answers.emplace_back("accept",
                           base::DictValue().Set("action", "moreDetails"));
    } else if (step == "registerIdP") {
      alert.title = site + " wants to be a sign-in provider";
      alert.buttons.push_back({"allow", "Allow"});
      answers.emplace_back("grant", base::DictValue());
    } else {
      return;  // loading and verifying show nothing of their own
    }
    sheet_ = UiPresenters::Get().ShowAlert(
        "identity", &page_->host_view(), std::move(alert),
        base::BindOnce(
            [](base::WeakPtr<Controller> self, std::string step,
               std::vector<std::pair<std::string, base::DictValue>> answers,
               int index) {
              if (!self) {
                return;
              }
              self->sheet_.Reset();
              const size_t at =
                  index >= 0 && static_cast<size_t>(index) < answers.size()
                      ? static_cast<size_t>(index)
                      : 0;
              self->Answer(step, answers[at].first, answers[at].second);
            },
            weak_factory_.GetWeakPtr(), step, std::move(answers)));
  }

  // Either callback may destroy this controller: both are taken first.
  void Dismiss() {
    DismissCallback dismiss = std::move(dismiss_);
    base::OnceCallback<void(bool)> register_answer = std::move(register_);
    if (dismiss) {
      std::move(dismiss).Run(DismissReason::kCloseButton);
    }
    if (register_answer) {
      std::move(register_answer).Run(false);
    }
  }

  // An identity provider's page: a new-window request of kind
  // identityProvider, in the site's profile.
  content::WebContents* OpenProviderPage(const GURL& url) {
    EngineContents* page =
        rp_ ? EngineContents::FromWebContents(rp_.get()) : nullptr;
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    Profile* profile =
        rp_ ? Profile::FromBrowserContext(rp_->GetBrowserContext()) : nullptr;
    std::shared_ptr<bdkcore::Profile> bdk_profile =
        rt && profile ? BdkProfileOf(*rt, profile) : nullptr;
    if (!page || !bdk_profile) {
      return nullptr;
    }
    std::unique_ptr<content::WebContents> created =
        content::WebContents::Create(
            content::WebContents::CreateParams(rp_->GetBrowserContext()));
    content::WebContents* raw = created.get();
    raw->GetController().LoadURLWithParams(
        content::NavigationController::LoadURLParams(url));
    EngineContents* opened = EngineContents::RequestNewPage(
        bdk_profile, page->Core(), std::move(created), "identityProvider",
        WindowOpenDisposition::kNewPopup, url, nullptr,
        /*user_gesture=*/true, std::string());
    if (!opened) {
      return nullptr;
    }
    modal_page_ = opened->GetWeakPtr();
    return raw;
  }

  struct AccountChoice {
    GURL idp;
    std::string id;  // the provider's own account id
    bool returning = false;
  };

  // The offered account a grant names: `id` of the provider `idp`. Without
  // `idp`, only an id that one provider alone offers names an account.
  const AccountChoice* FindAccount(const std::string& id,
                                   const std::string* idp) const {
    const AccountChoice* found = nullptr;
    for (const AccountChoice& choice : accounts_) {
      if (choice.id != id || (idp && choice.idp.spec() != *idp)) {
        continue;
      }
      if (found) {
        return nullptr;  // ambiguous: the grant must name the provider
      }
      found = &choice;
    }
    return found;
  }

  base::WeakPtr<content::WebContents> rp_;
  base::WeakPtr<EngineContents> page_;
  base::WeakPtr<EngineContents> modal_page_;
  std::shared_ptr<bdkcore::Request> request_;
  Dismisser sheet_;
  std::vector<AccountChoice> accounts_;
  std::map<std::string, GURL> login_urls_;
  AccountSelectionCallback on_selected_;
  LoginToIdPCallback login_;
  DismissCallback dismiss_;
  DismissCallback modal_dismiss_;
  MoreDetailsCallback more_details_;
  base::OnceCallback<void(bool)> register_;
  bool step_ended_ = false;
  base::WeakPtrFactory<Controller> weak_factory_{this};
};

}  // namespace

std::unique_ptr<content::IdentityRequestDialogController>
CreateIdentityDialogController(content::WebContents* rp_web_contents) {
  return std::make_unique<Controller>(rp_web_contents);
}

}  // namespace bdk
