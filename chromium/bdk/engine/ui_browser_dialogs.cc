// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_browser_dialogs.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/chromium_main.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/ui_device_chooser.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "bdk/engine/web_apps.h"
#include "chrome/browser/bluetooth/chrome_bluetooth_delegate.h"
#include "chrome/browser/bluetooth/chrome_bluetooth_delegate_impl_client.h"
#include "chrome/browser/enterprise/util/managed_browser_utils.h"
#include "chrome/browser/external_protocol/external_protocol_handler.h"
#include "chrome/browser/hid/chrome_hid_delegate.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/serial/chrome_serial_delegate.h"
#include "chrome/browser/ui/bluetooth/chrome_bluetooth_chooser_controller.h"
#include "chrome/browser/ui/dialogs/embedder_tab_modal_dialog_hook.h"
#include "chrome/browser/ui/hid/hid_chooser.h"
#include "chrome/browser/ui/hid/hid_chooser_controller.h"
#include "chrome/browser/ui/serial/embedder_serial_bluetooth.h"  // nogncheck
#include "chrome/browser/ui/serial/serial_chooser_controller.h"
#include "chrome/browser/ui/views/embedder_external_protocol_dialog_hook.h"
#include "chrome/browser/usb/chrome_usb_delegate.h"
#include "chrome/browser/usb/usb_chooser_controller.h"
#include "chrome/browser/web_applications/web_app_provider_factory.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/generated_resources.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/permissions/bluetooth_chooser_desktop.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/url_formatter/elide_url.h"
#include "content/public/browser/client_certificate_delegate.h"
#include "content/public/browser/serial_chooser.h"
#include "content/public/browser/usb_chooser.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "net/cert/x509_certificate.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_private_key.h"
#include "services/device/public/mojom/hid.mojom.h"
#include "services/device/public/mojom/serial.mojom.h"
#include "services/device/public/mojom/usb_device.mojom.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/dialog_model.h"
#include "ui/base/models/dialog_model_field.h"
#include "ui/base/models/dialog_model_host.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/strings/grit/ui_strings.h"

