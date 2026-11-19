// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// history.*: Chromium's HistoryService. A private profile reads and deletes in
// its parent's history and records nothing (services.md section 3).

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/contains.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/cancelable_task_tracker.h"
#include "bdk/engine/services/events.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace bdk {

namespace {

constexpr int kDefaultLimit = 100;
constexpr int kMaxLimit = 1000;
constexpr char kCursorPrefix[] = "h:";

base::CancelableTaskTracker& Tracker() {
  static base::NoDestructor<base::CancelableTaskTracker> tracker;
  return *tracker;
}

history::HistoryService* HistoryOf(const ServiceCall& c) {
  return HistoryServiceFactory::GetForProfile(
      c.original_profile(), ServiceAccessType::EXPLICIT_ACCESS);
}

}  // namespace

std::string HistoryEntryJson(const history::URLRow& row,
                             base::Time last_visit) {
  return "{\"url\":" + JsonString(row.url().spec()) +
         ",\"title\":" + JsonString(base::UTF16ToUTF8(row.title())) +
         ",\"lastVisit\":" + JsonTime(last_visit) +
         ",\"visitCount\":" + base::NumberToString(row.visit_count()) +
         ",\"typedCount\":" + base::NumberToString(row.typed_count()) + "}";
}

namespace {

// The cursor is the offset of the next item, written so that nobody takes it
// for anything else.
std::optional<size_t> ParseCursor(const std::string& cursor) {
  size_t offset = 0;
  if (!cursor.starts_with(kCursorPrefix) ||
      !base::StringToSizeT(cursor.substr(sizeof(kCursorPrefix) - 1), &offset)) {
    return std::nullopt;
  }
  return offset;
}

void Search(const std::shared_ptr<ServiceCall>& c) {
  history::HistoryService* history = HistoryOf(*c);
  if (!history) {
    c->Reject(BDK_DOMAIN_PLATFORM, "unavailable", "history is not available");
    return;
  }
  size_t offset = 0;
  if (std::optional<std::string> cursor = c->String("cursor")) {
    std::optional<size_t> parsed = ParseCursor(*cursor);
    if (!parsed) {
      c->InvalidArguments("the cursor is not one this call returned");
      return;
    }
    offset = *parsed;
  }
  const size_t limit = static_cast<size_t>(
      std::clamp(static_cast<int>(c->Number("limit").value_or(kDefaultLimit)),
                 1, kMaxLimit));
  history::QueryOptions options;
  TimeBounds bounds = ParseTimeRange(c->Dict("range"));
  options.begin_time = bounds.begin;
  options.end_time = bounds.end;
  // One more than the page, to know whether there is a next one.
  options.max_count = static_cast<int>(offset + limit + 1);
  options.duplicate_policy = history::QueryOptions::REMOVE_ALL_DUPLICATES;
  history->QueryHistory(
      base::UTF8ToUTF16(c->StringOr("text", "")), options,
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> c, size_t offset, size_t limit,
             history::QueryResults results) {
            std::string out = "{\"items\":[";
            const size_t end = std::min(results.size(), offset + limit);
            for (size_t i = offset; i < end; ++i) {
              out += (i > offset ? "," : "") +
                     HistoryEntryJson(results[i], results[i].visit_time());
            }
            out += "],\"nextCursor\":";
            out += results.size() > end
                       ? JsonString(kCursorPrefix + base::NumberToString(end))
                       : "null";
            out += "}";
            c->Resolve(out);
          },
          c, offset, limit),
      &Tracker());
}

void Visits(const std::shared_ptr<ServiceCall>& c) {
  history::HistoryService* history = HistoryOf(*c);
  if (!history) {
    c->Reject(BDK_DOMAIN_PLATFORM, "unavailable", "history is not available");
    return;
  }
  history->QueryURLAndVisits(
      GURL(c->StringOr("url", "")), history::VisitQuery404sPolicy::kInclude404s,
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> c,
             history::QueryURLAndVisitsResult result) {
            std::string out = "{\"visits\":[";
            if (result.success) {
              bool first = true;
              for (const history::VisitRow& visit : result.visits) {
                out += (first ? "" : ",") + std::string("{\"time\":") +
                       JsonTime(visit.visit_time) + ",\"transition\":" +
                       JsonString(ui::PageTransitionGetCoreTransitionString(
                           visit.transition)) +
                       ",\"referrer\":null}";
                first = false;
              }
            }
            out += "]}";
            c->Resolve(out);
          },
          c),
      &Tracker());
}

void Add(const std::shared_ptr<ServiceCall>& c) {
  if (c->is_private()) {
    // A private profile records no history.
    c->ResolveNull();
    return;
  }
  history::HistoryService* history = HistoryOf(*c);
  if (!history) {
    c->Reject(BDK_DOMAIN_PLATFORM, "unavailable", "history is not available");
    return;
  }
  const std::string url = c->StringOr("url", "");
  const base::Time when = c->Number("time")
                              ? base::Time::FromMillisecondsSinceUnixEpoch(
                                    static_cast<int64_t>(*c->Number("time")))
                              : base::Time::Now();
  const bool typed = c->StringOr("transition", "") == "typed";
  NoteApplicationVisit(GURL(url));
  history->AddPageWithDetails(
      GURL(url), base::UTF8ToUTF16(c->StringOr("title", "")), 1, typed ? 1 : 0,
      when, /*hidden=*/false, history::SOURCE_BROWSED);
  c->ResolveNull();
}

void Expire(const std::shared_ptr<ServiceCall>& c,
            std::set<GURL> urls,
            TimeBounds bounds) {
  history::HistoryService* history = HistoryOf(*c);
  if (!history) {
    c->Reject(BDK_DOMAIN_PLATFORM, "unavailable", "history is not available");
    return;
  }
  history->ExpireHistoryBetween(
      urls, std::nullopt, bounds.begin, bounds.end, /*user_initiated=*/true,
      base::BindOnce([](std::shared_ptr<ServiceCall> c) { c->ResolveNull(); },
                     c),
      &Tracker());
}

void DeleteUrls(const std::shared_ptr<ServiceCall>& c) {
  std::set<GURL> urls;
  if (const base::ListValue* list = c->List("urls")) {
    for (const base::Value& url : *list) {
      if (url.is_string()) {
        urls.insert(GURL(url.GetString()));
      }
    }
  }
  if (urls.empty()) {
    c->ResolveNull();
    return;
  }
  Expire(c, std::move(urls), TimeBounds());
}

void DeleteRange(const std::shared_ptr<ServiceCall>& c) {
  Expire(c, {}, ParseTimeRange(c->Dict("range")));
}

void DeleteAll(const std::shared_ptr<ServiceCall>& c) {
  Expire(c, {}, TimeBounds());
}

}  // namespace

void AddHistoryHandlers(HandlerTable& table) {
  table["history.search"] = &Search;
  table["history.visits"] = &Visits;
  table["history.add"] = &Add;
  table["history.deleteUrls"] = &DeleteUrls;
  table["history.deleteRange"] = &DeleteRange;
  table["history.deleteAll"] = &DeleteAll;
}

}  // namespace bdk
