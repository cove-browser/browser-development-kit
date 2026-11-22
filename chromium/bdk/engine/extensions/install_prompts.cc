// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/install_prompts.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_info.h"
#include "bdk/engine/extensions/extension_support.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/extensions/extension_install_prompt_show_params.h"
#include "chrome/browser/extensions/extension_uninstall_dialog.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/extensions/embedder_extension_dialogs.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/disable_reason.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/install/crx_install_error.h"
#include "extensions/browser/install_prompt_permissions.h"
#include "extensions/browser/uninstall_reason.h"
#include "extensions/common/extension.h"
#include "extensions/common/permissions/permission_set.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/native_ui_types.h"

namespace bdk {

namespace {

using extensions::InstallPromptData;
using Result = ExtensionInstallPrompt::Result;
using DonePtr = std::shared_ptr<ExtensionInstallPrompt::DoneCallback>;

constexpr char kInstallsSurface[] = "extension_installs";
constexpr char kUninstallsSurface[] = "extension_uninstalls";

std::string Utf8(const std::u16string& text) {
  return base::UTF16ToUTF8(text);
}

// The page a prompt belongs to: the preferred one when it is a page of the
// profile, else a page of the profile. A prompt that no page can carry is
// refused; nothing is installed or removed without being asked.
EngineContents* PageForPrompt(Profile* profile, EngineContents* preferred) {
  Profile* original = profile->GetOriginalProfile();
  auto belongs = [original](EngineContents* page) {
    std::shared_ptr<bdkcore::Contents> core = page->Core();
    return core && core->profile && core->profile->data &&
           ProfileOf(*core)->GetOriginalProfile() == original;
  };
  if (preferred && belongs(preferred)) {
    return preferred;
  }
  for (EngineContents* page : EngineContents::All()) {
    if (belongs(page)) {
      return page;
    }
  }
  return nullptr;
}

const char* KindOf(Profile* profile, const InstallPromptData& data) {
  switch (data.type()) {
    case InstallPromptData::PERMISSIONS_PROMPT:
      return "optionalPermissions";
    case InstallPromptData::RE_ENABLE_PROMPT:
    case InstallPromptData::REMOTE_INSTALL_PROMPT:
      return data.extension() &&
                     extensions::ExtensionPrefs::Get(profile)->HasDisableReason(
                         data.extension()->id(),
                         extensions::disable_reason::
                             DISABLE_PERMISSIONS_INCREASE)
                 ? "permissionsIncrease"
                 : "reEnable";
    default:
      return "install";
  }
}

// What the default presenter and a test script see: every warning string, the
// host warnings under them and the compatibility findings.
Alert BuildAlert(const std::string& kind,
                 const std::string& name,
                 const std::vector<std::string>& warnings,
                 const std::vector<std::string>& details,
                 const bdkcore::ext::Report& report) {
  Alert alert;
  std::string accept = "Add";
  if (kind == "install") {
    alert.title = "Add \"" + name + "\"?";
  } else if (kind == "reEnable") {
    alert.title = "Enable \"" + name + "\"?";
    accept = "Enable";
  } else if (kind == "permissionsIncrease") {
    alert.title = "\"" + name + "\" needs new permissions";
    accept = "Allow and enable";
  } else {
    alert.title = "\"" + name + "\" asks for more permissions";
    accept = "Allow";
  }
  std::string message =
      warnings.empty() ? "It asks for no special permissions." : "It can:";
  for (const std::string& warning : warnings) {
    message += "\n- " + warning;
  }
  for (const std::string& detail : details) {
    message += "\n    - " + detail;
  }
  if (!report.findings.empty()) {
    message += "\n\nCompatibility (" + report.verdict() + "):";
    for (const bdkcore::ext::Finding& finding : report.findings) {
      message += "\n- " + finding.message;
    }
  }
  alert.message = message;
  alert.buttons.push_back({"install", accept});
  alert.buttons.push_back({"cancel", "Cancel"});
  return alert;
}

// Ends the prompt: the result goes to Chromium once, and the prompt's own
// observers hear how it ended.
void Finish(const DonePtr& done,
            const std::shared_ptr<InstallPromptData>& data,
            const std::shared_ptr<PromptTrace>& trace,
            Result result) {
  if (!done || !*done) {
    return;
  }
  if (result == Result::ACCEPTED) {
    data->OnDialogAccepted();
  } else {
    data->OnDialogCanceled();
    if (trace && !trace->dry_run) {
      trace->declined = true;
    }
  }
  // Later, so that a refusal made while Chromium is still inside the prompt's
  // own call does not delete the prompt under it.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](ExtensionInstallPrompt::DoneCallback done, Result result) {
            std::move(done).Run(
                ExtensionInstallPrompt::DoneCallbackPayload(result));
          },
          std::move(*done), result));
}

