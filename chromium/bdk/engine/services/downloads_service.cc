// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// downloads.*: the profile's DownloadManager. A download's identifier is
// Chromium's own, written as a string. Where files go is the `downloads.*`
// settings (settings_service.cc). A private profile has its own manager and
// its own list.

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/profiles/profile.h"
#include "components/download/public/common/download_danger_type.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_url_parameters.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_manager.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/gurl.h"

namespace bdk {

namespace {

using download::DownloadItem;

constexpr size_t kDefaultLimit = 100;
constexpr char kCursorPrefix[] = "d:";

content::DownloadManager* ManagerOf(const ServiceCall& c) {
  return c.profile()->GetDownloadManager();
}

const char* StateName(const DownloadItem& item) {
  switch (item.GetState()) {
    case DownloadItem::IN_PROGRESS:
      return item.IsPaused() ? "paused" : "inProgress";
    case DownloadItem::COMPLETE:
      return "complete";
    case DownloadItem::CANCELLED:
      return "cancelled";
    default:
      return "interrupted";
  }
}

}  // namespace

std::string DownloadItemJson(const DownloadItem& item) {
  const base::FilePath& path = item.GetState() == DownloadItem::COMPLETE
                                   ? item.GetTargetFilePath()
                                   : item.GetFullPath();
  const int64_t total = item.GetTotalBytes();
  return "{\"id\":" + JsonString(base::NumberToString(item.GetId())) +
         ",\"url\":" + JsonString(item.GetURL().spec()) + ",\"path\":" +
         (path.empty() ? std::string("null")
                       : JsonString(path.AsUTF8Unsafe())) +
         ",\"state\":" + JsonString(StateName(item)) +
         ",\"receivedBytes\":" + base::NumberToString(item.GetReceivedBytes()) +
         ",\"totalBytes\":" + base::NumberToString(total < 0 ? 0 : total) +
         ",\"bytesPerSecond\":" + base::NumberToString(item.CurrentSpeed()) +
         ",\"danger\":" +
         (item.IsDangerous() ? std::string("\"dangerous\"")
                             : std::string("null")) +
         ",\"mimeType\":" + JsonStringOrNull(item.GetMimeType()) +
         ",\"startTime\":" + JsonTime(item.GetStartTime()) + ",\"endTime\":" +
         (item.GetState() == DownloadItem::COMPLETE ||
                  item.GetState() == DownloadItem::CANCELLED ||
                  item.GetState() == DownloadItem::INTERRUPTED
              ? JsonTime(item.GetEndTime())
              : std::string("null")) +
         "}";
}

namespace {

// The download the call names; null, with the call rejected, if there is none.
DownloadItem* ItemArg(const std::shared_ptr<ServiceCall>& c) {
  const std::string id = c->StringOr("id", "");
  uint32_t value = 0;
  DownloadItem* item = base::StringToUint(id, &value)
                           ? ManagerOf(*c)->GetDownload(value)
                           : nullptr;
  if (!item) {
    c->InvalidArguments("no download '" + id + "'");
  }
  return item;
}

void List(const std::shared_ptr<ServiceCall>& c) {
  size_t offset = 0;
  if (std::optional<std::string> cursor = c->String("cursor")) {
    if (!cursor->starts_with(kCursorPrefix) ||
        !base::StringToSizeT(cursor->substr(sizeof(kCursorPrefix) - 1),
                             &offset)) {
      c->InvalidArguments("the cursor is not one this call returned");
      return;
    }
  }
  const size_t limit = static_cast<size_t>(std::max(
      1.0, c->Number("limit").value_or(static_cast<double>(kDefaultLimit))));
  const std::string state = c->StringOr("state", "");
  const std::string text = base::ToLowerASCII(c->StringOr("text", ""));
  content::DownloadManager::DownloadVector all;
  ManagerOf(*c)->GetAllDownloads(&all);
  std::vector<DownloadItem*> items;
  for (DownloadItem* item : all) {
    if (!state.empty() && state != StateName(*item)) {
      continue;
    }
    if (!text.empty() &&
        base::ToLowerASCII(item->GetURL().spec() +
                           item->GetTargetFilePath().AsUTF8Unsafe())
                .find(text) == std::string::npos) {
      continue;
    }
    items.push_back(item);
  }
  std::sort(items.begin(), items.end(), [](DownloadItem* a, DownloadItem* b) {
    return a->GetStartTime() > b->GetStartTime();
  });
  std::string out = "{\"items\":[";
  const size_t end = std::min(items.size(), offset + limit);
  for (size_t i = offset; i < end; ++i) {
    out += (i > offset ? "," : "") + DownloadItemJson(*items[i]);
  }
  out += "],\"nextCursor\":";
  out += end < items.size()
             ? JsonString(kCursorPrefix + base::NumberToString(end))
             : "null";
  c->Resolve(out + "}");
}

void Get(const std::shared_ptr<ServiceCall>& c) {
  if (DownloadItem* item = ItemArg(c)) {
    c->Resolve(DownloadItemJson(*item));
  }
}

// Starts a download of the URL; the call is answered with the new item once
// Chromium has created it.
void StartUrl(const std::shared_ptr<ServiceCall>& c,
              const GURL& url,
              const std::string& path) {
  if (!url.is_valid()) {
    c->InvalidArguments("the URL is not valid");
    return;
  }
  constexpr net::NetworkTrafficAnnotationTag kAnnotation =
      net::DefineNetworkTrafficAnnotation("bdk_download", R"(
        semantics {
          sender: "BDK downloads service"
          description:
            "Downloads a URL when the application asks for it through the "
            "downloads.start command."
          trigger: "A call to downloads.start by the application."
          data: "The request for the URL, with the profile's cookies."
          destination: WEBSITE
        }
        policy {
          cookies_allowed: YES
          cookies_store: "The profile's cookie store."
          setting: "None. The application chooses what to download."
          policy_exception_justification: "Not implemented."
        })");
  auto params =
      std::make_unique<download::DownloadUrlParameters>(url, kAnnotation);
  if (!path.empty()) {
    params->set_file_path(base::FilePath::FromUTF8Unsafe(path));
  }
  params->set_callback(base::BindOnce(
      [](std::shared_ptr<ServiceCall> c, DownloadItem* item,
         download::DownloadInterruptReason reason) {
        if (item) {
          c->Resolve(DownloadItemJson(*item));
        } else {
          c->Reject(BDK_DOMAIN_PLATFORM, "downloadFailed",
                    "Chromium did not start the download: " +
                        download::DownloadInterruptReasonToString(reason));
        }
      },
      c));
  ManagerOf(*c)->DownloadUrl(std::move(params));
}

void Start(const std::shared_ptr<ServiceCall>& c) {
  StartUrl(c, GURL(c->StringOr("url", "")), c->StringOr("path", ""));
}

void Pause(const std::shared_ptr<ServiceCall>& c) {
  if (DownloadItem* item = ItemArg(c)) {
    if (item->GetState() != DownloadItem::IN_PROGRESS) {
      c->InvalidArguments("the download is not in progress");
      return;
    }
    item->Pause();
    c->ResolveNull();
  }
}

void Resume(const std::shared_ptr<ServiceCall>& c) {
  if (DownloadItem* item = ItemArg(c)) {
    if (!item->CanResume()) {
      c->InvalidArguments("the download cannot be resumed");
      return;
    }
    item->Resume(/*user_resume=*/true);
    c->ResolveNull();
  }
}

void Cancel(const std::shared_ptr<ServiceCall>& c) {
  if (DownloadItem* item = ItemArg(c)) {
    item->Cancel(/*user_cancel=*/true);
    c->ResolveNull();
  }
}

// A download that can resume resumes; one that cannot starts again from its
// URL.
void Retry(const std::shared_ptr<ServiceCall>& c) {
  DownloadItem* item = ItemArg(c);
  if (!item) {
    return;
  }
  if (item->GetState() == DownloadItem::INTERRUPTED && item->CanResume()) {
    item->Resume(/*user_resume=*/true);
    c->ResolveNull();
    return;
  }
  if (item->GetState() == DownloadItem::IN_PROGRESS ||
      item->GetState() == DownloadItem::COMPLETE) {
    c->InvalidArguments("the download did not fail");
    return;
  }
  StartUrl(c, item->GetURL(), "");
}

void Remove(const std::shared_ptr<ServiceCall>& c) {
  if (DownloadItem* item = ItemArg(c)) {
    item->Remove();
    c->ResolveNull();
  }
}

void DeleteFile(const std::shared_ptr<ServiceCall>& c) {
  if (DownloadItem* item = ItemArg(c)) {
    item->DeleteFile(base::BindOnce(
        [](std::shared_ptr<ServiceCall> c, bool) { c->ResolveNull(); }, c));
  }
}

void Open(const std::shared_ptr<ServiceCall>& c) {
  DownloadItem* item = ItemArg(c);
  if (!item) {
    return;
  }
  if (item->GetState() != DownloadItem::COMPLETE || item->IsDangerous() ||
      !item->CanOpenDownload()) {
    c->Reject(BDK_DOMAIN_BROKER, BDK_CODE_DENIED,
              "this download cannot be opened");
    return;
  }
  item->OpenDownload();
  c->ResolveNull();
}

void Reveal(const std::shared_ptr<ServiceCall>& c) {
  if (DownloadItem* item = ItemArg(c)) {
    item->ShowDownloadInShell();
    c->ResolveNull();
  }
}

void Clear(const std::shared_ptr<ServiceCall>& c) {
  content::DownloadManager::DownloadVector all;
  ManagerOf(*c)->GetAllDownloads(&all);
  std::vector<DownloadItem*> finished;
  for (DownloadItem* item : all) {
    if (item->GetState() != DownloadItem::IN_PROGRESS) {
      finished.push_back(item);
    }
  }
  for (DownloadItem* item : finished) {
    item->Remove();
  }
  c->ResolveNull();
}

}  // namespace

void AddDownloadsHandlers(HandlerTable& table) {
  table["downloads.list"] = &List;
  table["downloads.get"] = &Get;
  table["downloads.start"] = &Start;
  table["downloads.pause"] = &Pause;
  table["downloads.resume"] = &Resume;
  table["downloads.cancel"] = &Cancel;
  table["downloads.retry"] = &Retry;
  table["downloads.remove"] = &Remove;
  table["downloads.deleteFile"] = &DeleteFile;
  table["downloads.open"] = &Open;
  table["downloads.reveal"] = &Reveal;
  table["downloads.clear"] = &Clear;
}

}  // namespace bdk
