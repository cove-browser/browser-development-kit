// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// bookmarks.*: Chromium's BookmarkModel. Node identifiers are Chromium's
// numeric ids written as strings. A private profile reads and writes its
// parent's bookmarks (services.md section 3); the factory redirects it.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/bookmarks/browser/bookmark_client.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_model_load_waiter.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/bookmarks/browser/titled_url_match.h"
#include "components/bookmarks/common/bookmark_metrics.h"
#include "url/gurl.h"

namespace bdk {

namespace {

using bookmarks::BookmarkModel;
using bookmarks::BookmarkNode;

constexpr size_t kDefaultSearchLimit = 100;

}  // namespace

std::string BookmarkNodeJson(const BookmarkModel& model,
                             const BookmarkNode* node) {
  const BookmarkNode* parent = node->parent();
  const bool is_root_child = !parent || model.is_root_node(parent);
  const size_t index =
      parent ? parent->GetIndexOf(node).value_or(0) : static_cast<size_t>(0);
  return "{\"id\":" + JsonString(base::NumberToString(node->id())) +
         ",\"parentId\":" +
         (is_root_child ? std::string("null")
                        : JsonString(base::NumberToString(parent->id()))) +
         ",\"index\":" + base::NumberToString(index) +
         ",\"title\":" + JsonString(base::UTF16ToUTF8(node->GetTitle())) +
         ",\"url\":" +
         (node->is_folder() ? std::string("null")
                            : JsonString(node->url().spec())) +
         ",\"isFolder\":" + (node->is_folder() ? "true" : "false") +
         ",\"dateAdded\":" + JsonTime(node->date_added()) + "}";
}

namespace {

std::string TreeJson(const BookmarkModel& model, const BookmarkNode* node) {
  std::string out =
      "{\"node\":" + BookmarkNodeJson(model, node) + ",\"children\":[";
  bool first = true;
  for (const auto& child : node->children()) {
    out += (first ? "" : ",") + TreeJson(model, child.get());
    first = false;
  }
  return out + "]}";
}

std::string NodesJson(const BookmarkModel& model,
                      const std::vector<const BookmarkNode*>& nodes) {
  std::string out = "{\"items\":[";
  bool first = true;
  for (const BookmarkNode* node : nodes) {
    out += (first ? "" : ",") + BookmarkNodeJson(model, node);
    first = false;
  }
  return out + "]}";
}

using ModelHandler = void (*)(const std::shared_ptr<ServiceCall>&,
                              BookmarkModel&);

// Runs the handler once the profile's bookmark model has loaded.
void WithModel(const std::shared_ptr<ServiceCall>& c, ModelHandler handler) {
  BookmarkModel* model =
      BookmarkModelFactory::GetForBrowserContext(c->original_profile());
  if (!model) {
    c->Reject(BDK_DOMAIN_PLATFORM, "unavailable",
              "bookmarks are not available");
    return;
  }
  if (model->loaded()) {
    handler(c, *model);
    return;
  }
  bookmarks::ScheduleCallbackOnBookmarkModelLoad(
      *model, base::BindOnce(
                  [](std::shared_ptr<ServiceCall> c, ModelHandler handler,
                     base::WeakPtr<BookmarkModel> model) {
                    if (model) {
                      handler(c, *model);
                    }
                  },
                  c, handler, model->AsWeakPtr()));
}

// The node the argument names; null, with the call rejected, if there is none.
const BookmarkNode* NodeArg(const std::shared_ptr<ServiceCall>& c,
                            const BookmarkModel& model,
                            const char* key) {
  const std::string id = c->StringOr(key, "");
  int64_t value = 0;
  const BookmarkNode* node = base::StringToInt64(id, &value)
                                 ? bookmarks::GetBookmarkNodeByID(&model, value)
                                 : nullptr;
  if (!node || model.is_root_node(node)) {
    c->InvalidArguments("no bookmark '" + id + "'");
    return nullptr;
  }
  return node;
}

// A folder the caller may add to or move into; null, with the call rejected,
// if it is not one.
const BookmarkNode* FolderArg(const std::shared_ptr<ServiceCall>& c,
                              BookmarkModel& model,
                              const char* key) {
  const BookmarkNode* folder = NodeArg(c, model, key);
  if (!folder) {
    return nullptr;
  }
  if (!folder->is_folder() || model.client()->IsNodeManaged(folder)) {
    c->InvalidArguments("'" + c->StringOr(key, "") +
                        "' is not a folder that can be changed");
    return nullptr;
  }
  return folder;
}

size_t IndexArg(const ServiceCall& c, const BookmarkNode* parent) {
  const size_t count = parent->children().size();
  std::optional<double> index = c.Number("index");
  if (!index || *index < 0) {
    return count;
  }
  return std::min(static_cast<size_t>(*index), count);
}

// A node the caller may change: not a permanent folder, not managed.
bool Editable(const std::shared_ptr<ServiceCall>& c,
              const BookmarkModel& model,
              const BookmarkNode* node) {
  if (model.is_permanent_node(node) || model.client()->IsNodeManaged(node)) {
    c->InvalidArguments("'" + base::NumberToString(node->id()) +
                        "' cannot be changed");
    return false;
  }
  return true;
}

void Roots(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  c->Resolve("{\"roots\":[" +
             BookmarkNodeJson(model, model.bookmark_bar_node()) + "," +
             BookmarkNodeJson(model, model.other_node()) + "]}");
}

void Get(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  if (const BookmarkNode* node = NodeArg(c, model, "id")) {
    c->Resolve(BookmarkNodeJson(model, node));
  }
}

void Children(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  const BookmarkNode* node = NodeArg(c, model, "id");
  if (!node) {
    return;
  }
  std::vector<const BookmarkNode*> children;
  for (const auto& child : node->children()) {
    children.push_back(child.get());
  }
  c->Resolve(NodesJson(model, children));
}

void Subtree(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  if (const BookmarkNode* node = NodeArg(c, model, "id")) {
    c->Resolve(TreeJson(model, node));
  }
}

void Search(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  const size_t limit = static_cast<size_t>(std::max(
      1.0,
      c->Number("limit").value_or(static_cast<double>(kDefaultSearchLimit))));
  std::vector<const BookmarkNode*> nodes;
  for (const bookmarks::TitledUrlMatch& match : model.GetBookmarksMatching(
           base::UTF8ToUTF16(c->StringOr("query", "")), limit,
           query_parser::MatchingAlgorithm::DEFAULT)) {
    nodes.push_back(static_cast<const BookmarkNode*>(match.node.get()));
  }
  c->Resolve(NodesJson(model, nodes));
}

void ForUrl(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  std::vector<const BookmarkNode*> nodes;
  for (const BookmarkNode* node :
       model.GetNodesByURL(GURL(c->StringOr("url", "")))) {
    nodes.push_back(node);
  }
  c->Resolve(NodesJson(model, nodes));
}

void Create(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  const BookmarkNode* parent = FolderArg(c, model, "parentId");
  if (!parent) {
    return;
  }
  const GURL url(c->StringOr("url", ""));
  if (!url.is_valid()) {
    c->InvalidArguments("the URL is not valid");
    return;
  }
  const BookmarkNode* node =
      model.AddNewURL(parent, IndexArg(*c, parent),
                      base::UTF8ToUTF16(c->StringOr("title", "")), url);
  c->Resolve(BookmarkNodeJson(model, node));
}

void CreateFolder(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  const BookmarkNode* parent = FolderArg(c, model, "parentId");
  if (!parent) {
    return;
  }
  const BookmarkNode* node =
      model.AddFolder(parent, IndexArg(*c, parent),
                      base::UTF8ToUTF16(c->StringOr("title", "")));
  c->Resolve(BookmarkNodeJson(model, node));
}

void Update(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  const BookmarkNode* node = NodeArg(c, model, "id");
  if (!node || !Editable(c, model, node)) {
    return;
  }
  std::optional<std::string> url = c->String("url");
  if (url && (node->is_folder() || !GURL(*url).is_valid())) {
    c->InvalidArguments("a folder has no URL, and the URL must be valid");
    return;
  }
  if (std::optional<std::string> title = c->String("title")) {
    model.SetTitle(node, base::UTF8ToUTF16(*title),
                   metrics::BookmarkEditSource::kOther);
  }
  if (url) {
    model.SetURL(node, GURL(*url), metrics::BookmarkEditSource::kOther);
  }
  c->Resolve(BookmarkNodeJson(model, node));
}

void Move(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  const BookmarkNode* node = NodeArg(c, model, "id");
  if (!node || !Editable(c, model, node)) {
    return;
  }
  const BookmarkNode* parent = FolderArg(c, model, "parentId");
  if (!parent) {
    return;
  }
  if (parent == node || parent->HasAncestor(node)) {
    c->InvalidArguments("a folder cannot be moved into itself");
    return;
  }
  size_t index = IndexArg(*c, parent);
  model.Move(node, parent, index);
  c->Resolve(BookmarkNodeJson(model, node));
}

void Remove(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  const BookmarkNode* node = NodeArg(c, model, "id");
  if (!node || !Editable(c, model, node)) {
    return;
  }
  model.Remove(node, metrics::BookmarkEditSource::kOther, FROM_HERE);
  c->ResolveNull();
}

template <ModelHandler handler>
void Run(const std::shared_ptr<ServiceCall>& c) {
  WithModel(c, handler);
}

}  // namespace

void AddBookmarksHandlers(HandlerTable& table) {
  table["bookmarks.roots"] = &Run<Roots>;
  table["bookmarks.get"] = &Run<Get>;
  table["bookmarks.children"] = &Run<Children>;
  table["bookmarks.subtree"] = &Run<Subtree>;
  table["bookmarks.search"] = &Run<Search>;
  table["bookmarks.forUrl"] = &Run<ForUrl>;
  table["bookmarks.create"] = &Run<Create>;
  table["bookmarks.createFolder"] = &Run<CreateFolder>;
  table["bookmarks.update"] = &Run<Update>;
  table["bookmarks.move"] = &Run<Move>;
  table["bookmarks.remove"] = &Run<Remove>;
}

}  // namespace bdk