void ShowInstallRequest(Profile* profile,
                        InstallPromptContext context,
                        std::unique_ptr<InstallPromptData> prompt_data,
                        ExtensionInstallPrompt::DoneCallback done_callback) {
  auto done = std::make_shared<ExtensionInstallPrompt::DoneCallback>(
      std::move(done_callback));
  std::shared_ptr<InstallPromptData> data = std::move(prompt_data);
  const extensions::Extension* extension = data->extension();
  std::shared_ptr<bdkcore::Runtime> runtime = CurrentRuntime();
  if (!extension || !runtime) {
    Finish(done, data, context.trace, Result::ABORTED);
    return;
  }
  const bdkcore::ext::Report report = ReportOf(*runtime, *extension);
  if (context.trace) {
    context.trace->report_json = report.json();
    if (context.trace->dry_run) {
      Finish(done, data, context.trace, Result::ABORTED);
      return;
    }
  }
  EngineContents* page = PageForPrompt(profile, context.page.get());
  if (!page) {
    // No page of the profile to carry a request or a sheet: nothing may be
    // installed unasked.
    Finish(done, data, context.trace, Result::USER_CANCELED);
    return;
  }

  const std::string kind = KindOf(profile, *data);
  extensions::InstallPromptPermissions permissions = data->GetPermissions();
  std::vector<std::string> warnings;
  for (const std::u16string& warning : permissions.permissions) {
    warnings.push_back(Utf8(warning));
  }
  std::vector<std::string> host_warnings;
  for (const std::u16string& detail : permissions.details) {
    host_warnings.push_back(Utf8(detail));
  }
  base::ListValue warning_list;
  for (const std::string& warning : warnings) {
    warning_list.Append(warning);
  }
  base::ListValue host_list;
  for (const std::string& detail : host_warnings) {
    host_list.Append(detail);
  }
  base::DictValue payload;
  payload.Set("kind", kind);
  payload.Set("extensionId", extension->id());
  payload.Set("extensionName", extension->name());
  if (std::string icon = IconDataUrl(data->icon()); !icon.empty()) {
    payload.Set("icon", icon);
  }
  payload.Set("source", context.source.empty()
                            ? SourceKindOf(profile, *extension)
                            : context.source);
  payload.Set("warnings", std::move(warning_list));
  payload.Set("hostWarnings", std::move(host_list));
  if (std::optional<base::DictValue> compatibility =
          ParseJsonDict(report.json())) {
    payload.Set("compatibility", std::move(*compatibility));
  }
  if (context.trace) {
    context.trace->asked = true;
  }

  const std::string name = extension->name();
  base::WeakPtr<EngineContents> weak_page = page->GetWeakPtr();
  std::shared_ptr<PromptTrace> trace = context.trace;
  RaiseUiRequest(
      *page, BDK_SURFACE_EXTENSION_INSTALLS, payload,
      base::BindOnce(
          [](DonePtr done, std::shared_ptr<InstallPromptData> data,
             std::shared_ptr<PromptTrace> trace,
             base::WeakPtr<EngineContents> page, std::string kind,
             std::string name, std::vector<std::string> warnings,
             std::vector<std::string> host_warnings,
             bdkcore::ext::Report report, const UiResult& result) {
            if (result.outcome == bdkcore::UiOutcome::kCompleted &&
                result.completion == "grant") {
              // The router and the engine saw the gesture: a grant without
              // one never gets here (ui_request.cc).
              Finish(done, data, trace, Result::ACCEPTED);
              return;
            }
            if (result.outcome != bdkcore::UiOutcome::kDefaulted || !page) {
              // Denied, rejected, withdrawn, dropped: no install.
              Finish(done, data, trace, Result::USER_CANCELED);
              return;
            }
            // No handler took it: a sheet in the page's window, listing every
            // warning. Without a window to put it on the answer is -1, and
            // the install is refused.
            UiPresenters::Get().ShowAlert(
                kInstallsSurface, &page->host_view(),
                BuildAlert(kind, name, warnings, host_warnings, report),
                base::BindOnce(
                    [](DonePtr done, std::shared_ptr<InstallPromptData> data,
                       std::shared_ptr<PromptTrace> trace, int index) {
                      Finish(done, data, trace,
                             index == 0 ? Result::ACCEPTED
                                        : Result::USER_CANCELED);
                    },
                    done, data, trace));
          },
          done, data, trace, weak_page, kind, name, std::move(warnings),
          std::move(host_warnings), report));
}