namespace bdk {

namespace {

HostView* ViewOf(content::WebContents* contents) {
  EngineContents* page =
      contents ? EngineContents::FromWebContents(contents) : nullptr;
  return page ? &page->host_view() : nullptr;
}

// ---- External protocols -----------------------------------------------------

// The question ExternalProtocolDialog asks, with its wording. Cancel is the
// default, as in Chrome, so Return never opens another application; NSAlert
// gives Escape to it too.
void ShowOpenExternallySheet(
    const GURL& url,
    content::WebContents* contents,
    const std::optional<url::Origin>& initiating_origin,
    content::WeakDocumentPtr initiator_document,
    const std::u16string& program_name) {
  Alert alert;
  alert.title = base::UTF16ToUTF8(
      l10n_util::GetStringFUTF16(IDS_EXTERNAL_PROTOCOL_TITLE, program_name));
  alert.message = base::UTF16ToUTF8(
      initiating_origin && !initiating_origin->opaque()
          ? l10n_util::GetStringFUTF16(
                IDS_EXTERNAL_PROTOCOL_MESSAGE_WITH_INITIATING_ORIGIN,
                url_formatter::FormatOriginForSecurityDisplay(
                    *initiating_origin))
          : l10n_util::GetStringUTF16(IDS_EXTERNAL_PROTOCOL_MESSAGE));
  alert.buttons.push_back(
      {"cancel", base::UTF16ToUTF8(l10n_util::GetStringUTF16(
                     IDS_EXTERNAL_PROTOCOL_CANCEL_BUTTON_TEXT))});
  alert.buttons.push_back(
      {"open", base::UTF16ToUTF8(l10n_util::GetStringFUTF16(
                   IDS_EXTERNAL_PROTOCOL_OK_BUTTON_TEXT, program_name))});
  UiPresenters::Get().ShowAlert(
      "external_protocols", ViewOf(contents), std::move(alert),
      base::BindOnce(
          [](const GURL& launch_url, base::WeakPtr<content::WebContents> page,
             content::WeakDocumentPtr document, int index) {
            const bool open = index == 1;
            ExternalProtocolHandler::RecordHandleStateMetrics(
                /*checkbox_selected=*/false,
                open ? ExternalProtocolHandler::DONT_BLOCK
                     : ExternalProtocolHandler::BLOCK);
            if (open && page) {
              ExternalProtocolHandler::LaunchUrlWithoutSecurityCheck(
                  launch_url, page.get(), std::move(document));
            }
          },
          url, contents->GetWeakPtr(), std::move(initiator_document)));
}

// Chrome decided to ask (T-UIR-13): an external_protocols request
// (ui-requests.md 5.17). A grant opens the application, and with "remember"
// opens this scheme from this origin without asking again, as Chrome's
// checkbox does; it needs the confirming gesture, which the router checks. A
// denial, a rejection or a withdrawn request opens nothing; `disabled` blocks
// the scheme; with no handler the sheet above asks.
void AskToOpenExternally(const GURL& url,
                         content::WebContents* contents,
                         const std::optional<url::Origin>& initiating_origin,
                         content::WeakDocumentPtr initiator_document,
                         const std::u16string& program_name) {
  EngineContents* page =
      contents ? EngineContents::FromWebContents(contents) : nullptr;
  if (!page) {
    return;
  }
  base::DictValue payload;
  payload.Set("url", url.spec());
  if (initiating_origin && !initiating_origin->opaque()) {
    payload.Set("initiatingOrigin", initiating_origin->Serialize());
  }
  if (!program_name.empty()) {
    payload.Set("handlerName", base::UTF16ToUTF8(program_name));
  }
  // Chrome asks only for a navigation it is about to launch; whether a
  // gesture started it is not passed to the dialog.
  payload.Set("isUserGesture", false);
  struct Ask {
    GURL url;
    base::WeakPtr<content::WebContents> page;
    std::optional<url::Origin> origin;
    content::WeakDocumentPtr document;
    std::u16string program;
  };
  auto ask =
      std::make_unique<Ask>(Ask{url, contents->GetWeakPtr(), initiating_origin,
                                std::move(initiator_document), program_name});
  RaiseUiRequest(
      *page, BDK_SURFACE_EXTERNAL_PROTOCOLS, payload,
      base::BindOnce(
          [](std::unique_ptr<Ask> ask, const UiResult& result) {
            content::WebContents* contents = ask->page.get();
            if (!contents) {
              return;
            }
            if (result.outcome == bdkcore::UiOutcome::kDefaulted) {
              ShowOpenExternallySheet(ask->url, contents, ask->origin,
                                      std::move(ask->document), ask->program);
              return;
            }
            const bool open =
                result.outcome == bdkcore::UiOutcome::kCompleted &&
                result.completion == "grant";
            ExternalProtocolHandler::RecordHandleStateMetrics(
                result.args.FindBool("remember").value_or(false),
                open ? ExternalProtocolHandler::DONT_BLOCK
                     : ExternalProtocolHandler::BLOCK);
            if (!open) {
              return;
            }
            if (result.args.FindBool("remember").value_or(false) &&
                ask->origin && !ask->origin->opaque()) {
              ExternalProtocolHandler::SetBlockState(
                  ask->url.GetScheme(), *ask->origin,
                  ExternalProtocolHandler::DONT_BLOCK,
                  Profile::FromBrowserContext(contents->GetBrowserContext()));
            }
            ExternalProtocolHandler::LaunchUrlWithoutSecurityCheck(
                ask->url, contents, std::move(ask->document));
          },
          std::move(ask)));
}

// ---- Tab-modal dialog models ------------------------------------------------

std::u16string LabelText(const ui::DialogModelLabel& label) {
  if (label.replacements().empty()) {
    return label.GetString();
  }
  std::vector<std::u16string> parts;
  for (const ui::DialogModelLabel::TextReplacement& part :
       label.replacements()) {
    parts.push_back(part.text());
  }
  return l10n_util::GetStringFUTF16(label.message_id(), parts, nullptr);
}

// Hosts one ui::DialogModel as a sheet on the page's window. The model's
// title and paragraphs are the text; its OK and Cancel buttons are the
// buttons, with the model's default first; a checkbox keeps its initial state.
// Answers go to the model's own callbacks. Owns itself and the model until
// the model is closed.
class SheetDialogModelHost : public ui::DialogModelHost,
                             public content::WebContentsObserver {
 public:
  static void Show(std::unique_ptr<ui::DialogModel> model,
                   content::WebContents* contents) {
    (new SheetDialogModelHost(std::move(model), contents))->Present();
  }

  SheetDialogModelHost(const SheetDialogModelHost&) = delete;
  SheetDialogModelHost& operator=(const SheetDialogModelHost&) = delete;
  // Deleted by Finish(), from a task.
  ~SheetDialogModelHost() override = default;

 private:
  enum class Action { kAccept, kCancel, kClose };

  SheetDialogModelHost(std::unique_ptr<ui::DialogModel> model,
                       content::WebContents* contents)
      : content::WebContentsObserver(contents), model_(std::move(model)) {
    model_->set_host(GetPassKey(), this);
  }

  // A field a sheet cannot show: the dialog would ask for something the person
  // cannot give.
  bool HasUnsupportedField() {
    for (const std::unique_ptr<ui::DialogModelField>& field :
         model_->fields(GetPassKey())) {
      if (!field->is_visible()) {
        continue;
      }
      switch (field->type()) {
        case ui::DialogModelField::kParagraph:
        case ui::DialogModelField::kCheckbox:
        case ui::DialogModelField::kSeparator:
        case ui::DialogModelField::kTitleItem:
        // Views-only content (a bulleted list); the paragraphs carry the
        // question.
        case ui::DialogModelField::kCustom:
          break;
        default:
          return true;
      }
    }
    return false;
  }

  void Present() {
    if (HasUnsupportedField()) {
      LOG(WARNING) << "a tab-modal dialog ("
                   << model_->internal_name(GetPassKey())
                   << ") has a field a sheet cannot show; it is closed";
      Answer(Action::kClose);
      return;
    }
    Alert alert;
    alert.title = base::UTF16ToUTF8(model_->title(GetPassKey()));
    std::u16string message;
    for (const std::unique_ptr<ui::DialogModelField>& field :
         model_->fields(GetPassKey())) {
      if (!field->is_visible() ||
          field->type() != ui::DialogModelField::kParagraph) {
        continue;
      }
      ui::DialogModelParagraph* paragraph = field->AsParagraph();
      for (const std::u16string& text :
           {paragraph->header(), LabelText(paragraph->label())}) {
        if (!text.empty()) {
          message += (message.empty() ? u"" : u"\n\n") + text;
        }
      }
    }
    alert.message = base::UTF16ToUTF8(message);
    actions_.clear();
    auto add = [&](ui::DialogModel::Button* button, const char* key,
                   int default_label, Action action) {
      if (!button) {
        return;
      }
      const std::u16string& label = button->label();
      alert.buttons.push_back(
          {key, base::UTF16ToUTF8(label.empty()
                                      ? l10n_util::GetStringUTF16(default_label)
                                      : label)});
      actions_.push_back(action);
    };
    const bool cancel_first = model_->override_default_button(GetPassKey()) ==
                              ui::mojom::DialogButton::kCancel;
    ui::DialogModel::Button* ok = model_->ok_button(GetPassKey());
    ui::DialogModel::Button* cancel = model_->cancel_button(GetPassKey());
    if (cancel_first) {
      add(cancel, "cancel", IDS_APP_CANCEL, Action::kCancel);
    }
    add(ok, "accept", IDS_APP_OK, Action::kAccept);
    if (!cancel_first) {
      add(cancel, "cancel", IDS_APP_CANCEL, Action::kCancel);
    }
    if (alert.buttons.empty()) {
      alert.buttons.push_back(
          {"close", base::UTF16ToUTF8(l10n_util::GetStringUTF16(IDS_APP_OK))});
      actions_.push_back(Action::kClose);
    }
    dismiss_ = UiPresenters::Get().ShowAlert(
        "browser_dialogs", ViewOf(web_contents()), std::move(alert),
        base::BindOnce(&SheetDialogModelHost::OnAnswer,
                       weak_factory_.GetWeakPtr()));
  }

  void OnAnswer(int index) {
    dismiss_.Reset();
    Answer(index >= 0 && static_cast<size_t>(index) < actions_.size()
               ? actions_[index]
               : Action::kClose);
  }

  void Answer(Action action) {
    bool close = true;
    // A callback may close the dialog through Close(); the model is not
    // destroyed while its own callback runs.
    in_action_ = true;
    switch (action) {
      case Action::kAccept:
        close = model_->OnDialogAcceptAction(GetPassKey());
        break;
      case Action::kCancel:
        close = model_->OnDialogCancelAction(GetPassKey());
        break;
      case Action::kClose:
        model_->OnDialogCloseAction(GetPassKey());
        break;
    }
    in_action_ = false;
    if (finished_) {
      return;
    }
    close || close_requested_ ? Finish() : Present();
  }

  // Destroys the model (its destroying callback runs) and, later, this.
  void Finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
    weak_factory_.InvalidateWeakPtrs();
    model_->OnDialogDestroying(GetPassKey());
    model_.reset();
    Observe(nullptr);
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  // ui::DialogModelHost:
  void Close() override {
    if (in_action_) {
      close_requested_ = true;
      return;
    }
    Finish();
  }
  void OnDialogButtonChanged() override {}

  // content::WebContentsObserver:
  void WebContentsDestroyed() override { Finish(); }

  std::unique_ptr<ui::DialogModel> model_;
  std::vector<Action> actions_;
  Dismisser dismiss_;
  bool finished_ = false;
  bool in_action_ = false;
  bool close_requested_ = false;
  base::WeakPtrFactory<SheetDialogModelHost> weak_factory_{this};
};

std::unique_ptr<ui::DialogModel> ShowTabModalAsSheet(
    std::unique_ptr<ui::DialogModel> model,
    content::WebContents* contents) {
  SheetDialogModelHost::Show(std::move(model), contents);
  return nullptr;
}

// ---- Device choosers --------------------------------------------------------

// Each chooser is Chrome's controller shown as a device_choosers request
// (ui_device_chooser.h) instead of Chrome's views dialog. The chooser objects
// content keeps only hold the closure that takes the request down.
class ClosingUsbChooser : public content::UsbChooser {
 public:
  explicit ClosingUsbChooser(base::OnceClosure close)
      : close_(std::move(close)) {}

