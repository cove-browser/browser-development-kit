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
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/chrome_download_manager_delegate.h"
#include "chrome/browser/download/download_core_service.h"
#include "chrome/browser/download/download_core_service_factory.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/pref_names.h"
#include "components/download/public/common/download_danger_type.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_target_info.h"
#include "components/history/core/browser/history_service.h"
#include "components/keyed_service/core/service_access_type.h"
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
  return EngineContents::FromWebContents(
      content::DownloadItemUtils::GetWebContents(item));
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
        base::BindOnce(
            [](base::WeakPtr<BdkDownloadManagerDelegate> self,
               std::shared_ptr<
                   DownloadTargetDeterminerDelegate::ConfirmationCallback>
                   answer,
               base::FilePath suggested, std::string name,
               content::WebContents* page_contents, const UiResult& result) {
              if (!self) {
                return;
              }
              if (result.outcome == bdkcore::UiOutcome::kCompleted &&
                  result.completion == "accept") {
                const std::string* destination =
                    result.args.FindString("destination");
                self->Answer(
                    answer, !destination || destination->empty()
                                ? suggested
                                : base::FilePath::FromUTF8Unsafe(*destination));
                return;
              }
              if (result.outcome == bdkcore::UiOutcome::kDefaulted) {
                EngineContents* page =
                    EngineContents::FromWebContents(page_contents);
                UiPresenters::Get().ShowSavePanel(
                    "downloads", page ? &page->host_view() : nullptr, name,
                    base::BindOnce(
                        [](base::WeakPtr<BdkDownloadManagerDelegate> self,
                           std::shared_ptr<DownloadTargetDeterminerDelegate::
                                               ConfirmationCallback> answer,
                           std::optional<std::string> path) {
                          if (self) {
                            self->Answer(
                                answer,
                                path ? base::FilePath::FromUTF8Unsafe(*path)
                                     : base::FilePath());
                          }
                        },
                        self, answer));
                return;
              }
              // denied, rejected, gone
              self->Answer(answer, base::FilePath());
            },
            self, answer, suggested, name, page->web_contents()));
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
        base::BindOnce(
            [](base::WeakPtr<ItemWatch> self, content::WebContents* contents,
               const UiResult& result) {
              if (!self || result.outcome == bdkcore::UiOutcome::kWithdrawn) {
                return;
              }
              self->request_.reset();
              if (result.outcome == bdkcore::UiOutcome::kCompleted &&
                  result.completion == "grant") {
                self->Keep();
              } else if (result.outcome == bdkcore::UiOutcome::kDefaulted) {
                EngineContents* page =
                    EngineContents::FromWebContents(contents);
                Alert alert;
                alert.title = "Keep this download?";
                alert.message = "It may be dangerous.";
                alert.buttons.push_back({"keep", "Keep"});
                alert.buttons.push_back({"discard", "Discard"});
                UiPresenters::Get().ShowAlert(
                    "downloads", page ? &page->host_view() : nullptr,
                    std::move(alert),
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
            },
            self, contents));
  }

  void Keep() {
    // A download that ended while the person was asked (interrupted,
    // cancelled) has nothing left to keep.
    if (!item_ || item_->IsDone()) {
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

// Puts BDK's delegate in place of the one the profile has. Chromium asks
// history for the first free download id when it creates a profile's download
// manager, and gives the answer to the delegate of that moment; until a
// delegate has it, every download waits. The profile Chromium loads itself at
// startup (the last used one) gets its manager during that load, before BDK
// sees the profile, so the answer goes to the delegate this replaces: ask
// again for the new one. A manager the swap creates asks for the new one
// itself, and asking twice could hand out an id twice.
void ReplaceDelegate(DownloadCoreService* service, Profile* profile) {
  const bool had_manager = service->HasCreatedDownloadManager();
  auto delegate = std::make_unique<BdkDownloadManagerDelegate>(profile);
  content::DownloadIdCallback receive_id =
      delegate->GetDownloadIdReceiverCallback();
  service->SetDownloadManagerDelegateForTesting(std::move(delegate));
  // Off the record, ids come from the original profile's manager.
  if (!had_manager || profile->IsOffTheRecord()) {
    return;
  }
  history::HistoryService* history = HistoryServiceFactory::GetForProfile(
      profile, ServiceAccessType::EXPLICIT_ACCESS);
  if (!history) {
    // No database: ids count from the first, as Chromium's do then.
    std::move(receive_id).Run(DownloadItem::kInvalidId);
    return;
  }
  history->GetNextDownloadId(std::move(receive_id));
}

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
  ReplaceDelegate(service, profile);
  new ManagerWatch(profile->GetDownloadManager());
}

void ReplaceDownloadDelegateForTesting(Profile* profile) {
  DownloadCoreService* service =
      DownloadCoreServiceFactory::GetForBrowserContext(profile);
  if (service) {
    profile->GetDownloadManager();  // the manager first, as at startup
    ReplaceDelegate(service, profile);
  }
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
