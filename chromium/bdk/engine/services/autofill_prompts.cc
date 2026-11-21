// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/autofill_prompts.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/ui/autofill/autofill_bubble_base.h"
#include "chrome/browser/ui/autofill/autofill_bubble_handler.h"
#include "chrome/browser/ui/autofill/autofill_popup_controller.h"
#include "chrome/browser/ui/autofill/autofill_popup_view.h"
#include "chrome/browser/ui/autofill/payments/save_card_bubble_controller.h"
#include "chrome/browser/ui/autofill/save_address_bubble_controller.h"
#include "chrome/browser/ui/autofill/update_address_bubble_controller.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/autofill/popup/embedder_popup_view_factory.h"
#include "components/autofill/core/browser/data_model/payments/credit_card.h"
#include "components/autofill/core/browser/field_types.h"
#include "components/autofill/core/browser/foundations/autofill_client.h"
#include "components/autofill/core/browser/metrics/autofill_metrics.h"
#include "components/autofill/core/browser/suggestions/suggestion.h"
#include "components/autofill/core/browser/suggestions/suggestion_hiding_reason.h"
#include "components/autofill/core/browser/ui/payments/payments_ui_closed_reasons.h"
#include "components/signin/public/base/signin_buildflags.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"

namespace bdk {

namespace {

using autofill::AutofillBubbleBase;
using autofill::AutofillClient;

std::string Utf8(const std::u16string& text) {
  return base::UTF16ToUTF8(text);
}

// ---- Save and update prompts --------------------------------------------

// What Chromium's bubble views are to the controllers, for one `autofill`
// request: it raises the request, applies the answer to the controller and
// closes. It is its own owner, as Chromium's bubbles are: it deletes itself
// when the controller hides it or the question is over.
class SaveBubble : public AutofillBubbleBase {
 public:
  enum class Decision { kSave, kDecline };
  // How the bubble ended, for the controller's `closed`.
  enum class Outcome { kSaved, kDeclined, kIgnored };

  // `decide` applies a decision to the controller and `closed` tells the
  // controller the bubble is gone; both may use what `keep_alive` owns, which
  // lives as long as the bubble.
  static SaveBubble* Show(content::WebContents* contents,
                          base::DictValue payload,
                          Alert alert,
                          base::OnceCallback<void(Decision)> decide,
                          base::OnceCallback<void(Outcome)> closed,
                          std::shared_ptr<void> keep_alive) {
    auto* bubble = new SaveBubble(contents, std::move(payload),
                                  std::move(alert), std::move(decide),
                                  std::move(closed), std::move(keep_alive));
    bubble->Raise();
    return bubble;
  }

  // AutofillBubbleBase:
  void Hide() override {
    if (!closing_) {
      Close();
    }
  }
  bool IsMouseHovered() const override { return false; }

 private:
  SaveBubble(content::WebContents* contents,
             base::DictValue payload,
             Alert alert,
             base::OnceCallback<void(Decision)> decide,
             base::OnceCallback<void(Outcome)> closed,
             std::shared_ptr<void> keep_alive)
      : contents_(contents->GetWeakPtr()),
        payload_(std::move(payload)),
        alert_(std::move(alert)),
        decide_(std::move(decide)),
        closed_(std::move(closed)),
        keep_alive_(std::move(keep_alive)) {}
  ~SaveBubble() override = default;

  void Raise() {
    EngineContents* page = EngineContents::FromWebContents(contents_.get());
    if (page) {
      request_ = RaiseUiRequest(
          *page, BDK_SURFACE_AUTOFILL, payload_,
          base::BindOnce(&SaveBubble::Finished, weak_factory_.GetWeakPtr()));
    }
    if (!request_) {
      // No page, or the runtime is going away: nothing asks, nothing is saved.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&SaveBubble::Hide, weak_factory_.GetWeakPtr()));
    }
  }