 private:
  base::ScopedClosureRunner close_;
};

class ClosingSerialChooser : public content::SerialChooser {
 public:
  explicit ClosingSerialChooser(base::OnceClosure close)
      : close_(std::move(close)) {}

 private:
  base::ScopedClosureRunner close_;
};

class BdkUsbDelegate : public ChromeUsbDelegate {
 public:
  std::unique_ptr<content::UsbChooser> RunChooser(
      content::RenderFrameHost& frame,
      blink::mojom::WebUsbRequestDeviceOptionsPtr options,
      blink::mojom::WebUsbService::GetPermissionCallback callback) override {
    return std::make_unique<ClosingUsbChooser>(ShowDeviceChooser(
        "usb", &frame,
        std::make_unique<UsbChooserController>(&frame, std::move(options),
                                               std::move(callback))));
  }
};

class BdkHidDelegate : public ChromeHidDelegate {
 public:
  std::unique_ptr<content::HidChooser> RunChooser(
      content::RenderFrameHost* render_frame_host,
      std::vector<blink::mojom::HidDeviceFilterPtr> filters,
      std::vector<blink::mojom::HidDeviceFilterPtr> exclusion_filters,
      content::HidChooser::Callback callback) override {
    return std::make_unique<HidChooser>(ShowDeviceChooser(
        "hid", render_frame_host,
        std::make_unique<HidChooserController>(
            render_frame_host, std::move(filters), std::move(exclusion_filters),
            std::move(callback))));
  }
};

class BdkSerialDelegate : public ChromeSerialDelegate {
 public:
  std::unique_ptr<content::SerialChooser> RunChooser(
      content::RenderFrameHost* frame,
      std::vector<blink::mojom::SerialPortFilterPtr> filters,
      std::vector<device::BluetoothUUID> allowed_bluetooth_service_class_ids,
      content::SerialChooser::Callback callback) override {
    return std::make_unique<ClosingSerialChooser>(
        ShowDeviceChooser("serial", frame,
                          std::make_unique<SerialChooserController>(
                              frame, std::move(filters),
                              std::move(allowed_bluetooth_service_class_ids),
                              std::move(callback))));
  }
};

// Web Bluetooth is on only when the application declares why it uses
// Bluetooth (chromium_main_mac.mm). The chooser is a request; the scanning
// prompt (requestLEScan, behind Chromium's experimental features) and the
// pairing prompts are refused, so no views UI is made.
class BdkBluetoothClient : public ChromeBluetoothDelegateImplClient {
 public:
  std::unique_ptr<content::BluetoothChooser> RunBluetoothChooser(
      content::RenderFrameHost* frame,
      const content::BluetoothChooser::EventHandler& event_handler) override {
    return std::make_unique<permissions::BluetoothChooserDesktop>(
        std::make_unique<ChromeBluetoothChooserController>(frame,
                                                           event_handler),
        base::BindOnce(&ShowDeviceChooser, "bluetooth", frame));
  }
  std::unique_ptr<content::BluetoothScanningPrompt> ShowBluetoothScanningPrompt(
      content::RenderFrameHost* frame,
      const content::BluetoothScanningPrompt::EventHandler& event_handler)
      override {
    return nullptr;
  }
  void ShowBluetoothDevicePairDialog(
      content::RenderFrameHost* frame,
      const std::u16string& device_identifier,
      content::BluetoothDelegate::PairPromptCallback callback,
      content::BluetoothDelegate::PairingKind pairing_kind,
      const std::optional<std::u16string>& pin) override {
    std::move(callback).Run(content::BluetoothDelegate::PairPromptResult(
        content::BluetoothDelegate::PairPromptStatus::kCancelled));
  }
};

// ---- Preference defaults ----------------------------------------------------

// A factory with no service: its only job is the preference defaults below,
// registered after the preferences' own factories register them.
class BrowserDialogPrefDefaults : public BrowserContextKeyedServiceFactory {
 public:
  static BrowserDialogPrefDefaults* GetInstance() {
    static base::NoDestructor<BrowserDialogPrefDefaults> instance;
    return instance.get();
  }

