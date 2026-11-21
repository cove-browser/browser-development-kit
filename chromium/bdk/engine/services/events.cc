// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/events.h"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "bdk/engine/extensions/extension_events.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_model_observer.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/download/public/common/download_item.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_service_observer.h"
#include "components/history/core/browser/history_types.h"
#include "content/public/browser/download_manager.h"
#include "url/gurl.h"

namespace bdk {

namespace {

using bookmarks::BookmarkModel;
using bookmarks::BookmarkNode;
using ProfilePtr = std::weak_ptr<bdkcore::Profile>;

void Emit(const ProfilePtr& profile,
          const char* name,
          const std::string& payload) {
  EmitEvent(profile, name, payload);
}

class HistoryEvents : public history::HistoryServiceObserver {
 public:
  HistoryEvents(history::HistoryService* history, ProfilePtr profile)
      : profile_(std::move(profile)) {
    observation_.Observe(history);
  }

  void OnURLVisited(history::HistoryService*,
                    const history::VisitedURLInfo& info) override {
    Emit(profile_, "history.visited",
         "{\"entry\":" +
             HistoryEntryJson(info.url_row, info.visit_row.visit_time) +
             ",\"source\":\"user\"}");
  }

  void OnHistoryDeletions(history::HistoryService*,
                          const history::DeletionInfo& info) override {
    std::string urls = "[";
    bool first = true;
    if (!info.IsAllHistory()) {
      for (const history::URLRow& row : info.deleted_rows()) {
        urls += (first ? "" : ",") + JsonString(row.url().spec());
        first = false;
      }
    }
    Emit(profile_, "history.removed",
         "{\"urls\":" + urls +
             "],\"all\":" + (info.IsAllHistory() ? "true" : "false") +
             ",\"source\":\"application\"}");
  }

  void HistoryServiceBeingDeleted(history::HistoryService*) override {
    observation_.Reset();
  }

 private:
  ProfilePtr profile_;
  base::ScopedObservation<history::HistoryService,
                          history::HistoryServiceObserver>
      observation_{this};
};

class BookmarkEvents : public bookmarks::BookmarkModelObserver {
 public:
  BookmarkEvents(BookmarkModel* model, ProfilePtr profile)
      : model_(model), profile_(std::move(profile)) {
    model_->AddObserver(this);
  }

  ~BookmarkEvents() override {
    if (model_) {
      model_->RemoveObserver(this);
    }
  }

  void BookmarkModelLoaded(bool) override {}

  void BookmarkModelBeingDeleted() override {
    model_->RemoveObserver(this);
    model_ = nullptr;
  }

  void BookmarkNodeMoved(const BookmarkNode* old_parent,
                         size_t old_index,
                         const BookmarkNode* new_parent,
                         size_t new_index) override {
    const BookmarkNode* node = new_parent->children()[new_index].get();
    Emit(profile_, "bookmarks.moved",
         "{\"node\":" + BookmarkNodeJson(*model_, node) + ",\"oldParentId\":" +
             JsonString(base::NumberToString(old_parent->id())) +
             ",\"oldIndex\":" + base::NumberToString(old_index) +
             ",\"source\":\"application\"}");
  }

  void BookmarkNodeAdded(const BookmarkNode* parent,
                         size_t index,
                         bool added_by_user) override {
    Emit(profile_, "bookmarks.created",
         "{\"node\":" +
             BookmarkNodeJson(*model_, parent->children()[index].get()) +
             ",\"source\":\"application\"}");
  }

  void BookmarkNodeRemoved(const BookmarkNode* parent,
                           size_t old_index,
                           const BookmarkNode* node,
                           const std::set<GURL>& no_longer_bookmarked,
                           const base::Location& location) override {
    Emit(profile_, "bookmarks.removed",
         "{\"id\":" + JsonString(base::NumberToString(node->id())) +
             ",\"parentId\":" + JsonString(base::NumberToString(parent->id())) +
             ",\"source\":\"application\"}");
  }

  void BookmarkNodeChanged(const BookmarkNode* node) override {
    Emit(profile_, "bookmarks.changed",
         "{\"node\":" + BookmarkNodeJson(*model_, node) +
             ",\"source\":\"application\"}");
  }

  void BookmarkNodeFaviconChanged(const BookmarkNode*) override {}

  void BookmarkAllUserNodesRemoved(const std::set<GURL>&,
                                   const base::Location&) override {}

  void BookmarkNodeChildrenReordered(const BookmarkNode* node) override {
    Emit(profile_, "bookmarks.reordered",
         "{\"parentId\":" + JsonString(base::NumberToString(node->id())) +
             ",\"source\":\"application\"}");
  }