  void Finished(const UiResult& result) {
    request_.reset();
    switch (result.outcome) {
      case bdkcore::UiOutcome::kCompleted:
        if (result.completion == "grant" || result.completion == "save") {
          // Saving personal data needs the person's own click or key press;
          // the router refused the completion without the gesture.
          Resolve(Decision::kSave);
        } else if (result.args.FindBool("dismissed").value_or(false)) {
          Close();  // no decision: Chromium treats it as ignored
        } else {
          Resolve(Decision::kDecline);
        }
        return;
      case bdkcore::UiOutcome::kDefaulted:
        ShowDefault();
        return;
      case bdkcore::UiOutcome::kWithdrawn:
        return;  // the page went away; Chromium hides the bubble itself
      default:   // rejected, cancelled, or the surface is disabled
        Close();
        return;
    }
  }

  void ShowDefault() {
    EngineContents* page = EngineContents::FromWebContents(contents_.get());
    if (!page) {
      Close();
      return;
    }
    dismiss_ = UiPresenters::Get().ShowAlert(
        "autofill", &page->host_view(), alert_,
        base::BindOnce(&SaveBubble::Answered, weak_factory_.GetWeakPtr()));
  }

  void Answered(int index) {
    dismiss_.Reset();
    if (index == 0) {
      Resolve(Decision::kSave);
    } else if (index == 1) {
      Resolve(Decision::kDecline);
    } else {
      Close();
    }
  }

  void Resolve(Decision decision) {
    closing_ = true;  // the controller hides the bubble while it applies this
    outcome_ =
        decision == Decision::kSave ? Outcome::kSaved : Outcome::kDeclined;
    std::move(decide_).Run(decision);
    closing_ = false;
    Close();
  }

  void Close() {
    closing_ = true;
    if (request_) {
      bdkcore::withdraw_ui_request(*std::exchange(request_, nullptr));
    }
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
    if (closed_) {
      std::move(closed_).Run(outcome_);
    }
    delete this;
  }

  base::WeakPtr<content::WebContents> contents_;
  const base::DictValue payload_;
  const Alert alert_;
  base::OnceCallback<void(Decision)> decide_;
  base::OnceCallback<void(Outcome)> closed_;
  Outcome outcome_ = Outcome::kIgnored;
  std::shared_ptr<void> keep_alive_;
  std::shared_ptr<bdkcore::Request> request_;
  Dismisser dismiss_;
  bool closing_ = false;
  base::WeakPtrFactory<SaveBubble> weak_factory_{this};
};

// A bubble of a server-driven or toolbar-bound flow, which BDK does not
// show. Chromium needs an object that is there for as long as it thinks the
// bubble is open; nothing ever shows.
class UnsupportedBubble : public AutofillBubbleBase {
 public:
  static UnsupportedBubble* Get() {
    static base::NoDestructor<UnsupportedBubble> bubble;
    return bubble.get();
  }
  void Hide() override {}
  bool IsMouseHovered() const override { return false; }
};

Alert SaveAlert(const std::string& title,
                const std::string& summary,
                const std::u16string& save,
                const std::u16string& decline) {
  Alert alert;
  alert.title = title;
  alert.message = summary;
  alert.buttons.push_back({"save", Utf8(save)});
  alert.buttons.push_back({"decline", Utf8(decline)});
  return alert;
}

base::DictValue SavePayload(const char* kind,
                            bool is_update,
                            const std::string& title,
                            const std::string& summary,
                            const std::string& message) {
  return base::DictValue()
      .Set("kind", kind)
      .Set("isUpdate", is_update)
      .Set("summary", summary)
      .Set("text",
           base::DictValue().Set("title", title).Set("message", message));
}

class BdkAutofillBubbleHandler : public autofill::AutofillBubbleHandler {
 public:
  explicit BdkAutofillBubbleHandler(BrowserWindowInterface* browser)
      : scoped_user_data_(browser->GetUnownedUserDataHost(), *this) {}