  BrowserDialogPrefDefaults()
      : BrowserContextKeyedServiceFactory(
            "BdkBrowserDialogPrefDefaults",
            BrowserContextDependencyManager::GetInstance()) {
    DependsOn(web_app::WebAppProviderFactory::GetInstance());
  }

 private:
  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override {
    // A page offers itself for installation only when [web_apps] enabled
    // is set (web-apps.md 10.5, T-PWA-04): its prompt is then a
    // web_app_installs request. Read once, when the profile's web application
    // system starts, so it is a default rather than a value set after the
    // profile loads.
    registry->SetDefaultPrefValue(prefs::kWebAppInstallByUserEnabled,
                                  base::Value(WebAppsEnabled()));
  }

  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override {
    return nullptr;
  }
};

}  // namespace

void InstallBrowserDialogHooks() {
  embedder_external_protocol_dialog_hook::SetHook(
      base::BindRepeating(&AskToOpenExternally));
  embedder_tab_modal_dialog_hook::SetHook(
      base::BindRepeating(&ShowTabModalAsSheet));
}

void InstallBrowserDialogPrefDefaults() {
  BrowserDialogPrefDefaults::GetInstance();
}

std::unique_ptr<content::UsbDelegate> CreateUsbDelegate() {
  return std::make_unique<BdkUsbDelegate>();
}

std::unique_ptr<content::HidDelegate> CreateHidDelegate() {
  return std::make_unique<BdkHidDelegate>();
}

std::unique_ptr<content::SerialDelegate> CreateSerialDelegate() {
  // The chooser would open the Bluetooth adapter to list Bluetooth ports
  // (patch bdk-let-an-embedder-keep-the-serial-chooser-from-asking-for-
  // bluetooth).
  embedder_serial_bluetooth::SetMayAskForBluetooth(
      ApplicationDeclaresBluetoothUse());
  return std::make_unique<BdkSerialDelegate>();
}

std::unique_ptr<content::BluetoothDelegate> CreateBluetoothDelegate() {
  return std::make_unique<ChromeBluetoothDelegate>(
      std::make_unique<BdkBluetoothClient>());
}

namespace {

// One client-certificate question: the certificates offered, and the delegate
// the answer goes to, once.
struct CertificateSelection {
  CertificateSelection(std::string host,
                       net::ClientCertIdentityList identities,
                       std::unique_ptr<content::ClientCertificateDelegate> d)
      : host(std::move(host)),
        identities(std::move(identities)),
        delegate(std::move(d)) {}

