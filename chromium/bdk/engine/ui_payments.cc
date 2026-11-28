// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_payments.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/payments/embedder_payment_request_dialog.h"  // nogncheck
#include "chrome/browser/profiles/profile.h"
#include "components/payments/content/payment_app.h"
#include "components/payments/content/payment_handler_navigation_throttle.h"
#include "components/payments/content/payment_request.h"
#include "components/payments/content/payment_request_dialog.h"
#include "components/payments/content/payment_request_spec.h"
#include "components/payments/content/payment_request_state.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/payment_app_provider.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/mojom/payments/payment_request.mojom.h"
#include "ui/base/webui/web_ui_util.h"
#include "ui/base/window_open_disposition.h"
#include "url/origin.h"

namespace bdk {

namespace {

base::DictValue ItemJson(const payments::mojom::PaymentItemPtr& item) {
  base::DictValue entry;
  entry.Set("label", item ? item->label : std::string());
  entry.Set("currency",
            item && item->amount ? item->amount->currency : std::string());
  entry.Set("value",
            item && item->amount ? item->amount->value : std::string());
  return entry;
}

// The payment handler's page, as Chrome's payment handler view watches it:
// Chromium learns of it once its first navigation ends; the person's input in
// it is what lets the handler's answer resolve the page's show() (Chromium
// holds an answer until the person has used the handler's page,
// kPaymentRequestMandatoryPaymentAppUi); closing it cancels the payment.
class HandlerPage : public content::WebContentsObserver {
 public:
  HandlerPage(content::WebContents* contents,
              base::WeakPtr<payments::PaymentRequestState> state,
              payments::PaymentHandlerOpenWindowCallback opened,
              base::OnceClosure closed)
      : content::WebContentsObserver(contents),
        state_(std::move(state)),
        opened_(std::move(opened)),
        closed_(std::move(closed)) {}

  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (!opened_ || !navigation->IsInPrimaryMainFrame()) {
      return;
    }
    content::RenderFrameHost* frame = web_contents()->GetPrimaryMainFrame();
    const bool ok = navigation->HasCommitted() && !navigation->IsErrorPage();
    std::move(opened_).Run(ok, frame->GetProcess()->GetDeprecatedID(),
                           frame->GetRoutingID());
  }

  void DidGetUserInteraction(const blink::WebInputEvent& event) override {
    if (state_) {
      state_->set_user_interaction_in_web_payment_app(true);
    }
  }

  void WebContentsDestroyed() override {
    if (opened_) {
      std::move(opened_).Run(false, -1, -1);
    }
    if (closed_) {
      std::move(closed_).Run();
    }
  }

  // The payment ended: the page goes without cancelling anything.
  void Forget() { closed_.Reset(); }

