// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// privacy.*: clearing browsing data through the profile's BrowsingDataRemover,
// and estimating what a clear would remove.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/cancelable_task_tracker.h"
#include "bdk/engine/services/browsing_data.h"
#include "bdk/engine/services/events.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/browsing_data/chrome_browsing_data_remover_constants.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/download/public/common/download_item.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browsing_data_filter_builder.h"
#include "content/public/browser/browsing_data_remover.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/storage_partition.h"
#include "net/cookies/canonical_cookie.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "url/origin.h"

namespace bdk {

namespace {

using content::BrowsingDataRemover;

// Calls back once, when the remover has finished, and deletes itself.
class RemovalWaiter : public BrowsingDataRemover::Observer {
 public:
  RemovalWaiter(BrowsingDataRemover* remover, base::OnceClosure done)
      : remover_(remover), done_(std::move(done)) {
    remover_->AddObserver(this);
  }

  void OnBrowsingDataRemoverDone(uint64_t failed_data_types) override {
    remover_->RemoveObserver(this);
    std::move(done_).Run();
    delete this;
  }

 private:
  raw_ptr<BrowsingDataRemover> remover_;
  base::OnceClosure done_;
};

uint64_t DataTypesOf(const std::string& type) {
  if (type == "history") {
    return chrome_browsing_data_remover::DATA_TYPE_HISTORY;
  }
  if (type == "cookies") {
    return BrowsingDataRemover::DATA_TYPE_COOKIES |
           BrowsingDataRemover::DATA_TYPE_DOM_STORAGE;
  }
  if (type == "cache") {
    return BrowsingDataRemover::DATA_TYPE_CACHE;
  }
  if (type == "downloads") {
    return BrowsingDataRemover::DATA_TYPE_DOWNLOADS;
  }
  if (type == "passwords") {
    return chrome_browsing_data_remover::DATA_TYPE_PASSWORDS;
  }
  if (type == "autofill") {
    return chrome_browsing_data_remover::DATA_TYPE_FORM_DATA;
  }
  if (type == "siteSettings") {
    return chrome_browsing_data_remover::DATA_TYPE_CONTENT_SETTINGS;
  }
  return 0;  // hostedAppData: a runtime with no hosted apps has none
}

uint64_t MaskOf(const ServiceCall& c) {
  uint64_t mask = 0;
  if (const base::ListValue* types = c.List("types")) {
    for (const base::Value& type : *types) {
      mask |= DataTypesOf(type.GetString());
    }
  }
  return mask;
}

void Clear(const std::shared_ptr<ServiceCall>& c) {
  const uint64_t mask = MaskOf(*c);
  if (mask == 0) {
    c->ResolveNull();
    return;
  }
  TimeBounds bounds = ParseTimeRange(c->Dict("range"));
  std::unique_ptr<content::BrowsingDataFilterBuilder> filter;
  if (const base::ListValue* origins = c->List("origins")) {
    filter = content::BrowsingDataFilterBuilder::Create(
        content::BrowsingDataFilterBuilder::Mode::kDelete);
    for (const base::Value& origin : *origins) {
      filter->AddOrigin(url::Origin::Create(GURL(origin.GetString())));
    }
  }
  c->Progress(0.0, "clearing");
  // A removal that has started cannot be stopped; a call cancelled meanwhile
  // is answered cancelled, and the removal runs to its end.
  RemoveBrowsingData(
      c->profile(), bounds.begin,
      bounds.end.is_null() ? base::Time::Max() : bounds.end, mask,
      std::move(filter),
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> c) {
            c->Progress(1.0, "done");
            std::string types = "[";
            if (const base::ListValue* list = c->List("types")) {
              for (const base::Value& type : *list) {
                types += std::string(types.size() > 1 ? "," : "") +
                         JsonString(type.GetString());
              }
            }
            EmitEvent(c->builtin().profile, "privacy.cleared",
                      "{\"types\":" + types +
                          "],\"range\":null,\"source\":\"application\"}");
            c->ResolveNull();
          },
          c));
}

struct Estimate {
  std::string type;
  int items = 0;
};