  // Continues with the certificate at `index`, or without one when it is out
  // of range.
  void Choose(size_t index) {
    if (!delegate) {
      return;
    }
    std::unique_ptr<content::ClientCertificateDelegate> chosen_by =
        std::move(delegate);
    if (index >= identities.size()) {
      chosen_by->ContinueWithCertificate(nullptr, nullptr);
      return;
    }
    std::unique_ptr<net::ClientCertIdentity> chosen =
        std::move(identities[index]);
    scoped_refptr<net::X509Certificate> cert = chosen->certificate();
    net::ClientCertIdentity::SelfOwningAcquirePrivateKey(
        std::move(chosen),
        base::BindOnce(
            &content::ClientCertificateDelegate::ContinueWithCertificate,
            std::move(chosen_by), std::move(cert)));
  }

  // The request went away: what is open goes, and dropping the delegate
  // cancels it.
  void Cancel() {
    if (request) {
      bdkcore::withdraw_ui_request(*request);
      request.reset();
    }
    if (dismiss) {
      std::move(dismiss).Run();
    }
    delegate.reset();
  }

  // A sheet can hold a few buttons, not a table: the first certificates, each
  // named by its subject and issuer.
  void ShowSheet(HostView* view) {
    constexpr size_t kMaxShown = 8;
    const size_t shown = std::min(identities.size(), kMaxShown);
    Alert alert;
    alert.title = base::UTF16ToUTF8(
        l10n_util::GetStringUTF16(IDS_CLIENT_CERT_DIALOG_TITLE));
    alert.message = base::UTF16ToUTF8(l10n_util::GetStringFUTF16(
        IDS_CLIENT_CERT_DIALOG_TEXT, base::UTF8ToUTF16(host)));
    for (size_t i = 0; i < shown; ++i) {
      const net::X509Certificate& cert = *identities[i]->certificate();
      alert.buttons.push_back({"certificate" + base::NumberToString(i),
                               cert.subject().GetDisplayName() + " (" +
                                   cert.issuer().GetDisplayName() + ")"});
    }
    alert.buttons.push_back(
        {"cancel",
         base::UTF16ToUTF8(l10n_util::GetStringUTF16(IDS_APP_CANCEL))});
    dismiss = UiPresenters::Get().ShowAlert(
        "client_certificates", view, std::move(alert),
        base::BindOnce(
            [](base::WeakPtr<CertificateSelection> self, size_t shown,
               int index) {
              if (!self || index < 0) {
                return;  // nowhere to ask: dropping the delegate cancels
              }
              self->dismiss.Reset();
              // Cancel, as in Chrome's selector: go on without one.
              self->Choose(static_cast<size_t>(index) < shown
                               ? static_cast<size_t>(index)
                               : self->identities.size());
            },
            weak_factory.GetWeakPtr(), shown));
  }

