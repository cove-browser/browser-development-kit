// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The built-in commands the engine answers with Chromium services
// (services.md section 5). Each service has a file in this directory and adds
// its handlers to the table; command_dispatch.cc finds the handler by name.

#ifndef BDK_ENGINE_SERVICES_SERVICES_H_
#define BDK_ENGINE_SERVICES_SERVICES_H_

#include <map>
#include <memory>
#include <string>

#include "bdk/engine/services/service_call.h"

namespace bookmarks {
class BookmarkModel;
class BookmarkNode;
}  // namespace bookmarks

namespace download {
class DownloadItem;
}

namespace history {
class URLRow;
}

namespace bdk {

// The JSON of one value of the interface's types, shared by the commands that
// answer with it and the events that carry it.
std::string BookmarkNodeJson(const bookmarks::BookmarkModel& model,
                             const bookmarks::BookmarkNode* node);
std::string DownloadItemJson(const download::DownloadItem& item);
std::string HistoryEntryJson(const history::URLRow& row, base::Time last_visit);

using HandlerTable = std::map<std::string, CommandHandler>;

// One function per service file.
void AddHistoryHandlers(HandlerTable& table);
void AddBookmarksHandlers(HandlerTable& table);
void AddDownloadsHandlers(HandlerTable& table);
void AddSettingsHandlers(HandlerTable& table);
void AddPermissionsHandlers(HandlerTable& table);
void AddCookiesHandlers(HandlerTable& table);
void AddPrivacyHandlers(HandlerTable& table);
void AddProfilesHandlers(HandlerTable& table);
void AddSystemHandlers(HandlerTable& table);
void AddExtensionsHandlers(HandlerTable& table);
void AddContentsHandlers(HandlerTable& table);
void AddDebugHandlers(HandlerTable& table);
void AddDevToolsHandlers(HandlerTable& table);
void AddSearchHandlers(HandlerTable& table);

// Backend::CallerRevoked: detaches the caller's DevTools protocol clients from
// every page now, so a quiet client neither keeps a page paused nor holds one
// of the page's session slots (debug_service.cc).
void DetachDebugClientsOf(const bdkcore::Caller& caller);

// Answers a built-in command, on the UI thread. A command with no handler here
// is answered by the core's default: broker.notImplemented.
void ExecuteBuiltinCommand(bdkcore::Runtime& runtime,
                           const std::shared_ptr<bdkcore::BuiltinCall>& call);

// What runtime.info reports about the application (its name and internal
// scheme), read from the resolved configuration when the runtime starts.
void SetRuntimeInfo(const std::string& resolved_json);

// Maps a time range argument ({start?, end?}, both inclusive, milliseconds
// since the epoch) to Chromium's half-open range [begin, end). A missing bound
// is a null Time, which Chromium reads as unbounded.
struct TimeBounds {
  base::Time begin;
  base::Time end;
};
TimeBounds ParseTimeRange(const base::DictValue* range);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_SERVICES_H_
