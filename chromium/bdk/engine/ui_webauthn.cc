// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_webauthn.h"

#include <cctype>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/process/process_info.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/ui/views/webauthn/embedder_authenticator_dialog_factory.h"
#include "chrome/browser/ui/webauthn/authenticator_request_dialog_view_controller.h"
#include "chrome/browser/ui/webauthn/authenticator_request_sheet_model.h"
#include "chrome/browser/ui/webauthn/sheet_models.h"
#include "chrome/browser/webauthn/authenticator_request_dialog_model.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "device/fido/discoverable_credential_metadata.h"
#include "device/fido/public/fido_constants.h"
#include "device/fido/public/fido_transport_protocol.h"
#include "device/fido/public/fido_types.h"

namespace bdk {

namespace {

using Model = AuthenticatorRequestDialogModel;
using Step = Model::Step;
using SheetModel = AuthenticatorRequestSheetModel;

// What one step of the request is, for the application (ui-requests.md
// 5.18): the BDK step, an error's kind and, for the steps that need them,
// the PIN mode.
struct StepInfo {
  const char* step = "other";
  const char* error = nullptr;
  const char* pin_mode = nullptr;
  bool pre_user_verification = false;
};

StepInfo InfoOf(Step step) {
  switch (step) {
    case Step::kMechanismSelection:
    case Step::kSelectPriorityMechanism:
      return {.step = "chooseMechanism"};
    case Step::kUsbInsertAndActivate:
    case Step::kClientPinTapAgain:
      return {.step = "touchSecurityKey"};
    case Step::kClientPinEntry:
      return {.step = "enterPIN", .pin_mode = "enter"};
    case Step::kClientPinSetup:
      return {.step = "enterPIN", .pin_mode = "setup"};
    case Step::kClientPinChange:
      return {.step = "enterPIN", .pin_mode = "change"};
    case Step::kSelectAccount:
      return {.step = "chooseAccount"};
    case Step::kPreSelectAccount:
      return {.step = "chooseAccount", .pre_user_verification = true};
    case Step::kCableV2QRCode:
    case Step::kCableV2Connecting:
    case Step::kCableV2Connected:
    case Step::kBlePowerOnAutomatic:
    case Step::kBlePowerOnManual:
    case Step::kBlePermissionMac:
      return {.step = "connectPhone"};
    case Step::kOffTheRecordInterstitial:
    case Step::kResidentCredentialConfirmation:
    case Step::kRetryInternalUserVerification:
    case Step::kInlineBioEnrollment:
    case Step::kChromeProfileCreatePasskey:
      return {.step = "confirm"};
    case Step::kErrorNoAvailableTransports:
      return {.step = "error", .error = "noMechanisms"};
    case Step::kErrorNoPasskeys:
      return {.step = "error", .error = "noPasskeys"};
    case Step::kErrorInternalUnrecognized:
      return {.step = "error", .error = "unrecognized"};
    case Step::kTimedOut:
      return {.step = "error", .error = "timedOut"};
    case Step::kKeyNotRegistered:
      return {.step = "error", .error = "keyNotRegistered"};
    case Step::kKeyAlreadyRegistered:
      return {.step = "error", .error = "keyAlreadyRegistered"};
    case Step::kMissingCapability:
      return {.step = "error", .error = "missingCapability"};
    case Step::kStorageFull:
      return {.step = "error", .error = "storageFull"};
    case Step::kCableV2Error:
      return {.step = "error", .error = "phone"};
    case Step::kClientPinErrorSoftBlock:
      return {.step = "error", .error = "pinSoftBlocked"};
    case Step::kClientPinErrorHardBlock:
      return {.step = "error", .error = "pinBlocked"};
    case Step::kClientPinErrorAuthenticatorRemoved:
      return {.step = "error", .error = "keyRemoved"};
    case Step::kErrorGpmDisabled:
    case Step::kErrorWindowsHelloNotEnabled:
    case Step::kGPMError:
      return {.step = "error", .error = "other"};
    default:
      // Google Password Manager's steps (BDK has no Google sign-in, so they
      // are not expected) and those that show no dialog.
      return {};
  }
}

// Chromium's own sheet for the step: its wording, its buttons and what they
// do. The table follows CreateSheetViewForCurrentStepOf()
// (chrome/browser/ui/views/webauthn/sheet_view_factory.cc); a step it does
// not list has no sheet, and the request then carries no text.
std::unique_ptr<AuthenticatorSheetModelBase> SheetFor(Model* model) {
  using PinMode = AuthenticatorClientPinEntrySheetModel::Mode;
  switch (model->step()) {
    case Step::kMechanismSelection:
      if (model->ui_presentation == UIPresentation::kModalImmediate) {
        return std::make_unique<CombinedSelectorSheetModel>(model);
      }
      if (model->request_type == device::FidoRequestType::kGetAssertion) {
        return std::make_unique<AuthenticatorMultiSourcePickerSheetModel>(
            model);
      }
      return std::make_unique<AuthenticatorMechanismSelectorSheetModel>(model);
    case Step::kSelectPriorityMechanism:
      return std::make_unique<AuthenticatorPriorityMechanismSheetModel>(model);
    case Step::kUsbInsertAndActivate:
      return std::make_unique<AuthenticatorInsertAndActivateUsbSheetModel>(
          model);
    case Step::kClientPinTapAgain:
      return std::make_unique<AuthenticatorClientPinTapAgainSheetModel>(model);
    case Step::kClientPinEntry:
      return std::make_unique<AuthenticatorClientPinEntrySheetModel>(
          model, PinMode::kPinEntry, model->pin_error);
    case Step::kClientPinSetup:
      return std::make_unique<AuthenticatorClientPinEntrySheetModel>(
          model, PinMode::kPinSetup, model->pin_error);
    case Step::kClientPinChange:
      return std::make_unique<AuthenticatorClientPinEntrySheetModel>(
          model, PinMode::kPinChange, model->pin_error);
    case Step::kSelectAccount:
      return std::make_unique<AuthenticatorSelectAccountSheetModel>(
          model, AuthenticatorSelectAccountSheetModel::kPostUserVerification);
    case Step::kPreSelectAccount:
      return std::make_unique<AuthenticatorSelectAccountSheetModel>(
          model, AuthenticatorSelectAccountSheetModel::kPreUserVerification);
    case Step::kCableV2QRCode:
      return std::make_unique<AuthenticatorHybridAndSecurityKeySheetModel>(
          model);
    case Step::kCableV2Connecting:
      return std::make_unique<AuthenticatorConnectingSheetModel>(model);
    case Step::kCableV2Connected:
      return std::make_unique<AuthenticatorConnectedSheetModel>(model);
    case Step::kCableV2Error:
      return std::make_unique<AuthenticatorCableErrorSheetModel>(model);
    case Step::kBlePowerOnAutomatic:
      return std::make_unique<AuthenticatorBlePowerOnAutomaticSheetModel>(
          model);
    case Step::kBlePowerOnManual:
      return std::make_unique<AuthenticatorBlePowerOnManualSheetModel>(model);
    case Step::kBlePermissionMac:
      return std::make_unique<AuthenticatorBlePermissionMacSheetModel>(model);
    case Step::kOffTheRecordInterstitial:
      return std::make_unique<AuthenticatorOffTheRecordInterstitialSheetModel>(
          model);
    case Step::kResidentCredentialConfirmation:
      return std::make_unique<
          AuthenticatorResidentCredentialConfirmationSheetView>(model);
    case Step::kRetryInternalUserVerification:
      return std::make_unique<AuthenticatorRetryUvSheetModel>(model);
    case Step::kInlineBioEnrollment:
      return std::make_unique<AuthenticatorBioEnrollmentSheetModel>(model);
    case Step::kChromeProfileCreatePasskey:
      return std::make_unique<
          AuthenticatorChromeProfileCreatePasskeySheetModel>(model);
    case Step::kErrorNoAvailableTransports:
      return std::make_unique<AuthenticatorNoAvailableTransportsErrorModel>(
          model);
    case Step::kErrorNoPasskeys:
      return std::make_unique<AuthenticatorNoPasskeysErrorModel>(model);
    case Step::kErrorGpmDisabled:
      return std::make_unique<AuthenticatorGpmDisabledErrorModel>(model);
    case Step::kErrorInternalUnrecognized:
      return std::make_unique<AuthenticatorInternalUnrecognizedErrorSheetModel>(
          model);
    case Step::kTimedOut:
      return std::make_unique<AuthenticatorTimeoutErrorModel>(model);
    case Step::kKeyNotRegistered:
      return std::make_unique<AuthenticatorNotRegisteredErrorModel>(model);
    case Step::kKeyAlreadyRegistered:
      return std::make_unique<AuthenticatorAlreadyRegisteredErrorModel>(model);
    case Step::kMissingCapability:
      return AuthenticatorGenericErrorSheetModel::ForMissingCapability(model);
    case Step::kStorageFull:
      return AuthenticatorGenericErrorSheetModel::ForStorageFull(model);
    case Step::kClientPinErrorSoftBlock:
      return AuthenticatorGenericErrorSheetModel::ForClientPinErrorSoftBlock(
          model);
    case Step::kClientPinErrorHardBlock:
      return AuthenticatorGenericErrorSheetModel::ForClientPinErrorHardBlock(
          model);
    case Step::kClientPinErrorAuthenticatorRemoved:
      return AuthenticatorGenericErrorSheetModel::
          ForClientPinErrorAuthenticatorRemoved(model);
    default:
      return nullptr;
  }
}

// "kUsbInsertAndActivate" becomes "usbInsertAndActivate": Chromium's name of
// the step, for an application that wants to tell two steps of one kind
// apart.
std::string DetailOf(Step step) {
  std::ostringstream name;
  name << step;
  std::string text = name.str();
  if (text.size() > 1 && text[0] == 'k') {
    text = text.substr(1);
    text[0] = static_cast<char>(std::tolower(text[0]));
  }
  return text;
}

const char* KindOf(device::FidoTransportProtocol transport) {
  switch (transport) {
    case device::FidoTransportProtocol::kUsbHumanInterfaceDevice:
    case device::FidoTransportProtocol::kNearFieldCommunication:
    case device::FidoTransportProtocol::kBluetoothLowEnergy:
    case device::FidoTransportProtocol::kSmartCard:
      return "securityKey";
    case device::FidoTransportProtocol::kHybrid:
    case device::FidoTransportProtocol::kDeprecatedAoa:
      return "phone";
    case device::FidoTransportProtocol::kInternal:
      return "platform";
  }
  return "other";
}

// The kind of a mechanism: what the person would use.
const char* KindOf(const Model::Mechanism& mechanism) {
  return std::visit(
      [](const auto& type) -> const char* {
        using T = std::decay_t<decltype(type)>;
        if constexpr (std::is_same_v<T, Model::Mechanism::Credential>) {
          return "passkey";
        } else if constexpr (std::is_same_v<T, Model::Mechanism::Password>) {
          return "password";
        } else if constexpr (std::is_same_v<T, Model::Mechanism::Transport>) {
          return KindOf(type.value());
        } else if constexpr (std::is_same_v<T, Model::Mechanism::WindowsAPI>) {
          return "platform";
        } else if constexpr (std::is_same_v<T, Model::Mechanism::Hybrid>) {
          return "phone";
        } else if constexpr (std::is_same_v<T,
                                            Model::Mechanism::ICloudKeychain>) {
          return "iCloudKeychain";
        } else {
          return "other";  // Google Password Manager, signing in again
        }
      },
      mechanism.type);
}

void SetIfAny(base::DictValue& dict,
              const char* key,
              const std::u16string& value) {
  if (!value.empty()) {
    dict.Set(key, base::UTF16ToUTF8(value));
  }
}

// The payload of the step the model is at (ui-requests.md 5.18): the same
// facts and wording Chromium's dialog shows.
base::DictValue PayloadOf(Model* model,
                          SheetModel* sheet,
                          content::WebContents* web_contents,
                          int ceremony) {
  const StepInfo info = InfoOf(model->step());
  base::DictValue payload;
  payload.Set("ceremony", base::NumberToString(ceremony));
  payload.Set("relyingParty", model->relying_party_id);
  payload.Set("origin", web_contents->GetPrimaryMainFrame()
                            ->GetLastCommittedOrigin()
                            .Serialize());
  payload.Set("operation",
              model->request_type == device::FidoRequestType::kMakeCredential
                  ? "create"
                  : "get");
  payload.Set("step", info.step);
  payload.Set("detail", DetailOf(model->step()));
  if (info.error) {
    payload.Set("error", info.error);
  }
  base::DictValue text;
  if (sheet) {
    text.Set("title", base::UTF16ToUTF8(sheet->GetStepTitle()));
    text.Set("message", base::UTF16ToUTF8(sheet->GetStepDescription()));
    SetIfAny(text, "error", sheet->GetError());
    SetIfAny(text, "hint", sheet->GetHint());
    if (sheet->GetAcceptButtonState() ==
        SheetModel::AcceptButtonState::kEnabled) {
      payload.Set("acceptLabel",
                  base::UTF16ToUTF8(sheet->GetAcceptButtonLabel()));
    }
    if (sheet->IsCancelButtonVisible()) {
      payload.Set("cancelLabel",
                  base::UTF16ToUTF8(sheet->GetCancelButtonLabel()));
    }
    if (sheet->IsOtherMechanismButtonVisible()) {
      payload.Set("otherMechanismsLabel",
                  base::UTF16ToUTF8(sheet->GetOtherMechanismButtonLabel()));
    }
    payload.Set("busy", sheet->IsActivityIndicatorVisible());
  }
  payload.Set("text", std::move(text));

  if (std::string(info.step) == "chooseMechanism") {
    base::ListValue mechanisms;
    for (size_t i = 0; i < model->mechanisms.size(); ++i) {
      const Model::Mechanism& mechanism = model->mechanisms[i];
      base::DictValue entry;
      entry.Set("id", base::NumberToString(i));
      entry.Set("kind", KindOf(mechanism));
      entry.Set("name", base::UTF16ToUTF8(mechanism.name));
      SetIfAny(entry, "description", mechanism.description);
      mechanisms.Append(std::move(entry));
    }
    payload.Set("mechanisms", std::move(mechanisms));
  }
  if (std::string(info.step) == "chooseAccount") {
    base::ListValue accounts;
    for (size_t i = 0; i < model->creds.size(); ++i) {
      const device::DiscoverableCredentialMetadata& cred = model->creds[i];
      base::DictValue entry;
      entry.Set("id", base::NumberToString(i));
      entry.Set("name", cred.user.name.value_or(""));
      entry.Set("displayName", cred.user.display_name.value_or(""));
      accounts.Append(std::move(entry));
    }
    payload.Set("accounts", std::move(accounts));
  }
  if (info.pin_mode) {
    base::DictValue pin;
    pin.Set("mode", info.pin_mode);
    pin.Set("minLength", static_cast<int>(model->min_pin_length));
    if (model->pin_attempts) {
      pin.Set("attemptsRemaining", *model->pin_attempts);
    }
    payload.Set("pin", std::move(pin));
  }
  if (model->step() == Step::kCableV2QRCode && model->cable_qr_string) {
    payload.Set("qrCode", *model->cable_qr_string);
  }
  return payload;
}

// The index an id of the payload names, if it is one of `count`.
std::optional<size_t> IndexOf(const std::string* id, size_t count) {
  size_t index = 0;
  if (!id || !base::StringToSizeT(*id, &index) || index >= count) {
    return std::nullopt;
  }
  return index;
}

int g_next_ceremony = 1;

// BDK's dialog for one request: owned by the model, from the first step that
// shows a dialog until the model shows none (the request finished, or a
// platform authenticator took over).
class BdkAuthenticatorDialog : public AuthenticatorRequestDialogViewController,
                               public Model::Observer {
 public:
  BdkAuthenticatorDialog(content::WebContents* web_contents, Model* model)
      : model_(model), ceremony_(g_next_ceremony++) {
    if (EngineContents* page = EngineContents::FromWebContents(web_contents)) {
      page_ = page->GetWeakPtr();
    }
    observation_.Observe(model);
  }

  ~BdkAuthenticatorDialog() override { EndStep(); }

  // Model::Observer:
  void OnStepTransition() override {
    if (!model_ || model_->should_dialog_be_closed()) {
      return;
    }
    EndStep();
    sheet_ = SheetFor(model_);
    Raise();
  }

  void OnSheetModelChanged() override { RaiseIfChanged(); }
  void OnButtonsStateChanged() override { RaiseIfChanged(); }

  void OnModelDestroyed(Model* model) override {
    EndStep();
    sheet_.reset();
    observation_.Reset();
    model_ = nullptr;
  }

 private:
  // The step's request and default presenter go.
  void EndStep() {
    weak_factory_.InvalidateWeakPtrs();
    if (request_) {
      std::shared_ptr<bdkcore::Request> request = std::move(request_);
      bdkcore::withdraw_ui_request(*request);
    }
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
  }

  // A step whose facts changed while it was shown (a PIN error, the list of
  // mechanisms, the busy indicator) is a new request.
  void RaiseIfChanged() {
    // Nothing is shown before the first step transition.
    if (!model_ || model_->should_dialog_be_closed() || !page_ ||
        payload_.empty()) {
      return;
    }
    if (PayloadOf(model_, sheet_.get(), page_->web_contents(), ceremony_) ==
        payload_) {
      return;
    }
    EndStep();
    Raise();
  }

  void Raise() {
    if (!page_ || !page_->Core()) {
      Later(&BdkAuthenticatorDialog::Cancel);  // no page to ask in
      return;
    }
    payload_ =
        PayloadOf(model_, sheet_.get(), page_->web_contents(), ceremony_);
    request_ = RaiseUiRequest(*page_, BDK_SURFACE_WEBAUTHN, payload_,
                              base::BindOnce(&BdkAuthenticatorDialog::OnEnded,
                                             weak_factory_.GetWeakPtr()));
    if (!request_) {
      Later(&BdkAuthenticatorDialog::Cancel);
    }
  }

  void OnEnded(const UiResult& result) {
    request_.reset();
    switch (result.outcome) {
      case bdkcore::UiOutcome::kCompleted: {
        base::DictValue answer = result.args.Clone();
        answer.Set("completion", result.completion);
        Answer(std::move(answer));
        return;
      }
      case bdkcore::UiOutcome::kDefaulted:
        if (!page_) {
          Later(&BdkAuthenticatorDialog::Cancel);
          return;
        }
        dismiss_ = UiPresenters::Get().ShowWebAuthn(
            &page_->host_view(), payload_,
            base::BindOnce(&BdkAuthenticatorDialog::Answer,
                           weak_factory_.GetWeakPtr()));
        return;
      case bdkcore::UiOutcome::kWithdrawn:
        return;  // the model moved on, or this dialog is going
      default:
        // Rejected, dropped, disabled: never a choice made for the person.
        Later(&BdkAuthenticatorDialog::Cancel);
        return;
    }
  }

  // The answer of a handler or the default presenter, as a completion and
  // its arguments. It acts on the model from a task: the model may finish the
  // request, and destroy this dialog, in the call.
  void Answer(base::DictValue answer) {
    dismiss_.Reset();
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&BdkAuthenticatorDialog::Apply,
                       weak_factory_.GetWeakPtr(), std::move(answer)));
  }