// Chromium's own prompts: the hook the patch adds to
// ExtensionInstallPrompt::GetDefaultShowDialogCallback().
void OnShowDialog(std::unique_ptr<ExtensionInstallPromptShowParams> params,
                  ExtensionInstallPrompt::DoneCallback done,
                  std::unique_ptr<InstallPromptData> data) {
  InstallPromptContext context;
  if (content::WebContents* contents = params->GetParentWebContents()) {
    if (EngineContents* page = EngineContents::FromWebContents(contents)) {
      context.page = page->GetWeakPtr();
    }
  }
  ShowInstallRequest(params->profile(), std::move(context), std::move(data),
                     std::move(done));
}

// The prompt client of a BDK install.
class BdkInstallClient : public extensions::ExtensionInstallPromptClient {
 public:
  BdkInstallClient(Profile* profile, InstallPromptContext context)
      : profile_(profile), context_(std::move(context)) {}

  // ExtensionInstallPromptClient:
  void OnInstallSuccess(scoped_refptr<const extensions::Extension>,
                        SkBitmap*) override {}
  void OnInstallFailure(const extensions::CrxInstallError&) override {}
  void SetUseAppInstalledBubble(bool) override {}
  void SetSkipPostInstallUI(bool) override {}

  void ConfirmInstall(DoneCallback done,
                      const extensions::Extension* extension) override {
    if (context_.record && !(context_.trace && context_.trace->dry_run)) {
      NoteInstallSource(profile_, extension->id(), *context_.record);
    }
    PromptForExtension(profile_, context_, extension,
                       InstallPromptData::INSTALL_PROMPT, nullptr,
                       std::move(done));
  }

  void ConfirmReEnable(DoneCallback done,
                       const extensions::Extension* extension,
                       content::BrowserContext* browser_context) override {
    PromptForExtension(
        profile_, context_, extension,
        ExtensionInstallPrompt::GetReEnablePromptTypeForExtension(
            browser_context, extension),
        nullptr, std::move(done));
  }

  void ConfirmPermissions(
      DoneCallback done,
      const extensions::Extension* extension,
      std::unique_ptr<const extensions::PermissionSet> permissions) override {
    PromptForExtension(profile_, context_, extension,
                       InstallPromptData::PERMISSIONS_PROMPT,
                       std::move(permissions), std::move(done));
  }

  void ShowInstallDialog(DoneCallback done,
                         const extensions::Extension* extension,
                         const SkBitmap*) override {
    PromptForExtension(profile_, context_, extension,
                       InstallPromptData::INSTALL_PROMPT, nullptr,
                       std::move(done));
  }

 private:
  raw_ptr<Profile> profile_;
  InstallPromptContext context_;
};