 private:
  base::WeakPtr<payments::PaymentRequestState> state_;
  payments::PaymentHandlerOpenWindowCallback opened_;
  base::OnceClosure closed_;
};

// BDK's PaymentRequestDialog: a `payments` request once Chromium knows which
// apps can pay. It owns itself until CloseDialog(), as Chrome's sheet does.
class PaymentSheet : public payments::PaymentRequestDialog,
                     public payments::PaymentRequestState::Observer {
 public:
  explicit PaymentSheet(base::WeakPtr<payments::PaymentRequest> request)
      : request_(std::move(request)) {}

  PaymentSheet(const PaymentSheet&) = delete;
  PaymentSheet& operator=(const PaymentSheet&) = delete;

  ~PaymentSheet() override {
    if (request_ && request_->state()) {
      request_->state()->RemoveObserver(this);
    }
  }

  base::WeakPtr<PaymentSheet> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // payments::PaymentRequestDialog:
  void ShowDialog() override {
    if (!request_ || !request_->state()) {
      return;
    }
    request_->state()->AddObserver(this);
    if (request_->state()->is_get_all_apps_finished()) {
      RaiseSoon();
    }
  }

  void RetryDialog() override { RaiseSoon(); }

  void CloseDialog() override {
    if (closed_) {
      return;
    }
    closed_ = true;
    if (request_ui_) {
      bdkcore::withdraw_ui_request(*request_ui_);
      request_ui_.reset();
    }
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
    if (handler_watch_) {
      handler_watch_->Forget();
    }
    if (handler_page_) {
      handler_page_->Close();
    }
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  // Chromium rejects show() itself; the request is already answered.
  void ShowErrorMessage() override {}
  void ShowProcessingSpinner() override { processing_ = true; }
  void ShowLoadingView() override {}
  bool IsInteractive() const override { return !processing_; }

  // A payment handler's own page (PaymentRequestEvent.openWindow()): a
  // new-window request of kind paymentHandler, in the page's profile.
  void ShowPaymentHandlerScreen(
      const GURL& url,
      const std::optional<gfx::Size>& preferred_size,
      payments::PaymentHandlerOpenWindowCallback done) override {
    content::WebContents* page_contents =
        request_ && request_->state() ? request_->state()->GetWebContents()
                                      : nullptr;
    EngineContents* page = page_contents
                               ? EngineContents::FromWebContents(page_contents)
                               : nullptr;
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    Profile* profile =
        page_contents
            ? Profile::FromBrowserContext(page_contents->GetBrowserContext())
            : nullptr;
    std::shared_ptr<bdkcore::Profile> bdk_profile =
        rt && profile ? BdkProfileOf(*rt, profile) : nullptr;
    if (!page || !bdk_profile) {
      std::move(done).Run(false, -1, -1);
      return;
    }
    std::unique_ptr<content::WebContents> created =
        content::WebContents::Create(content::WebContents::CreateParams(
            page_contents->GetBrowserContext()));
    content::WebContents* raw = created.get();
    payments::PaymentHandlerNavigationThrottle::MarkPaymentHandlerWebContents(
        raw);
    content::PaymentAppProvider::GetOrCreateForWebContents(page_contents)
        ->SetOpenedWindow(raw);
    handler_watch_ = std::make_unique<HandlerPage>(
        raw, request_->state(), std::move(done),
        base::BindOnce(&PaymentSheet::Cancel, weak_factory_.GetWeakPtr()));
    raw->GetController().LoadURLWithParams(
        content::NavigationController::LoadURLParams(url));
    EngineContents* handler = EngineContents::RequestNewPage(
        bdk_profile, page->Core(), std::move(created), "paymentHandler",
        WindowOpenDisposition::kNewPopup, url, nullptr,
        /*user_gesture=*/true, std::string());
    handler_page_ = handler ? handler->GetWeakPtr() : nullptr;
  }

  void ConfirmPaymentForTesting() override {}
  bool ClickOptOutForTesting() override { return false; }

  // payments::PaymentRequestState::Observer:
  void OnGetAllPaymentAppsFinished() override { RaiseSoon(); }
  void OnSelectedInformationChanged() override {}

 private:
  // Chromium is still inside ShowDialog() or its observers here: a refusal
  // made there is lost (the page's show() never settles), so the request is
  // raised, or the payment refused, from a task of its own.
  void RaiseSoon() {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&PaymentSheet::Raise, weak_factory_.GetWeakPtr()));
  }

  // Raises the request, or refuses what BDK does not ask for.
  void Raise() {
    if (closed_ || request_ui_ || dismiss_ || !request_ || !request_->spec() ||
        !request_->state()) {
      return;
    }
    payments::PaymentRequestSpec* spec = request_->spec().get();
    payments::PaymentRequestState* state = request_->state().get();
    // Shipping and payer details need address and contact pickers that BDK
    // does not have (ui-requests.md 5.22): such a request is refused.
    if (spec->request_shipping() || spec->request_payer_name() ||
        spec->request_payer_email() || spec->request_payer_phone()) {
      request_->OnInternalError(
          "this browser does not ask for shipping or payer details");
      return;
    }
    if (state->available_apps().empty()) {
      request_->OnInternalError("no payment app can pay this request");
      return;
    }
    EngineContents* page =
        EngineContents::FromWebContents(state->GetWebContents());
    if (!page) {
      request_->OnUserCancelled();
      return;
    }
    base::DictValue payload;
    payload.Set("kind", "paymentRequest");
    payload.Set("origin",
                url::Origin::Create(state->GetFrameOrigin()).Serialize());
    payload.Set("topLevelOrigin",
                url::Origin::Create(state->GetTopOrigin()).Serialize());
    payments::PaymentApp* first = state->available_apps().front().get();
    payload.Set("total", ItemJson(spec->GetTotal(first)));
    base::ListValue items;
    for (const payments::mojom::PaymentItemPtr* item :
         spec->GetDisplayItems(first)) {
      items.Append(ItemJson(*item));
    }
    payload.Set("displayItems", std::move(items));
    base::ListValue apps;
    for (size_t i = 0; i < state->available_apps().size(); ++i) {
      const payments::PaymentApp& app = *state->available_apps()[i];
      base::DictValue entry;
      entry.Set("id", "app" + base::NumberToString(i));
      entry.Set("name", base::UTF16ToUTF8(app.GetLabel()));
      const GURL origin(base::UTF16ToUTF8(app.GetSublabel()));
      if (origin.is_valid()) {
        entry.Set("origin", url::Origin::Create(origin).Serialize());
      } else if (GURL(app.GetId()).is_valid()) {
        entry.Set("origin", url::Origin::Create(GURL(app.GetId())).Serialize());
      }
      if (const SkBitmap* icon = app.icon_bitmap();
          icon && !icon->drawsNothing()) {
        entry.Set("icon", webui::GetBitmapDataUrl(*icon));
      }
      apps.Append(std::move(entry));
    }
    payload.Set("apps", std::move(apps));
    payload.Set("requests", base::DictValue()
                                .Set("shipping", false)
                                .Set("payerName", false)
                                .Set("payerEmail", false)
                                .Set("payerPhone", false));
    page_ = page->GetWeakPtr();
    request_ui_ = RaiseUiRequest(
        *page, BDK_SURFACE_PAYMENTS, payload,
        base::BindOnce(&PaymentSheet::OnEnded, weak_factory_.GetWeakPtr(),
                       payload.Clone()));
    if (!request_ui_ && request_) {
      request_->OnUserCancelled();
    }
  }