  AutofillBubbleBase* ShowSaveAddressProfileBubble(
      content::WebContents* web_contents,
      std::unique_ptr<autofill::SaveAddressBubbleController> controller,
      bool is_user_gesture) override {
    autofill::SaveAddressBubbleController* c = controller.get();
    const std::string title = Utf8(c->GetWindowTitle());
    const std::string summary = Utf8(c->GetAddressSummary());
    return SaveBubble::Show(
        web_contents,
        SavePayload("address", false, title, summary, Utf8(c->GetBodyText())),
        SaveAlert(title, summary, c->GetOkButtonLabel(),
                  c->GetNegativeButtonLabel()),
        base::BindOnce(
            [](autofill::SaveAddressBubbleController* c,
               SaveBubble::Decision decision) {
              if (decision == SaveBubble::Decision::kSave) {
                c->OnUserDecision(
                    AutofillClient::AddressPromptUserDecision::kAccepted,
                    c->GetAutofillProfile());
              } else {
                c->OnUserDecision(c->GetCancelCallbackValue(), std::nullopt);
              }
            },
            c),
        base::BindOnce([](autofill::SaveAddressBubbleController* c,
                          SaveBubble::Outcome) { c->OnBubbleClosed(); },
                       c),
        std::shared_ptr<void>(std::move(controller)));
  }

  AutofillBubbleBase* ShowUpdateAddressProfileBubble(
      content::WebContents* web_contents,
      std::unique_ptr<autofill::UpdateAddressBubbleController> controller,
      bool is_user_gesture) override {
    autofill::UpdateAddressBubbleController* c = controller.get();
    // The update prompt names the address that would change.
    const bool adds_to_existing = false;
    const std::string title = Utf8(c->GetWindowTitle(adds_to_existing));
    const std::string summary =
        Utf8(c->GetProfileToSave().GetInfo(autofill::NAME_FULL, "en-US"));
    return SaveBubble::Show(
        web_contents, SavePayload("address", true, title, summary, ""),
        SaveAlert(title, summary, c->GetPositiveButtonText(adds_to_existing),
                  c->GetNegativeButtonText(adds_to_existing)),
        base::BindOnce(
            [](autofill::UpdateAddressBubbleController* c,
               SaveBubble::Decision decision) {
              if (decision == SaveBubble::Decision::kSave) {
                c->OnUserDecision(
                    AutofillClient::AddressPromptUserDecision::kAccepted,
                    c->GetProfileToSave());
              } else {
                c->OnUserDecision(
                    AutofillClient::AddressPromptUserDecision::kDeclined,
                    std::nullopt);
              }
            },
            c),
        base::BindOnce([](autofill::UpdateAddressBubbleController* c,
                          SaveBubble::Outcome) { c->OnBubbleClosed(); },
                       c),
        std::shared_ptr<void>(std::move(controller)));
  }

  AutofillBubbleBase* ShowSaveCreditCardBubble(
      content::WebContents* web_contents,
      autofill::SaveCardBubbleController* controller,
      bool is_user_gesture) override {
    // The card number is never in the request: only what the person would
    // see, with the number masked.
    const std::string title = Utf8(controller->GetWindowTitle());
    const std::string summary =
        Utf8(controller->GetCard().NetworkAndLastFourDigits());
    return SaveBubble::Show(
        web_contents,
        SavePayload("card", false, title, summary,
                    Utf8(controller->GetExplanatoryMessage())),
        SaveAlert(title, summary, controller->GetAcceptButtonText(),
                  controller->GetDeclineButtonText()),
        base::BindOnce(
            [](autofill::SaveCardBubbleController* c,
               SaveBubble::Decision decision) {
              if (decision == SaveBubble::Decision::kSave) {
                // A card that needs a name or an expiry date from the person
                // is saved as it is; there is nothing to ask in a sheet.
                c->OnSaveButton({});
              }
            },
            controller),
        base::BindOnce(
            [](autofill::SaveCardBubbleController* c,
               SaveBubble::Outcome outcome) {
              c->OnBubbleClosed(
                  outcome == SaveBubble::Outcome::kSaved
                      ? autofill::PaymentsUiClosedReason::kAccepted
                  : outcome == SaveBubble::Outcome::kDeclined
                      ? autofill::PaymentsUiClosedReason::kCancelled
                      : autofill::PaymentsUiClosedReason::kClosed);
            },
            controller),
        nullptr);
  }

