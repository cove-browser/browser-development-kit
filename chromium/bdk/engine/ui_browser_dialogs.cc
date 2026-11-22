// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_browser_dialogs.h"

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
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/ui_presenters.h"
#include "chrome/browser/bluetooth/chrome_bluetooth_delegate.h"
#include "chrome/browser/bluetooth/chrome_bluetooth_delegate_impl_client.h"
#include "chrome/browser/enterprise/util/managed_browser_utils.h"
#include "chrome/browser/external_protocol/external_protocol_handler.h"
#include "chrome/browser/hid/chrome_hid_delegate.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/serial/chrome_serial_delegate.h"
#include "chrome/browser/ui/dialogs/embedder_tab_modal_dialog_hook.h"
#include "chrome/browser/ui/views/embedder_external_protocol_dialog_hook.h"
#include "chrome/browser/usb/chrome_usb_delegate.h"
#include "chrome/browser/web_applications/web_app_provider_factory.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/generated_resources.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/url_formatter/elide_url.h"
#include "content/public/browser/client_certificate_delegate.h"
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
void AskToOpenExternally(const GURL& url,
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

// Each chooser is refused as the person cancelling it would, so the page's
// request rejects (requestDevice: NotFoundError, HID: an empty list), and no
// chooser UI is made.
class BdkUsbDelegate : public ChromeUsbDelegate {
 public:
  std::unique_ptr<content::UsbChooser> RunChooser(
      content::RenderFrameHost& frame,
      blink::mojom::WebUsbRequestDeviceOptionsPtr options,
      blink::mojom::WebUsbService::GetPermissionCallback callback) override {
    std::move(callback).Run(nullptr);
    return nullptr;
  }
};

class BdkHidDelegate : public ChromeHidDelegate {
 public:
  std::unique_ptr<content::HidChooser> RunChooser(
      content::RenderFrameHost* render_frame_host,
      std::vector<blink::mojom::HidDeviceFilterPtr> filters,
      std::vector<blink::mojom::HidDeviceFilterPtr> exclusion_filters,
      content::HidChooser::Callback callback) override {
    std::move(callback).Run({});
    return nullptr;
  }
};

class BdkSerialDelegate : public ChromeSerialDelegate {
 public:
  std::unique_ptr<content::SerialChooser> RunChooser(
      content::RenderFrameHost* frame,
      std::vector<blink::mojom::SerialPortFilterPtr> filters,
      std::vector<device::BluetoothUUID> allowed_bluetooth_service_class_ids,
      content::SerialChooser::Callback callback) override {
    std::move(callback).Run(nullptr);
    return nullptr;
  }
};

// Web Bluetooth is also off at start (chromium_main_mac.mm); this keeps any
// path that still reaches a chooser, a scanning prompt or a pairing prompt
// from making views UI. No chooser is content's "not supported".
class BdkBluetoothClient : public ChromeBluetoothDelegateImplClient {
 public:
  std::unique_ptr<content::BluetoothChooser> RunBluetoothChooser(
      content::RenderFrameHost* frame,
      const content::BluetoothChooser::EventHandler& event_handler) override {
    return nullptr;
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
    // Installing the page as an application shows Chrome's install dialog
    // (tab-modal views), and BDK applications own their windows: a page
    // never offers itself for installation. Read once, when the profile's
    // web application system starts, so it is a default rather than a value
    // set after the profile loads.
    registry->SetDefaultPrefValue(prefs::kWebAppInstallByUserEnabled,
                                  base::Value(false));
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
  return std::make_unique<BdkSerialDelegate>();
}

std::unique_ptr<content::BluetoothDelegate> CreateBluetoothDelegate() {
  return std::make_unique<ChromeBluetoothDelegate>(
      std::make_unique<BdkBluetoothClient>());
}

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
  // A sheet can hold a few buttons, not a table: the first certificates,
  // each named by its subject and issuer.
  constexpr size_t kMaxShown = 8;
  if (others.size() > kMaxShown) {
    others.resize(kMaxShown);
  }
  Alert alert;
  alert.title = base::UTF16ToUTF8(
      l10n_util::GetStringUTF16(IDS_CLIENT_CERT_DIALOG_TITLE));
  alert.message = base::UTF16ToUTF8(l10n_util::GetStringFUTF16(
      IDS_CLIENT_CERT_DIALOG_TEXT,
      base::ASCIIToUTF16(cert_request_info->host_and_port.ToString())));
  for (size_t i = 0; i < others.size(); ++i) {
    const net::X509Certificate& cert = *others[i]->certificate();
    alert.buttons.push_back({"certificate" + base::NumberToString(i),
                             cert.subject().GetDisplayName() + " (" +
                                 cert.issuer().GetDisplayName() + ")"});
  }
  alert.buttons.push_back(
      {"cancel", base::UTF16ToUTF8(l10n_util::GetStringUTF16(IDS_APP_CANCEL))});
  auto identities =
      std::make_unique<net::ClientCertIdentityList>(std::move(others));
  Dismisser dismiss = UiPresenters::Get().ShowAlert(
      "client_certificates", view, std::move(alert),
      base::BindOnce(
          [](std::unique_ptr<net::ClientCertIdentityList> list,
             std::unique_ptr<content::ClientCertificateDelegate> chosen_by,
             int index) {
            if (index < 0) {
              return;  // nowhere to ask: dropping the delegate cancels
            }
            if (static_cast<size_t>(index) >= list->size()) {
              // Cancel, as in Chrome's selector: go on without one.
              chosen_by->ContinueWithCertificate(nullptr, nullptr);
              return;
            }
            std::unique_ptr<net::ClientCertIdentity> chosen =
                std::move((*list)[index]);
            scoped_refptr<net::X509Certificate> cert = chosen->certificate();
            net::ClientCertIdentity::SelfOwningAcquirePrivateKey(
                std::move(chosen),
                base::BindOnce(&content::ClientCertificateDelegate::
                                   ContinueWithCertificate,
                               std::move(chosen_by), std::move(cert)));
          },
          std::move(identities), std::move(delegate)));
  // Content runs this when the request goes away first (the page navigated).
  return dismiss;
}

}  // namespace bdk