  void OnEnded(base::DictValue payload, const UiResult& result) {
    request_ui_.reset();
    if (closed_) {
      return;
    }
    if (result.outcome == bdkcore::UiOutcome::kCompleted) {
      const std::string* app = result.args.FindString("app");
      if (result.completion == "grant" && app) {
        PayWith(*app);
      } else {
        Cancel();
      }
      return;
    }
    if (result.outcome != bdkcore::UiOutcome::kDefaulted || !page_) {
      Cancel();
      return;
    }
    // The default presenter: the total and who asks, Cancel first (Return),
    // and one button for each app, which names the app and its origin.
    Alert alert;
    const base::DictValue* total = payload.FindDict("total");
    alert.title = "Pay " +
                  (total && total->FindString("value")
                       ? *total->FindString("currency") + " " +
                             *total->FindString("value")
                       : std::string()) +
                  "?";
    alert.message = "Requested by " + *payload.FindString("origin");
    alert.buttons.push_back({"cancel", "Cancel"});
    const base::ListValue* apps = payload.FindList("apps");
    for (const base::Value& app : *apps) {
      const base::DictValue& entry = app.GetDict();
      std::string title = "Pay with " + *entry.FindString("name");
      if (const std::string* origin = entry.FindString("origin")) {
        title += " (" + *origin + ")";
      }
      alert.buttons.push_back({*entry.FindString("id"), std::move(title)});
    }
    std::vector<std::string> keys;
    for (const AlertButton& button : alert.buttons) {
      keys.push_back(button.key);
    }
    dismiss_ = UiPresenters::Get().ShowAlert(
        "payments", &page_->host_view(), std::move(alert),
        base::BindOnce(&PaymentSheet::OnSheetAnswered,
                       weak_factory_.GetWeakPtr(), std::move(keys)));
  }

  void OnSheetAnswered(std::vector<std::string> keys, int index) {
    dismiss_.Reset();
    if (index > 0 && static_cast<size_t>(index) < keys.size()) {
      PayWith(keys[static_cast<size_t>(index)]);
      return;
    }
    Cancel();
  }

  void PayWith(const std::string& app_id) {
    if (!request_ || !request_->state()) {
      return;
    }
    payments::PaymentRequestState* state = request_->state().get();
    for (size_t i = 0; i < state->available_apps().size(); ++i) {
      if (app_id == "app" + base::NumberToString(i)) {
        state->SetSelectedApp(state->available_apps()[i]->AsWeakPtr());
        request_->Pay();
        return;
      }
    }
    Cancel();
  }

  void Cancel() {
    if (request_) {
      request_->OnUserCancelled();
    }
  }

  base::WeakPtr<payments::PaymentRequest> request_;
  base::WeakPtr<EngineContents> page_;
  std::shared_ptr<bdkcore::Request> request_ui_;
  Dismisser dismiss_;
  std::unique_ptr<HandlerPage> handler_watch_;
  base::WeakPtr<EngineContents> handler_page_;
  bool processing_ = false;
  bool closed_ = false;
  base::WeakPtrFactory<PaymentSheet> weak_factory_{this};
};

}  // namespace

void InstallPaymentHooks() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  // A page the person sees may ask to pay, not only the active tab of a
  // window of Chromium's (BDK's pages are background tabs of host contexts).
  embedder_payment_request_dialog::SetFollowsVisibility(true);
  embedder_payment_request_dialog::SetFactory(
      base::BindRepeating([](base::WeakPtr<payments::PaymentRequest> request)
                              -> base::WeakPtr<payments::PaymentRequestDialog> {
        return (new PaymentSheet(std::move(request)))->GetWeakPtr();
      }));
}

}  // namespace bdk