  // Everything below is not shown.
  AutofillBubbleBase* ShowIbanBubble(content::WebContents*,
                                     autofill::IbanBubbleController*,
                                     bool,
                                     autofill::IbanBubbleType) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowOfferNotificationBubble(
      content::WebContents*,
      autofill::OfferNotificationBubbleController*,
      bool) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowSaveAutofillAiDataBubble(
      content::WebContents*,
      autofill::AutofillAiImportDataController*) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowAutofillAiLocalSaveNotification(
      content::WebContents*,
      autofill::AutofillAiImportDataController*) override {
    return UnsupportedBubble::Get();
  }
#if BUILDFLAG(ENABLE_DICE_SUPPORT)
  AutofillBubbleBase* ShowAddressSignInPromo(
      content::WebContents*,
      const autofill::AutofillProfile&) override {
    return UnsupportedBubble::Get();
  }
#endif
  AutofillBubbleBase* ShowFilledCardInformationBubble(
      content::WebContents*,
      autofill::FilledCardInformationBubbleController*,
      bool) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowVirtualCardEnrollBubble(
      content::WebContents*,
      autofill::VirtualCardEnrollBubbleController*,
      bool) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowVirtualCardEnrollConfirmationBubble(
      content::WebContents*,
      autofill::VirtualCardEnrollBubbleController*) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowMandatoryReauthBubble(
      content::WebContents*,
      autofill::MandatoryReauthBubbleController*,
      bool,
      autofill::MandatoryReauthBubbleType) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowSaveCardConfirmationBubble(
      content::WebContents*,
      autofill::SaveCardBubbleController*) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowSaveIbanConfirmationBubble(
      content::WebContents*,
      autofill::IbanBubbleController*) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowOmniboxAutofillBubble(
      content::WebContents*,
      autofill::OmniboxAutofillBubbleController*) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowPaymentsChurnedUsersBubble(
      content::WebContents*,
      autofill::PaymentsChurnedUsersBubbleController*,
      bool) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowPaymentsChurnedUsersConfirmationBubble(
      content::WebContents*,
      autofill::PaymentsChurnedUsersBubbleController*) override {
    return UnsupportedBubble::Get();
  }
  AutofillBubbleBase* ShowWalletReminderNoticeBubble(
      content::WebContents*,
      autofill::WalletReminderNoticeBubbleController*,
      bool) override {
    return UnsupportedBubble::Get();
  }

 private:
  ui::ScopedUnownedUserData<autofill::AutofillBubbleHandler> scoped_user_data_;
};

// ---- The suggestion popup ----------------------------------------------

const char* KindOf(autofill::SuggestionType type) {
  switch (type) {
    case autofill::SuggestionType::kPasswordEntry:
    case autofill::SuggestionType::kBackupPasswordEntry:
    case autofill::SuggestionType::kAccountStoragePasswordEntry:
    case autofill::SuggestionType::kPasswordFieldByFieldFilling:
    case autofill::SuggestionType::kFillPassword:
      return "password";
    case autofill::SuggestionType::kGeneratePasswordEntry:
      return "generatedPassword";
    case autofill::SuggestionType::kCreditCardEntry:
    case autofill::SuggestionType::kVirtualCreditCardEntry:
    case autofill::SuggestionType::kSaveAndFillCreditCardEntry:
      return "card";
    default:
      return "address";
  }
}

// The popup of one controller, when the application shows the suggestions.
// It is what Chromium's popup is to the controller: the controller shows it,
// hides it, tells it the suggestions changed, and it gives back the one
// chosen. A handler that answers `.useDefault` gets Chromium's own popup,
// which this then forwards to.
class PopupView : public autofill::AutofillPopupView {
 public:
  PopupView(base::WeakPtr<autofill::AutofillPopupController> controller,
            std::optional<const SearchBarConfig> search_bar,
            std::optional<const TabbedPaneConfig> tabbed_pane,
            std::optional<const SubPopupConfig> sub_popup)
      : controller_(std::move(controller)),
        search_bar_(std::move(search_bar)),
        tabbed_pane_(std::move(tabbed_pane)),
        sub_popup_(std::move(sub_popup)) {}