void Finish(const std::shared_ptr<ServiceCall>& c,
            std::vector<Estimate> estimates) {
  std::string out = "{\"estimates\":[";
  for (size_t i = 0; i < estimates.size(); ++i) {
    out += std::string(i ? "," : "") +
           "{\"type\":" + JsonString(estimates[i].type) +
           ",\"items\":" + base::NumberToString(estimates[i].items) +
           ",\"bytes\":null}";
  }
  c->Resolve(out + "]}");
}

// Counts what each requested type holds, one type after the other. History,
// cookies and the downloads list are counted; the other types report 0 items.
struct Counting {
  std::shared_ptr<ServiceCall> call;
  std::vector<std::string> pending;  // types still to count, last first
  std::vector<Estimate> done;
};

void CountNext(std::shared_ptr<Counting> counting);

void Counted(std::shared_ptr<Counting> counting, std::string type, int items) {
  counting->done.push_back(Estimate{std::move(type), items});
  CountNext(std::move(counting));
}

void CountNext(std::shared_ptr<Counting> counting) {
  const std::shared_ptr<ServiceCall>& c = counting->call;
  if (counting->pending.empty()) {
    Finish(c, counting->done);
    return;
  }
  const std::string type = counting->pending.back();
  counting->pending.pop_back();
  if (type == "history") {
    history::HistoryService* history = HistoryServiceFactory::GetForProfile(
        c->original_profile(), ServiceAccessType::EXPLICIT_ACCESS);
    if (history) {
      history::QueryOptions options;
      TimeBounds bounds = ParseTimeRange(c->Dict("range"));
      options.begin_time = bounds.begin;
      options.end_time = bounds.end;
      static base::NoDestructor<base::CancelableTaskTracker> tracker;
      history->QueryHistory(
          u"", options,
          base::BindOnce(
              [](std::shared_ptr<Counting> counting, std::string type,
                 history::QueryResults results) {
                Counted(std::move(counting), std::move(type),
                        static_cast<int>(results.size()));
              },
              counting, type),
          tracker.get());
      return;
    }
  } else if (type == "cookies") {
    c->profile()
        ->GetDefaultStoragePartition()
        ->GetCookieManagerForBrowserProcess()
        ->GetAllCookies(base::BindOnce(
            [](std::shared_ptr<Counting> counting, std::string type,
               const std::vector<net::CanonicalCookie>& cookies) {
              Counted(std::move(counting), std::move(type),
                      static_cast<int>(cookies.size()));
            },
            counting, type));
    return;
  } else if (type == "downloads") {
    std::vector<raw_ptr<download::DownloadItem, VectorExperimental>> items;
    c->profile()->GetDownloadManager()->GetAllDownloads(&items);
    Counted(std::move(counting), type, static_cast<int>(items.size()));
    return;
  }
  Counted(std::move(counting), type, 0);
}

void EstimateCommand(const std::shared_ptr<ServiceCall>& c) {
  std::vector<std::string> types;
  if (const base::ListValue* list = c->List("types")) {
    for (const base::Value& type : *list) {
      types.insert(types.begin(), type.GetString());
    }
  }
  auto counting = std::make_shared<Counting>();
  counting->call = c;
  counting->pending = std::move(types);
  CountNext(std::move(counting));
}

}  // namespace

void RemoveBrowsingData(
    Profile* profile,
    base::Time begin,
    base::Time end,
    uint64_t data_types,
    std::unique_ptr<content::BrowsingDataFilterBuilder> filter,
    base::OnceClosure done) {
  BrowsingDataRemover* remover = profile->GetBrowsingDataRemover();
  const uint64_t origins = BrowsingDataRemover::ORIGIN_TYPE_UNPROTECTED_WEB |
                           BrowsingDataRemover::ORIGIN_TYPE_PROTECTED_WEB;
  // The waiter deletes itself when the remover is done.
  auto* waiter = new RemovalWaiter(remover, std::move(done));
  if (filter) {
    remover->RemoveWithFilterAndReply(begin, end, data_types, origins,
                                      std::move(filter), waiter);
  } else {
    remover->RemoveAndReply(begin, end, data_types, origins, waiter);
  }
}

void AddPrivacyHandlers(HandlerTable& table) {
  table["privacy.clear"] = &Clear;
  table["privacy.estimate"] = &EstimateCommand;
}

}  // namespace bdk