 private:
  raw_ptr<BookmarkModel> model_;
  ProfilePtr profile_;
};

// Reports the downloads of one profile: new ones, their progress, their end
// and their removal.
class DownloadEvents : public content::DownloadManager::Observer,
                       public download::DownloadItem::Observer {
 public:
  DownloadEvents(content::DownloadManager* manager, ProfilePtr profile)
      : manager_(manager), profile_(std::move(profile)) {
    manager_->AddObserver(this);
    content::DownloadManager::DownloadVector items;
    manager_->GetAllDownloads(&items);
    for (download::DownloadItem* item : items) {
      item->AddObserver(this);
    }
  }

  ~DownloadEvents() override {
    if (manager_) {
      manager_->RemoveObserver(this);
      content::DownloadManager::DownloadVector items;
      manager_->GetAllDownloads(&items);
      for (download::DownloadItem* item : items) {
        item->RemoveObserver(this);
      }
    }
  }

  void OnDownloadCreated(content::DownloadManager*,
                         download::DownloadItem* item) override {
    item->AddObserver(this);
    Emit(profile_, "downloads.created",
         "{\"item\":" + DownloadItemJson(*item) +
             ",\"source\":\"application\"}");
  }

  void ManagerGoingDown(content::DownloadManager*) override {
    manager_ = nullptr;
  }

  void OnDownloadUpdated(download::DownloadItem* item) override {
    switch (item->GetState()) {
      case download::DownloadItem::COMPLETE:
        Emit(profile_, "downloads.completed",
             "{\"item\":" + DownloadItemJson(*item) + "}");
        return;
      case download::DownloadItem::INTERRUPTED:
        Emit(profile_, "downloads.interrupted",
             "{\"item\":" + DownloadItemJson(*item) + ",\"reason\":null}");
        return;
      default:
        Emit(profile_, "downloads.updated",
             "{\"item\":" + DownloadItemJson(*item) + "}");
    }
  }

  void OnDownloadRemoved(download::DownloadItem* item) override {
    Emit(profile_, "downloads.removed",
         "{\"id\":" + JsonString(base::NumberToString(item->GetId())) +
             ",\"source\":\"application\"}");
  }

  void OnDownloadDestroyed(download::DownloadItem* item) override {
    item->RemoveObserver(this);
  }

 private:
  raw_ptr<content::DownloadManager> manager_;
  ProfilePtr profile_;
};

class ProfileEvents : public ServiceEvents {
 public:
  ProfileEvents(Profile* profile, const ProfilePtr& bdk_profile) {
    Profile* original = profile->GetOriginalProfile();
    if (profile == original) {
      if (history::HistoryService* history =
              HistoryServiceFactory::GetForProfile(
                  profile, ServiceAccessType::EXPLICIT_ACCESS)) {
        history_ = std::make_unique<HistoryEvents>(history, bdk_profile);
      }
      if (BookmarkModel* model =
              BookmarkModelFactory::GetForBrowserContext(profile)) {
        bookmarks_ = std::make_unique<BookmarkEvents>(model, bdk_profile);
      }
    }
    if (content::DownloadManager* manager = profile->GetDownloadManager()) {
      downloads_ = std::make_unique<DownloadEvents>(manager, bdk_profile);
    }
    cookies_ = CreateCookieEvents(profile, bdk_profile);
    passwords_ = CreatePasswordEvents(profile, bdk_profile);
    autofill_ = CreateAutofillEvents(profile, bdk_profile);
    extensions_ = CreateExtensionEvents(profile, bdk_profile);
  }

 private:
  std::unique_ptr<HistoryEvents> history_;
  std::unique_ptr<BookmarkEvents> bookmarks_;
  std::unique_ptr<DownloadEvents> downloads_;
  std::unique_ptr<ServiceEvents> cookies_;
  std::unique_ptr<ServiceEvents> passwords_;
  std::unique_ptr<ServiceEvents> autofill_;
  std::unique_ptr<ServiceEvents> extensions_;
};

}  // namespace

std::unique_ptr<ServiceEvents> CreateServiceEvents(
    Profile* profile,
    std::weak_ptr<bdkcore::Profile> bdk_profile) {
  return std::make_unique<ProfileEvents>(profile, bdk_profile);
}

void EmitEvent(const std::weak_ptr<bdkcore::Profile>& bdk_profile,
               const std::string& name,
               const std::string& payload_json) {
  if (std::shared_ptr<bdkcore::Profile> p = bdk_profile.lock()) {
    EmitEvent(p, name, payload_json);
  }
}

void EmitEvent(const std::shared_ptr<bdkcore::Profile>& bdk_profile,
               const std::string& name,
               const std::string& payload_json) {
  if (std::shared_ptr<bdkcore::Runtime> runtime = bdkcore::current_runtime()) {
    bdkcore::emit_builtin_event(
        runtime, name, bdk_profile ? bdk_profile->handle : 0, payload_json);
  }
}

}  // namespace bdk