  // AutofillPopupView:
  bool Show(autofill::AutoselectFirstSuggestion autoselect) override {
    autoselect_ = autoselect;
    shown_at_ = base::TimeTicks::Now();
    content::WebContents* contents =
        controller_ ? controller_->GetWebContents() : nullptr;
    EngineContents* page = EngineContents::FromWebContents(contents);
    if (!page) {
      return false;
    }
    contents_ = contents->GetWeakPtr();
    Raise(*page);
    return request_ != nullptr;
  }

  void Hide() override {
    if (request_) {
      bdkcore::withdraw_ui_request(*std::exchange(request_, nullptr));
    }
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
    if (inner_) {
      inner_->Hide();
    }
    delete this;
  }

  bool HandleKeyPressEvent(
      const input::NativeWebKeyboardEvent& event) override {
    return inner_ && inner_->HandleKeyPressEvent(event);
  }

  void OnSuggestionsChanged(bool prefer_prev_arrow_side) override {
    if (inner_) {
      inner_->OnSuggestionsChanged(prefer_prev_arrow_side);
      return;
    }
    // The request carries the list it was raised with: a new list is a new
    // request.
    EngineContents* page = EngineContents::FromWebContents(contents_.get());
    if (!page) {
      return;
    }
    if (request_) {
      bdkcore::withdraw_ui_request(*std::exchange(request_, nullptr));
    }
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
    Raise(*page);
  }

  bool OverlapsWithPictureInPictureWindow() const override {
    return inner_ && inner_->OverlapsWithPictureInPictureWindow();
  }

  void AxAnnounce(const std::u16string& text) override {
    if (inner_) {
      inner_->AxAnnounce(text);
    }
  }

  std::optional<int32_t> GetAxUniqueId() override {
    return inner_ ? inner_->GetAxUniqueId() : std::nullopt;
  }

  base::WeakPtr<autofill::AutofillPopupView> CreateSubPopupView(
      base::WeakPtr<autofill::AutofillPopupController> sub_controller)
      override {
    return inner_ ? inner_->CreateSubPopupView(std::move(sub_controller))
                  : nullptr;
  }

  std::optional<size_t> GetIndexOfSubPopupAnchorSuggestion() const override {
    return inner_ ? inner_->GetIndexOfSubPopupAnchorSuggestion() : std::nullopt;
  }

  bool HasFocus() const override { return inner_ && inner_->HasFocus(); }

  base::WeakPtr<autofill::AutofillPopupView> GetWeakPtr() override {
    return weak_factory_.GetWeakPtr();
  }

 private:
  ~PopupView() override = default;

  // Where the field is, relative to the page's view.
  gfx::RectF FieldRect() const {
    gfx::RectF bounds = controller_->element_bounds();
    if (content::WebContents* contents = controller_->GetWebContents()) {
      const gfx::Rect container = contents->GetContainerBounds();
      bounds.Offset(-container.x(), -container.y());
    }
    return bounds;
  }