  std::string host;
  net::ClientCertIdentityList identities;
  std::unique_ptr<content::ClientCertificateDelegate> delegate;
  std::shared_ptr<bdkcore::Request> request;
  Dismisser dismiss;
  base::WeakPtrFactory<CertificateSelection> weak_factory{this};
};

}  // namespace

base::OnceClosure SelectClientCertificate(
    content::BrowserContext* browser_context,
    content::WebContents* web_contents,
    net::SSLCertRequestInfo* cert_request_info,
    net::ClientCertIdentityList client_certs,
    std::unique_ptr<content::ClientCertificateDelegate> delegate) {
  Profile* profile = Profile::FromBrowserContext(browser_context);
  net::ClientCertIdentityList matching, others;
  enterprise_util::AutoSelectCertificates(
      profile,
      enterprise_util::GetRequestingUrl(cert_request_info->host_and_port),
      std::move(client_certs), &matching, &others);
  // Chrome's automatic selection (the AutoSelectCertificateForUrls setting):
  // the first match, with no question.
  if (!matching.empty()) {
    std::unique_ptr<net::ClientCertIdentity> identity = std::move(matching[0]);
    scoped_refptr<net::X509Certificate> cert = identity->certificate();
    net::ClientCertIdentity::SelfOwningAcquirePrivateKey(
        std::move(identity),
        base::BindOnce(
            &content::ClientCertificateDelegate::ContinueWithCertificate,
            std::move(delegate), std::move(cert)));
    return base::OnceClosure();
  }
  // No page to ask on (a service worker): the request goes on without one.
  HostView* view = ViewOf(web_contents);
  if (!view || others.empty()) {
    delegate->ContinueWithCertificate(nullptr, nullptr);
    return base::OnceClosure();
  }
  // A client_certificates request (ui-requests.md 5.3, T-UIR-13): the
  // certificates by an index, subject and issuer. Choosing one continues with
  // it; a denial, a rejection or `disabled` goes on without one, as Chrome's
  // selector does on Cancel; with no handler the sheet asks.
  auto selection = std::make_shared<CertificateSelection>(
      cert_request_info->host_and_port.ToString(), std::move(others),
      std::move(delegate));
  EngineContents* page = EngineContents::FromWebContents(web_contents);
  base::DictValue payload;
  payload.Set("host", selection->host);
  base::ListValue certificates;
  for (size_t i = 0; i < selection->identities.size(); ++i) {
    const net::X509Certificate& cert = *selection->identities[i]->certificate();
    certificates.Append(base::DictValue()
                            .Set("id", base::NumberToString(i))
                            .Set("subject", cert.subject().GetDisplayName())
                            .Set("issuer", cert.issuer().GetDisplayName()));
  }
  payload.Set("certificates", std::move(certificates));
  selection->request = RaiseUiRequest(
      *page, BDK_SURFACE_CLIENT_CERTIFICATES, payload,
      base::BindOnce(
          [](std::shared_ptr<CertificateSelection> selection, HostView* view,
             const UiResult& result) {
            if (result.outcome == bdkcore::UiOutcome::kWithdrawn) {
              return;  // the page went first; content cancelled it
            }
            if (result.outcome == bdkcore::UiOutcome::kDefaulted) {
              selection->ShowSheet(view);
              return;
            }
            size_t index = selection->identities.size();
            if (result.outcome == bdkcore::UiOutcome::kCompleted &&
                result.completion == "accept") {
              if (const std::string* id = result.args.FindString("certificate");
                  !id || !base::StringToSizeT(*id, &index)) {
                index = selection->identities.size();
              }
            }
            selection->Choose(index);
          },
          selection, view));
  if (!selection->request) {
    selection->Choose(selection->identities.size());
    return base::OnceClosure();
  }
  // Content runs this when the request goes away first (the page navigated).
  return base::BindOnce(
      [](std::shared_ptr<CertificateSelection> selection) {
        selection->Cancel();
      },
      selection);
}

}  // namespace bdk
