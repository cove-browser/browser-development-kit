// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/download_prompts.h"

#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/services/native_presenter.h"
#include "bdk/engine/services/ui_requests.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/chrome_download_manager_delegate.h"
#include "chrome/browser/download/download_core_service.h"
#include "chrome/browser/download/download_core_service_factory.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/pref_names.h"
#include "components/download/public/common/download_danger_type.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_target_info.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/web_contents.h"
#include "ui/shell_dialogs/selected_file_info.h"

namespace bdk {

namespace {

using download::DownloadItem;

// For tests: every download target is determined as dangerous content, as
// Safe Browsing or the file-type policy would (this build has neither before
// M6, so no real download is dangerous).
bool g_force_danger = false;

EngineContents* PageOf(DownloadItem* item) {
  return FindEngineContents(content::DownloadItemUtils::GetWebContents(item));
}

base::DictValue DownloadInfo(DownloadItem* item,
                             const std::string& suggested_name) {
  base::DictValue info;
  info.Set("url", item->GetURL().spec());
  info.Set("suggestedName", suggested_name);
  info.Set("mimeType", item->GetMimeType());
  if (item->GetTotalBytes() > 0) {
    info.Set("size", static_cast<double>(item->GetTotalBytes()));
  }
  if (item->GetReferrerUrl().is_valid()) {
    info.Set("referrer", item->GetReferrerUrl().spec());
  }
  return info;
}

const char* DangerKind(download::DownloadDangerType type, bool insecure) {
  if (insecure) {
    return "insecure";
  }
  switch (type) {
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE:
      return "dangerousFileType";
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_URL:
    case download::DOWNLOAD_DANGER_TYPE_DANGEROUS_HOST:
      return "dangerousURL";
    case download::DOWNLOAD_DANGER_TYPE_UNCOMMON_CONTENT:
      return "uncommon";
    default:
      return "dangerousContent";
  }
}

// The destination question: Chromium shows a file picker through the download
// manager delegate; BDK's delegate asks through a request instead.
class BdkDownloadManagerDelegate : public ChromeDownloadManagerDelegate {
 public:
  explicit BdkDownloadManagerDelegate(Profile* profile)
      : ChromeDownloadManagerDelegate(profile) {}

 public:
  bool DetermineDownloadTarget(
      DownloadItem* item,
      download::DownloadTargetCallback* callback) override {
    if (g_force_danger) {
      *callback = base::BindOnce(
          [](download::DownloadTargetCallback original,
             download::DownloadTargetInfo info) {
            info.danger_type = download::DOWNLOAD_DANGER_TYPE_DANGEROUS_CONTENT;
            std::move(original).Run(std::move(info));
          },
          std::move(*callback));
    }
    return ChromeDownloadManagerDelegate::DetermineDownloadTarget(item,
                                                                  callback);
  }