  base::DictValue Payload() const {
    const gfx::RectF field = FieldRect();
    base::ListValue suggestions;
    for (int row = 0; row < controller_->GetLineCount(); ++row) {
      const autofill::Suggestion& suggestion =
          controller_->GetSuggestionAt(row);
      if (!suggestion.IsAcceptable()) {
        continue;  // titles, separators and footers are not choices
      }
      base::DictValue entry;
      // The id is the row; the engine hands it back to the controller.
      entry.Set("id", base::NumberToString(row));
      entry.Set("label", Utf8(suggestion.main_text.value));
      for (const auto& line : suggestion.labels) {
        if (!line.empty() && !line.front().value.empty()) {
          entry.Set("sublabel", Utf8(line.front().value));
          break;
        }
      }
      entry.Set("kind", KindOf(suggestion.type));
      suggestions.Append(std::move(entry));
    }
    return base::DictValue()
        .Set("fieldRect", base::DictValue()
                              .Set("x", field.x())
                              .Set("y", field.y())
                              .Set("width", field.width())
                              .Set("height", field.height()))
        .Set("suggestions", std::move(suggestions));
  }

  void Raise(EngineContents& page) {
    payload_ = Payload();
    const gfx::RectF field = FieldRect();
    request_ = RaiseUiRequest(
        page.Core(), BDK_SURFACE_AUTOFILL_SUGGESTIONS, payload_,
        base::BindOnce(&PopupView::Finished, weak_factory_.GetWeakPtr()),
        &page.host_view(),
        std::array<double, 2>{field.x(), field.y() + field.height()});
    // The person may choose only after the popup was in front of them for a
    // moment; the controller enforces that from here on.
    if (controller_) {
      controller_->OnPopupPainted();
    }
  }

  void Finished(const UiResult& result) {
    request_.reset();
    switch (result.outcome) {
      case bdkcore::UiOutcome::kCompleted: {
        const std::string* id = result.args.FindString("suggestion");
        int row = 0;
        // The router refused the completion without the gesture.
        if ((result.completion == "grant" || result.completion == "accept") &&
            id && base::StringToInt(*id, &row)) {
          Accept(row);
        } else {
          Dismiss();
        }
        return;
      }
      case bdkcore::UiOutcome::kDefaulted:
        ShowDefault();
        return;
      case bdkcore::UiOutcome::kWithdrawn:
        return;  // hidden by the controller, or a newer request took over
      default:   // rejected, cancelled, or the surface is disabled
        Dismiss();
        return;
    }
  }

  void ShowDefault() {
    base::WeakPtr<PopupView> self = weak_factory_.GetWeakPtr();
    if (std::optional<Dismisser> scripted =
            ScriptedPresent("autofill_suggestions", payload_,
                            base::BindOnce(&PopupView::Scripted, self))) {
      dismiss_ = std::move(*scripted);
      return;
    }
    // Chromium's own popup, anchored in the page's native view.
    inner_ = autofill::embedder_popup_view_factory::CreateViewsPopupView(
        controller_, search_bar_, tabbed_pane_, sub_popup_);
    if (!inner_ || !inner_->Show(autoselect_)) {
      Dismiss();
    }
  }

  // A script chose the suggestion with this label, or none.
  void Scripted(const base::DictValue& answer) {
    dismiss_.Reset();
    const std::string* label = answer.FindString("answer");
    if (!label || !controller_) {
      Dismiss();
      return;
    }
    for (const base::Value& entry : *payload_.FindList("suggestions")) {
      const base::DictValue& suggestion = entry.GetDict();
      int row = 0;
      if (*suggestion.FindString("label") == *label &&
          base::StringToInt(*suggestion.FindString("id"), &row)) {
        Accept(row);
        return;
      }
    }
    Dismiss();
  }