// The uninstall confirmation: ExtensionUninstallDialog does the work around
// it (the icon, uninstalling when confirmed, telling its delegate) and shows
// itself through Show(), which here raises the request.
class BdkUninstallDialog : public extensions::ExtensionUninstallDialog {
 public:
  BdkUninstallDialog(Profile* profile,
                     gfx::NativeWindow parent,
                     Delegate* delegate,
                     base::WeakPtr<EngineContents> page)
      : ExtensionUninstallDialog(profile, parent, delegate),
        ui_profile_(profile),
        page_(std::move(page)) {}

  ~BdkUninstallDialog() override {
    // Nothing may reach the delegate from here on.
    closed_ = true;
    Dismiss();
  }

  // Whether the person (or nothing able to ask) said no.
  bool cancelled() const { return cancelled_; }

 private:
  // ExtensionUninstallDialog:
  void Show() override {
    EngineContents* page = PageForPrompt(ui_profile_, page_.get());
    if (!page) {
      Finish(CLOSE_ACTION_CANCELED);
      return;
    }
    base::DictValue requested_by;
    if (triggering_extension()) {
      requested_by.Set("kind", "extension");
      requested_by.Set("extensionId", triggering_extension()->id());
    } else {
      requested_by.Set("kind", "user");
    }
    base::DictValue payload;
    payload.Set("extensionId", extension()->id());
    payload.Set("extensionName", extension()->name());
    payload.Set("requestedBy", std::move(requested_by));
    request_ = RaiseUiRequest(
        *page, BDK_SURFACE_EXTENSION_UNINSTALLS, payload,
        base::BindOnce(&BdkUninstallDialog::OnResult,
                       weak_factory_.GetWeakPtr(), page->GetWeakPtr()));
  }

  void Close() override {
    // The extension was removed some other way while the request was open.
    if (request_) {
      bdkcore::withdraw_ui_request(*request_);
    }
    Dismiss();
  }

  void OnResult(base::WeakPtr<EngineContents> page, const UiResult& result) {
    request_.reset();
    if (result.outcome == bdkcore::UiOutcome::kCompleted &&
        result.completion == "grant") {
      Finish(CLOSE_ACTION_UNINSTALL);
      return;
    }
    if (result.outcome != bdkcore::UiOutcome::kDefaulted || !page) {
      Finish(CLOSE_ACTION_CANCELED);
      return;
    }
    Alert alert;
    alert.title = "Remove \"" + extension()->name() + "\"?";
    alert.message =
        "This removes the extension and the data it stored in this browser.";
    alert.buttons.push_back({"remove", "Remove"});
    alert.buttons.push_back({"cancel", "Cancel"});
    dismiss_ = UiPresenters::Get().ShowAlert(
        kUninstallsSurface, &page->host_view(), std::move(alert),
        base::BindOnce(&BdkUninstallDialog::OnAlert,
                       weak_factory_.GetWeakPtr()));
  }

  void OnAlert(int index) {
    dismiss_.Reset();
    Finish(index == 0 ? CLOSE_ACTION_UNINSTALL : CLOSE_ACTION_CANCELED);
  }

  void Dismiss() {
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
  }

  // Tells the base, once. The delegate may delete this dialog from inside the
  // call, so nothing here touches a member afterwards.
  void Finish(CloseAction action) {
    if (closed_) {
      return;
    }
    closed_ = true;
    cancelled_ = action == CLOSE_ACTION_CANCELED;
    OnDialogClosed(action);
  }

  raw_ptr<Profile> ui_profile_;
  base::WeakPtr<EngineContents> page_;
  std::shared_ptr<bdkcore::Request> request_;
  Dismisser dismiss_;
  bool closed_ = false;
  bool cancelled_ = false;
  base::WeakPtrFactory<BdkUninstallDialog> weak_factory_{this};
};

