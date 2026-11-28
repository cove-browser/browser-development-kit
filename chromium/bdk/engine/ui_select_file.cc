// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_select_file.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/ui_dialogs.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/shell_dialogs/select_file_dialog.h"
#include "ui/shell_dialogs/select_file_dialog_factory.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {

namespace {

class PagePolicy;

// The policies of BDK's alive, so the factory knows one of its own (there is
// no RTTI to tell a policy's class).
std::set<const ui::SelectFilePolicy*>& LivePolicies() {
  static base::NoDestructor<std::set<const ui::SelectFilePolicy*>> live;
  return *live;
}

// Chrome's policy (whether file dialogs are allowed at all), and the page.
class PagePolicy : public ui::SelectFilePolicy {
 public:
  explicit PagePolicy(content::WebContents* web_contents)
      : chrome_(web_contents),
        web_contents_(web_contents ? web_contents->GetWeakPtr() : nullptr) {
    LivePolicies().insert(this);
  }
  ~PagePolicy() override { LivePolicies().erase(this); }

  bool CanOpenSelectFileDialog() override {
    return chrome_.CanOpenSelectFileDialog();
  }
  void SelectFileDenied() override { chrome_.SelectFileDenied(); }

  content::WebContents* web_contents() const { return web_contents_.get(); }

 private:
  ChromeSelectFilePolicy chrome_;
  base::WeakPtr<content::WebContents> web_contents_;
};

const char* ModeName(ui::SelectFileDialog::Type type) {
  switch (type) {
    case ui::SelectFileDialog::SELECT_FOLDER:
    case ui::SelectFileDialog::SELECT_UPLOAD_FOLDER:
    case ui::SelectFileDialog::SELECT_EXISTING_FOLDER:
      return "directory";
    case ui::SelectFileDialog::SELECT_SAVEAS_FILE:
      return "save";
    case ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE:
      return "openMultiple";
    case ui::SelectFileDialog::SELECT_NONE:
    case ui::SelectFileDialog::SELECT_OPEN_FILE:
      break;
  }
  return "open";
}

// A file dialog of a page: a file_pickers request of that page.
class PageFileDialog : public ui::SelectFileDialog {
 public:
  PageFileDialog(Listener* listener, std::unique_ptr<PagePolicy> policy)
      : ui::SelectFileDialog(listener, std::move(policy)) {}

  // ui::BaseShellDialog:
  bool IsRunning(gfx::NativeWindow owning_window) const override {
    return running_;
  }
  void ListenerDestroyed() override { listener_ = nullptr; }

 protected:
  ~PageFileDialog() override = default;

  // ui::SelectFileDialog:
  void SelectFileImpl(Type type,
                      const std::u16string& title,
                      const base::FilePath& default_path,
                      const FileTypeInfo* file_types,
                      int file_type_index,
                      const base::FilePath::StringType& default_extension,
                      gfx::NativeWindow owning_window,
                      const GURL* caller) override {
    content::WebContents* contents =
        static_cast<PagePolicy*>(select_file_policy())->web_contents();
    EngineContents* page =
        contents ? EngineContents::FromWebContents(contents) : nullptr;
    if (!page) {
      // The page went while Chromium was getting ready to ask.
      if (listener_) {
        listener_->FileSelectionCanceled();
      }
      return;
    }
    type_ = type;
    has_type_choices_ = file_types && file_types->extensions.size() > 1;
    base::DictValue payload;
    payload.Set("mode", ModeName(type));
    base::ListValue types;
    if (file_types) {
      for (const std::vector<base::FilePath::StringType>& group :
           file_types->extensions) {
        for (const base::FilePath::StringType& extension : group) {
          types.Append("." + extension);
        }
      }
    }
    payload.Set("acceptedTypes", std::move(types));
    if (type == SELECT_SAVEAS_FILE && !default_path.BaseName().empty()) {
      payload.Set("suggestedName", default_path.BaseName().AsUTF8Unsafe());
    }
    payload.Set("isFileSystemAccessAPI", true);
    const url::Origin origin =
        caller && caller->is_valid()
            ? url::Origin::Create(*caller)
            : contents->GetPrimaryMainFrame()->GetLastCommittedOrigin();
    payload.Set("origin", origin.Serialize());
    running_ = true;
    page->dialogs().RunPicker(
        std::move(payload),
        base::BindOnce(&PageFileDialog::OnChosen,
                       scoped_refptr<PageFileDialog>(this)));
  }

  bool HasMultipleFileTypeChoicesImpl() override { return has_type_choices_; }

 private:
  void OnChosen(std::vector<base::FilePath> paths) {
    running_ = false;
    if (!listener_) {
      return;
    }
    if (paths.empty()) {
      listener_->FileSelectionCanceled();
      return;
    }
    if (type_ == SELECT_OPEN_MULTI_FILE) {
      std::vector<ui::SelectedFileInfo> files;
      for (const base::FilePath& path : paths) {
        files.emplace_back(path);
      }
      listener_->MultiFilesSelected(files);
      return;
    }
    listener_->FileSelected(ui::SelectedFileInfo(paths.front()), 0);
  }

  Type type_ = SELECT_NONE;
  bool has_type_choices_ = false;
  bool running_ = false;
};

class Factory : public ui::SelectFileDialogFactory {
 public:
  ui::SelectFileDialog* Create(
      ui::SelectFileDialog::Listener* listener,
      std::unique_ptr<ui::SelectFilePolicy> policy) override {
    // Only BDK's policy carries a page; one created by
    // CreateSelectFilePolicy for no page, or none at all, is Chromium's.
    if (policy && LivePolicies().contains(policy.get()) &&
        static_cast<PagePolicy*>(policy.get())->web_contents()) {
      return new PageFileDialog(
          listener, std::unique_ptr<PagePolicy>(
                        static_cast<PagePolicy*>(policy.release())));
    }
    return ui::CreateSelectFileDialog(listener, std::move(policy));
  }
};

}  // namespace

std::unique_ptr<ui::SelectFilePolicy> CreateSelectFilePolicy(
    content::WebContents* web_contents) {
  return std::make_unique<PagePolicy>(web_contents);
}

void InstallSelectFileDialogs() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  ui::SelectFileDialog::SetFactory(std::make_unique<Factory>());
}

}  // namespace bdk