  void Accept(int row) {
    if (!controller_ || row < 0 || row >= controller_->GetLineCount()) {
      return;
    }
    const base::TimeDelta wait = shown_at_ +
                                 autofill::AutofillSuggestionController::
                                     kIgnoreEarlyClicksOnSuggestionsDuration +
                                 base::Milliseconds(100) -
                                 base::TimeTicks::Now();
    if (wait.is_positive()) {
      // Chromium ignores a choice made right after the popup appeared.
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&PopupView::AcceptNow, weak_factory_.GetWeakPtr(),
                         row),
          wait);
      return;
    }
    AcceptNow(row);
  }

  void AcceptNow(int row) {
    if (controller_) {
      controller_->AcceptSuggestion(
          row, autofill::AutofillMetrics::SuggestionAcceptedMethod::kMouse,
          /*was_obscured=*/false);
    }
  }

  void Dismiss() {
    if (controller_) {
      controller_->Hide(autofill::SuggestionHidingReason::kUserAborted);
    }
  }

  base::WeakPtr<autofill::AutofillPopupController> controller_;
  const std::optional<const SearchBarConfig> search_bar_;
  const std::optional<const TabbedPaneConfig> tabbed_pane_;
  const std::optional<const SubPopupConfig> sub_popup_;
  base::WeakPtr<content::WebContents> contents_;
  autofill::AutoselectFirstSuggestion autoselect_{false};
  base::TimeTicks shown_at_;
  base::DictValue payload_;
  std::shared_ptr<bdkcore::Request> request_;
  Dismisser dismiss_;
  base::WeakPtr<autofill::AutofillPopupView> inner_;
  base::WeakPtrFactory<PopupView> weak_factory_{this};
};

base::WeakPtr<autofill::AutofillPopupView> CreatePopupView(
    base::WeakPtr<autofill::AutofillPopupController> controller,
    std::optional<const autofill::AutofillPopupView::SearchBarConfig>
        search_bar,
    std::optional<const autofill::AutofillPopupView::TabbedPaneConfig>
        tabbed_pane,
    std::optional<const autofill::AutofillPopupView::SubPopupConfig>
        sub_popup) {
  content::WebContents* contents =
      controller ? controller->GetWebContents() : nullptr;
  EngineContents* page = EngineContents::FromWebContents(contents);
  if (!page) {
    return autofill::embedder_popup_view_factory::CreateViewsPopupView(
        std::move(controller), std::move(search_bar), std::move(tabbed_pane),
        std::move(sub_popup));
  }
  std::shared_ptr<bdkcore::Contents> core = page->Core();
  if (!SurfaceAvailable(core, BDK_SURFACE_AUTOFILL_SUGGESTIONS) ||
      !SurfaceAvailable(core, BDK_SURFACE_AUTOFILL)) {
    // `autofill` disabled: nothing is offered, so nothing is filled. (The
    // suggestion surface itself has no disabled mode.)
    return nullptr;
  }
  const bool scripted = PresenterScripted("autofill_suggestions");
  if (!SurfaceHasHandler(core, BDK_SURFACE_AUTOFILL_SUGGESTIONS) && !scripted) {
    // No one to ask: Chromium's popup, as it is.
    base::WeakPtr<autofill::AutofillPopupView> view =
        autofill::embedder_popup_view_factory::CreateViewsPopupView(
            std::move(controller), std::move(search_bar),
            std::move(tabbed_pane), std::move(sub_popup));
    if (view) {
      RecordPresented("autofill_suggestions", base::DictValue());
    }
    return view;
  }
  return (new PopupView(std::move(controller), std::move(search_bar),
                        std::move(tabbed_pane), std::move(sub_popup)))
      ->GetWeakPtr();
}

}  // namespace

std::unique_ptr<autofill::AutofillBubbleHandler> CreateAutofillBubbleHandler(
    BrowserWindowInterface* browser) {
  return std::make_unique<BdkAutofillBubbleHandler>(browser);
}

void InstallAutofillPopupHook() {
  autofill::embedder_popup_view_factory::SetFactory(
      base::BindRepeating(&CreatePopupView));
}

}  // namespace bdk