 protected:
  void ShowFilePickerForDownload(
      DownloadItem* download,
      const base::FilePath& suggested_path,
      DownloadTargetDeterminerDelegate::ConfirmationCallback callback)
      override {
    EngineContents* page = PageOf(download);
    if (!page) {
      // A download of no BDK page (an extension's, a restored one): the
      // usual picker.
      ChromeDownloadManagerDelegate::ShowFilePickerForDownload(
          download, suggested_path, std::move(callback));
      return;
    }
    const std::string name = suggested_path.BaseName().AsUTF8Unsafe();
    base::DictValue payload;
    payload.Set("download", DownloadInfo(download, name));
    payload.Set("needsLocation", true);
    auto answer = std::make_shared<
        DownloadTargetDeterminerDelegate::ConfirmationCallback>(
        std::move(callback));
    base::WeakPtr<BdkDownloadManagerDelegate> self = weak_factory_.GetWeakPtr();
    const base::FilePath suggested = suggested_path;
    RaiseUiRequest(
        *page, BDK_SURFACE_DOWNLOADS, payload,
        [self, answer, suggested, name, page_contents = page->web_contents()](
            bdkcore::UiOutcome outcome, const std::string& completion,
            const std::string& arguments) {
          if (!self) {
            return;
          }
          if (outcome == bdkcore::UiOutcome::kCompleted &&
              completion == "accept") {
            const std::string destination =
                ArgumentString(arguments, "destination");
            self->Answer(answer,
                         destination.empty()
                             ? suggested
                             : base::FilePath::FromUTF8Unsafe(destination));
            return;
          }
          if (outcome == bdkcore::UiOutcome::kDefaulted) {
            EngineContents* page = FindEngineContents(page_contents);
            PresentSavePanel(
                "downloads", name,
                page ? WindowOfPage(*page) : bdk_native_handle_t{},
                base::BindOnce(
                    [](base::WeakPtr<BdkDownloadManagerDelegate> self,
                       std::shared_ptr<DownloadTargetDeterminerDelegate::
                                           ConfirmationCallback> answer,
                       std::optional<std::string> path) {
                      if (self) {
                        self->Answer(
                            answer, path ? base::FilePath::FromUTF8Unsafe(*path)
                                         : base::FilePath());
                      }
                    },
                    self, answer));
            return;
          }
          self->Answer(answer, base::FilePath());  // denied, rejected, gone
        });
  }

 private:
  // An empty path cancels the download.
  void Answer(
      std::shared_ptr<DownloadTargetDeterminerDelegate::ConfirmationCallback>
          answer,
      const base::FilePath& path) {
    if (!*answer) {
      return;
    }
    OnConfirmationCallbackComplete(
        std::move(*answer),
        path.empty() ? DownloadConfirmationResult::CANCELED
                     : DownloadConfirmationResult::CONFIRMED,
        path.empty() ? ui::SelectedFileInfo() : ui::SelectedFileInfo(path));
  }

  base::WeakPtrFactory<BdkDownloadManagerDelegate> weak_factory_{this};
};

// Watches one download for becoming dangerous.
class ItemWatch : public DownloadItem::Observer {
 public:
  explicit ItemWatch(DownloadItem* item) : item_(item) {
    item_->AddObserver(this);
    OnDownloadUpdated(item_);
  }
  ~ItemWatch() override {
    if (item_) {
      item_->RemoveObserver(this);
    }
  }

  void OnDownloadUpdated(DownloadItem* item) override {
    const bool dangerous = item->IsDangerous() || item->IsInsecure();
    if (!dangerous && request_) {
      bdkcore::withdraw_ui_request(*request_);  // resolved elsewhere
      request_.reset();
      prompted_ = false;
    }
    if (dangerous && !prompted_) {
      prompted_ = true;
      Ask(item);
    }
  }

  void OnDownloadDestroyed(DownloadItem* item) override {
    if (request_) {
      bdkcore::withdraw_ui_request(*request_);
    }
    item_->RemoveObserver(this);
    item_ = nullptr;
    delete this;
  }

 private:
  void Ask(DownloadItem* item) {
    EngineContents* page = PageOf(item);
    if (!page) {
      return;  // not a BDK page's download: Chromium's own handling
    }
    const bool insecure = item->IsInsecure();
    const char* kind = DangerKind(item->GetDangerType(), insecure);
    const std::string name =
        item->GetTargetFilePath().BaseName().AsUTF8Unsafe();
    base::DictValue payload;
    payload.Set("download", DownloadInfo(item, name));
    payload.Set("danger", kind);
    payload.Set("text", base::DictValue()
                            .Set("title", "This download may be dangerous")
                            .Set("message", name + " (" + kind + ")"));
    content::WebContents* contents = page->web_contents();
    const std::string guid = item->GetGuid();
    base::WeakPtr<ItemWatch> self = weak_factory_.GetWeakPtr();
    request_ = RaiseUiRequest(
        *page, BDK_SURFACE_DOWNLOADS, payload,
        [self, contents](bdkcore::UiOutcome outcome,
                         const std::string& completion, const std::string&) {
          if (!self || outcome == bdkcore::UiOutcome::kWithdrawn) {
            return;
          }
          self->request_.reset();
          if (outcome == bdkcore::UiOutcome::kCompleted &&
              completion == "grant") {
            self->Keep();
          } else if (outcome == bdkcore::UiOutcome::kDefaulted) {
            EngineContents* page = FindEngineContents(contents);
            Alert alert;
            alert.title = "Keep this download?";
            alert.message = "It may be dangerous.";
            alert.buttons.push_back({"keep", "Keep"});
            alert.buttons.push_back({"discard", "Discard"});
            PresentAlert("downloads", std::move(alert),
                         page ? WindowOfPage(*page) : bdk_native_handle_t{},
                         base::BindOnce(
                             [](base::WeakPtr<ItemWatch> self, int index) {
                               if (self) {
                                 index == 0 ? self->Keep() : self->Discard();
                               }
                             },
                             self));
          } else {
            self->Discard();  // denied, rejected, dropped: never kept
          }
        });
  }