// One uninstall BDK asked for: owns the dialog until it closes.
class UninstallRun : public extensions::ExtensionUninstallDialog::Delegate {
 public:
  UninstallRun(Profile* profile,
               base::WeakPtr<EngineContents> page,
               scoped_refptr<const extensions::Extension> extension,
               UninstallDone done)
      : done_(std::move(done)) {
    dialog_ = std::make_unique<BdkUninstallDialog>(profile, gfx::NativeWindow(),
                                                   this, std::move(page));
    dialog_->ConfirmUninstall(
        extension, extensions::UNINSTALL_REASON_USER_INITIATED,
        extensions::UNINSTALL_SOURCE_CHROME_EXTENSIONS_PAGE);
  }

  ~UninstallRun() override = default;

  // ExtensionUninstallDialog::Delegate:
  void OnExtensionUninstallDialogClosed(bool did_start_uninstall,
                                        const std::u16string& error) override {
    const UninstallOutcome outcome =
        did_start_uninstall    ? UninstallOutcome::kUninstalled
        : dialog_->cancelled() ? UninstallOutcome::kCancelled
                               : UninstallOutcome::kFailed;
    if (done_) {
      std::move(done_).Run(outcome, Utf8(error));
    }
    // The dialog is still inside its own call: both go afterwards.
    base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(dialog_));
    base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                                  this);
  }

 private:
  std::unique_ptr<BdkUninstallDialog> dialog_;
  UninstallDone done_;
};

}  // namespace

void InstallExtensionPromptHooks() {
  embedder_extension_dialogs::SetInstallDialogHook(
      base::BindRepeating(&OnShowDialog));
  embedder_extension_dialogs::SetUninstallDialogFactory(base::BindRepeating(
      [](Profile* profile, gfx::NativeWindow parent,
         extensions::ExtensionUninstallDialog::Delegate* delegate)
          -> std::unique_ptr<extensions::ExtensionUninstallDialog> {
        return std::make_unique<BdkUninstallDialog>(profile, parent, delegate,
                                                    nullptr);
      }));
}

void PromptForExtension(
    Profile* profile,
    InstallPromptContext context,
    const extensions::Extension* extension,
    InstallPromptData::PromptType type,
    std::unique_ptr<const extensions::PermissionSet> custom_permissions,
    ExtensionInstallPrompt::DoneCallback done) {
  // ExtensionInstallPrompt loads the icon and fills the prompt's data (the
  // permission set Chromium shows for the extension); its dialog callback is
  // ours.
  auto prompt = std::make_unique<ExtensionInstallPrompt>(
      profile, gfx::NativeWindow(), std::make_unique<InstallPromptData>(type));
  ExtensionInstallPrompt* raw = prompt.get();
  auto keep_alive = base::BindOnce(
      [](std::unique_ptr<ExtensionInstallPrompt> prompt,
         ExtensionInstallPrompt::DoneCallback done,
         ExtensionInstallPrompt::DoneCallbackPayload payload) {
        std::move(done).Run(payload);
        // Chromium is not inside the prompt any more: the done callback is
        // always run from a posted task (Finish).
      },
      std::move(prompt), std::move(done));
  raw->ShowDialog(
      std::move(keep_alive), extension, nullptr, std::move(custom_permissions),
      base::BindRepeating(
          [](Profile* profile, InstallPromptContext context,
             std::unique_ptr<ExtensionInstallPromptShowParams> params,
             ExtensionInstallPrompt::DoneCallback done,
             std::unique_ptr<InstallPromptData> data) {
            ShowInstallRequest(profile, context, std::move(data),
                               std::move(done));
          },
          profile, std::move(context)));
}

std::unique_ptr<extensions::ExtensionInstallPromptClient> CreateInstallClient(
    Profile* profile,
    InstallPromptContext context) {
  return std::make_unique<BdkInstallClient>(profile, std::move(context));
}

void ConfirmAndUninstall(Profile* profile,
                         base::WeakPtr<EngineContents> page,
                         scoped_refptr<const extensions::Extension> extension,
                         UninstallDone done) {
  // Deletes itself when the dialog closes.
  new UninstallRun(profile, std::move(page), std::move(extension),
                   std::move(done));
}

}  // namespace bdk