  void Later(void (BdkAuthenticatorDialog::*action)()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(action, weak_factory_.GetWeakPtr()));
  }

  void Cancel() {
    if (model_) {
      model_->CancelAuthenticatorRequest();
    }
  }

  // Each branch ends with the call into the model, which may destroy `this`.
  void Apply(base::DictValue answer) {
    if (!model_) {
      return;
    }
    const std::string* completion = answer.FindString("completion");
    const std::string* action = answer.FindString("action");
    const std::string* pin = answer.FindString("pin");
    const StepInfo info = InfoOf(model_->step());
    const std::string step = info.step;
    if (completion && *completion == "grant" && step == "chooseAccount") {
      // The core let this through only with a gesture of this application.
      if (std::optional<size_t> index =
              IndexOf(answer.FindString("account"), model_->creds.size())) {
        if (info.pre_user_verification) {
          model_->OnAccountPreselectedIndex(*index);
        } else {
          model_->OnAccountSelected(*index);
        }
        return;
      }
    } else if (completion && *completion == "accept") {
      if (const std::string* id = answer.FindString("mechanism")) {
        if (std::optional<size_t> index =
                IndexOf(id, model_->mechanisms.size());
            index && step == "chooseMechanism") {
          model_->mechanisms[*index].callback.Run();
          return;
        }
      } else if (pin && info.pin_mode && sheet_) {
        auto* entry =
            static_cast<AuthenticatorClientPinEntrySheetModel*>(sheet_.get());
        entry->SetPinCode(base::UTF8ToUTF16(*pin));
        if (std::string(info.pin_mode) != "enter") {
          // A presenter that asked twice says what the second field held;
          // otherwise the application confirmed the new PIN itself.
          const std::string* confirmation =
              answer.FindString("pinConfirmation");
          entry->SetPinConfirmation(
              base::UTF8ToUTF16(confirmation ? *confirmation : *pin));
        }
        static_cast<SheetModel*>(entry)->OnAccept();
        return;
      } else if (action && *action == "otherMechanisms") {
        model_->StartOver();
        return;
      } else if (!action && sheet_ &&
                 static_cast<SheetModel*>(sheet_.get())
                         ->GetAcceptButtonState() ==
                     SheetModel::AcceptButtonState::kEnabled) {
        static_cast<SheetModel*>(sheet_.get())->OnAccept();
        return;
      }
    }
    // "deny", and anything that does not fit the step: the person did not
    // choose anything this step offers, so the request is cancelled.
    model_->CancelAuthenticatorRequest();
  }

  raw_ptr<Model> model_;  // Owns this.
  const int ceremony_;
  base::WeakPtr<EngineContents> page_;
  std::unique_ptr<AuthenticatorSheetModelBase> sheet_;
  base::DictValue payload_;
  std::shared_ptr<bdkcore::Request> request_;
  Dismisser dismiss_;
  base::ScopedObservation<Model, Model::Observer> observation_{this};
  base::WeakPtrFactory<BdkAuthenticatorDialog> weak_factory_{this};
};

}  // namespace

void InstallWebAuthnDialogHook() {
  // macOS ends a process that calls a Bluetooth API without Bluetooth metadata
  // in the responsible application's Info.plist. Chrome checks that before its
  // Bluetooth power manager, but not before starting the phone (hybrid)
  // discovery or answering whether hybrid is available, which ask only
  // IsLowEnergySupported(). Without the metadata, Bluetooth is reported as
  // unsupported: no WebAuthn path calls it, security keys over USB still work,
  // and passkeys on a phone are not offered (BDKWebAuthnSupport says what they
  // need).
  if (!base::DoesResponsibleProcessHaveBluetoothMetadata()) {
    static base::NoDestructor<
        std::unique_ptr<device::BluetoothAdapterFactory::GlobalOverrideValues>>
        no_bluetooth(
            device::BluetoothAdapterFactory::Get()->InitGlobalOverrideValues());
    (*no_bluetooth)->SetLESupported(false);
  }
  embedder_authenticator_dialog_factory::SetFactory(base::BindRepeating(
      [](content::WebContents* web_contents, Model* model)
          -> std::unique_ptr<AuthenticatorRequestDialogViewController> {
        return std::make_unique<BdkAuthenticatorDialog>(web_contents, model);
      }));
}

}  // namespace bdk
