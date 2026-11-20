// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_dialogs.h"

#include <algorithm>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "components/strings/grit/components_strings.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/javascript_dialog_type.h"
#include "ui/base/l10n/l10n_util.h"

namespace bdk {

namespace {

using Mode = blink::mojom::FileChooserParams::Mode;

const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::kOpenMultiple:
      return "openMultiple";
    case Mode::kUploadFolder:
    case Mode::kOpenDirectory:
      return "directory";
    case Mode::kSave:
      return "save";
    case Mode::kOpen:
      break;
  }
  return "open";
}

// The files of a folder, recursively, skipping hidden ones as Chrome's file
// chooser does.
std::vector<base::FilePath> EnumerateFolder(const base::FilePath& folder) {
  std::vector<base::FilePath> files;
  base::FileEnumerator enumerator(folder, /*recursive=*/true,
                                  base::FileEnumerator::FILES);
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    if (path.BaseName().value().starts_with(".")) {
      continue;
    }
    files.push_back(path);
  }
  return files;
}

blink::mojom::FileChooserFileInfoPtr FileInfo(const base::FilePath& path) {
  return blink::mojom::FileChooserFileInfo::NewNativeFile(
      blink::mojom::NativeFileInfo::New(path, std::u16string(),
                                        std::vector<std::u16string>()));
}

}  // namespace

PageDialogs::Pending::Pending() = default;
PageDialogs::Pending::Pending(Pending&&) = default;
PageDialogs::Pending& PageDialogs::Pending::operator=(Pending&&) = default;
PageDialogs::Pending::~Pending() = default;

PageDialogs::PageDialogs(EngineContents& page) : page_(page) {}

PageDialogs::~PageDialogs() {
  CancelAll();
}

void PageDialogs::OnNavigated() {
  dialogs_since_navigation_ = 0;
  suppressed_ = false;
}

// JavaScript dialogs ----------------------------------------------------------

void PageDialogs::RunJavaScriptDialog(
    content::WebContents* web_contents,
    content::RenderFrameHost* render_frame_host,
    content::JavaScriptDialogType dialog_type,
    const std::u16string& message_text,
    const std::u16string& default_prompt_text,
    DialogClosedCallback callback,
    bool* did_suppress_message) {
  if (suppressed_) {
    *did_suppress_message = true;
    return;
  }
  const char* kind = "alert";
  switch (dialog_type) {
    case content::JAVASCRIPT_DIALOG_TYPE_CONFIRM:
      kind = "confirm";
      break;
    case content::JAVASCRIPT_DIALOG_TYPE_PROMPT:
      kind = "prompt";
      break;
    case content::JAVASCRIPT_DIALOG_TYPE_ALERT:
      break;
  }
  base::DictValue payload;
  payload.Set("kind", kind);
  payload.Set("origin",
              render_frame_host->GetLastCommittedOrigin().Serialize());
  payload.Set("pageTitle", base::UTF16ToUTF8(web_contents->GetTitle()));
  payload.Set("message", base::UTF16ToUTF8(message_text));
  if (dialog_type == content::JAVASCRIPT_DIALOG_TYPE_PROMPT) {
    payload.Set("defaultPromptText", base::UTF16ToUTF8(default_prompt_text));
  }
  payload.Set("isReload", false);
  payload.Set("offersSuppression", dialogs_since_navigation_ > 0);
  ++dialogs_since_navigation_;
  Pending pending;
  pending.script = std::move(callback);
  Start(std::move(pending), BDK_SURFACE_JAVASCRIPT_DIALOGS, std::move(payload));
}

void PageDialogs::RunBeforeUnloadDialog(
    content::WebContents* web_contents,
    content::RenderFrameHost* render_frame_host,
    bool is_reload,
    DialogClosedCallback callback) {
  // Chromium's fixed text: the page's own string is never shown.
  base::DictValue payload;
  payload.Set("kind", "beforeUnload");
  payload.Set("origin",
              render_frame_host->GetLastCommittedOrigin().Serialize());
  payload.Set("pageTitle", base::UTF16ToUTF8(web_contents->GetTitle()));
  payload.Set("message", base::UTF16ToUTF8(l10n_util::GetStringUTF16(
                             IDS_BEFOREUNLOAD_MESSAGEBOX_MESSAGE)));
  payload.Set("isReload", is_reload);
  payload.Set("offersSuppression", false);
  Pending pending;
  pending.script = std::move(callback);
  Start(std::move(pending), BDK_SURFACE_JAVASCRIPT_DIALOGS, std::move(payload));
}

bool PageDialogs::HandleJavaScriptDialog(
    content::WebContents* web_contents,
    bool accept,
    const std::u16string* prompt_override) {
  // Only the application's own presenter answers a dialog: an automation
  // session does not (ui-requests.md section 3).
  return false;
}

void PageDialogs::CancelDialogs(content::WebContents* web_contents,
                                bool reset_state) {
  CancelAll();
  if (reset_state) {
    OnNavigated();
  }
}

// File pickers ----------------------------------------------------------------

void PageDialogs::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    scoped_refptr<content::FileSelectListener> listener,
    const blink::mojom::FileChooserParams& params) {
  base::DictValue payload;
  payload.Set("mode", ModeName(params.mode));
  base::ListValue types;
  for (const std::u16string& type : params.accept_types) {
    types.Append(base::UTF16ToUTF8(type));
  }
  payload.Set("acceptedTypes", std::move(types));
  if (params.mode == Mode::kSave && !params.default_file_name.empty()) {
    payload.Set("suggestedName", params.default_file_name.AsUTF8Unsafe());
  }
  payload.Set("isFileSystemAccessAPI", false);
  payload.Set("origin",
              render_frame_host->GetLastCommittedOrigin().Serialize());
  Pending pending;
  pending.listener = std::move(listener);
  pending.mode = params.mode;
  Start(std::move(pending), BDK_SURFACE_FILE_PICKERS, std::move(payload));
}