  void Keep() {
    if (!item_) {
      return;
    }
    if (item_->IsInsecure()) {
      item_->ValidateInsecureDownload();
    } else if (item_->IsDangerous()) {
      item_->ValidateDangerousDownload();
    }
  }

  void Discard() {
    if (item_ && (item_->IsDangerous() || item_->IsInsecure())) {
      item_->Remove();
    }
  }

  raw_ptr<DownloadItem> item_;
  bool prompted_ = false;
  std::shared_ptr<bdkcore::Request> request_;
  base::WeakPtrFactory<ItemWatch> weak_factory_{this};
};

// Watches a profile's download manager.
class ManagerWatch : public content::DownloadManager::Observer {
 public:
  explicit ManagerWatch(content::DownloadManager* manager) : manager_(manager) {
    manager_->AddObserver(this);
  }
  ~ManagerWatch() override {
    if (manager_) {
      manager_->RemoveObserver(this);
    }
  }

  void OnDownloadCreated(content::DownloadManager* manager,
                         DownloadItem* item) override {
    EngineContents* page = PageOf(item);
    if (page) {
      std::shared_ptr<bdkcore::Contents> core = page->Core();
      std::shared_ptr<bdkcore::Runtime> rt = core ? core->rt.lock() : nullptr;
      if (rt && !bdkcore::ui_surface_available(*rt, BDK_SURFACE_DOWNLOADS)) {
        item->Cancel(true);  // disabled: downloads are cancelled at the start
        return;
      }
    }
    new ItemWatch(item);  // deletes itself with the item
  }

  void ManagerGoingDown(content::DownloadManager* manager) override {
    manager_->RemoveObserver(this);
    manager_ = nullptr;
    delete this;
  }

 private:
  raw_ptr<content::DownloadManager> manager_;
};

}  // namespace

void InstallDownloadPrompts(Profile* profile) {
  // A profile is opened again by a later runtime; its delegate and watch stay.
  static const char kInstalled[] = "bdk.download_prompts";
  if (profile->GetUserData(kInstalled)) {
    return;
  }
  profile->SetUserData(kInstalled,
                       std::make_unique<base::SupportsUserData::Data>());
  DownloadCoreService* service =
      DownloadCoreServiceFactory::GetForBrowserContext(profile);
  if (!service) {
    return;
  }
  service->SetDownloadManagerDelegateForTesting(
      std::make_unique<BdkDownloadManagerDelegate>(profile));
  new ManagerWatch(profile->GetDownloadManager());
}

void ForceDangerousDownloadsForTesting(bool on) {
  g_force_danger = on;
}

void AskWhereToSaveForTesting(bool ask) {
  for (Profile* profile :
       g_browser_process->profile_manager()->GetLoadedProfiles()) {
    profile->GetPrefs()->SetBoolean(prefs::kPromptForDownload, ask);
    for (Profile* otr : profile->GetAllOffTheRecordProfiles()) {
      otr->GetPrefs()->SetBoolean(prefs::kPromptForDownload, ask);
    }
  }
}

}  // namespace bdk