void PageDialogs::EnumerateDirectory(
    scoped_refptr<content::FileSelectListener> listener,
    const base::FilePath& path) {
  // The page asked to upload a folder it already knows the person chose (a
  // drop): enumerate it, as Chrome does.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&EnumerateFolder, path),
      base::BindOnce(&PageDialogs::OnFolderEnumerated,
                     weak_factory_.GetWeakPtr(), std::move(listener), path));
}

void PageDialogs::OnFolderEnumerated(
    scoped_refptr<content::FileSelectListener> listener,
    const base::FilePath& folder,
    std::vector<base::FilePath> files) {
  std::vector<blink::mojom::FileChooserFileInfoPtr> infos;
  for (const base::FilePath& file : files) {
    infos.push_back(FileInfo(file));
  }
  listener->FileSelected(std::move(infos), folder, Mode::kUploadFolder);
}

// Both ------------------------------------------------------------------------

void PageDialogs::Start(Pending pending,
                        uint32_t surface,
                        base::DictValue payload) {
  const int id = next_id_++;
  base::DictValue shown = payload.Clone();
  pending_.emplace(id, std::move(pending));
  std::shared_ptr<bdkcore::Request> request = RaiseUiRequest(
      page_->Core(), surface, payload,
      base::BindOnce(&PageDialogs::OnEnded, weak_factory_.GetWeakPtr(), id,
                     std::move(shown)),
      &page_->host_view());
  if (!request) {
    Complete(id, false, std::string(), {});
    return;
  }
  // The request may already have ended and erased the entry.
  auto it = pending_.find(id);
  if (it != pending_.end()) {
    it->second.request = std::move(request);
  }
}

void PageDialogs::OnEnded(int id,
                          base::DictValue payload,
                          const UiResult& result) {
  auto it = pending_.find(id);
  if (it == pending_.end()) {
    return;
  }
  switch (result.outcome) {
    case bdkcore::UiOutcome::kCompleted: {
      if (result.args.FindBool("suppressFurther").value_or(false)) {
        suppressed_ = true;
      }
      const bool accepted = result.completion == "accept";
      const std::string* text = result.args.FindString("text");
      std::vector<base::FilePath> paths;
      if (const base::ListValue* list = result.args.FindList("paths")) {
        for (const base::Value& path : *list) {
          if (path.is_string()) {
            paths.push_back(base::FilePath::FromUTF8Unsafe(path.GetString()));
          }
        }
      }
      Complete(id, accepted, text ? *text : std::string(), std::move(paths));
      return;
    }
    case bdkcore::UiOutcome::kDefaulted: {
      if (it->second.listener) {
        it->second.dismiss = UiPresenters::Get().ShowFilePicker(
            &page_->host_view(), payload,
            base::BindOnce(&PageDialogs::OnPickerAnswered,
                           weak_factory_.GetWeakPtr(), id));
      } else {
        it->second.dismiss = UiPresenters::Get().ShowJavaScriptDialog(
            &page_->host_view(), payload,
            base::BindOnce(&PageDialogs::OnScriptAnswered,
                           weak_factory_.GetWeakPtr(), id));
      }
      return;
    }
    default:
      // Rejected, dropped, withdrawn or disabled: the safe answer.
      Complete(id, false, std::string(), {});
      return;
  }
}

void PageDialogs::OnScriptAnswered(int id, JavaScriptDialogAnswer answer) {
  if (answer.suppress_further) {
    suppressed_ = true;
  }
  Complete(id, answer.accepted, answer.text, {});
}

void PageDialogs::OnPickerAnswered(int id, std::vector<base::FilePath> paths) {
  Complete(id, !paths.empty(), std::string(), std::move(paths));
}

void PageDialogs::Complete(int id,
                           bool accepted,
                           const std::string& text,
                           std::vector<base::FilePath> paths) {
  auto it = pending_.find(id);
  if (it == pending_.end()) {
    return;
  }
  Pending pending = std::move(it->second);
  pending_.erase(it);
  if (pending.dismiss) {
    std::move(pending.dismiss).Run();
  }
  if (pending.script) {
    std::move(pending.script).Run(accepted, base::UTF8ToUTF16(text));
    return;
  }
  if (!pending.listener) {
    return;
  }
  if (!accepted || paths.empty()) {
    pending.listener->FileSelectionCanceled();
    return;
  }
  if (pending.mode == Mode::kUploadFolder) {
    // The chosen folder stands for the files in it.
    EnumerateDirectory(std::move(pending.listener), paths.front());
    return;
  }
  if (pending.mode != Mode::kOpenMultiple && paths.size() > 1) {
    paths.resize(1);
  }
  std::vector<blink::mojom::FileChooserFileInfoPtr> infos;
  for (const base::FilePath& path : paths) {
    infos.push_back(FileInfo(path));
  }
  pending.listener->FileSelected(std::move(infos), base::FilePath(),
                                 pending.mode);
}

void PageDialogs::CancelAll() {
  std::vector<int> ids;
  for (const auto& [id, pending] : pending_) {
    ids.push_back(id);
  }
  for (int id : ids) {
    auto it = pending_.find(id);
    if (it == pending_.end()) {
      continue;
    }
    // Tells the application it is withdrawn; its end answers the page.
    if (it->second.request) {
      bdkcore::withdraw_ui_request(*it->second.request);
    }
    Complete(id, false, std::string(), {});
  }
}

}  // namespace bdk
