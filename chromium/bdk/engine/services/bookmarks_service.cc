// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// bookmarks.*: Chromium's BookmarkModel. Node identifiers are Chromium's
// numeric ids written as strings. A private profile reads and writes its
// parent's bookmarks (services.md section 3); the factory redirects it.

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequence_bound.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/bookmarks/bookmark_html_writer.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/keep_alive/profile_keep_alive_types.h"
#include "chrome/browser/profiles/keep_alive/scoped_profile_keep_alive.h"
#include "chrome/browser/profiles/profile.h"
#include "components/bookmarks/browser/bookmark_client.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_model_load_waiter.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/bookmarks/browser/titled_url_match.h"
#include "components/bookmarks/common/bookmark_metrics.h"
#include "components/query_parser/query_parser.h"
#include "components/user_data_importer/common/imported_bookmark_entry.h"
#include "components/user_data_importer/content/content_bookmark_parser.h"
#include "components/user_data_importer/utility/bookmark_parser.h"
#include "url/gurl.h"

namespace bdk {

namespace {

using bookmarks::BookmarkModel;
using bookmarks::BookmarkNode;
using user_data_importer::ContentBookmarkParser;

constexpr size_t kDefaultSearchLimit = 100;
constexpr size_t kImportProgressStep = 100;

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
                   bookmarks::metrics::BookmarkEditSource::kOther);
  }
  if (url) {
    model.SetURL(node, GURL(*url),
                 bookmarks::metrics::BookmarkEditSource::kOther);
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
  model.Remove(node, bookmarks::metrics::BookmarkEditSource::kOther, FROM_HERE);
  c->ResolveNull();
}

// ---- import and export (Netscape bookmarks HTML) ----

// The folder under "Other bookmarks" that an import adds to (services.md 5.5).
constexpr char16_t kImportFolderTitle[] = u"Imported bookmarks";

// The child folder of `parent` with the title, made if there is none.
const BookmarkNode* FolderNamed(BookmarkModel& model,
                                const BookmarkNode* parent,
                                const std::u16string& title) {
  for (const auto& child : parent->children()) {
    if (child->is_folder() && child->GetTitle() == title) {
      return child.get();
    }
  }
  return model.AddFolder(parent, parent->children().size(), title);
}

// Adds what the parser read under a new folder of the other bookmarks, keeping
// the file's folders. Returns how many bookmarks (not folders) it added.
int AddImported(
    const std::shared_ptr<ServiceCall>& c,
    BookmarkModel& model,
    const std::vector<user_data_importer::ImportedBookmarkEntry>& entries) {
  const BookmarkNode* root = nullptr;
  int imported = 0;
  size_t done = 0;
  model.BeginExtensiveChanges();
  for (const user_data_importer::ImportedBookmarkEntry& entry : entries) {
    ++done;
    if (!entry.is_folder && !entry.url.is_valid()) {
      continue;
    }
    if (!root) {
      root = model.AddFolder(model.other_node(),
                             model.other_node()->children().size(),
                             kImportFolderTitle);
    }
    const BookmarkNode* parent = root;
    for (const std::u16string& name : entry.path) {
      parent = FolderNamed(model, parent, name);
    }
    if (entry.is_folder) {
      FolderNamed(model, parent, entry.title);
    } else {
      model.AddURL(parent, parent->children().size(), entry.title, entry.url,
                   nullptr,
                   entry.creation_time.is_null()
                       ? std::nullopt
                       : std::optional<base::Time>(entry.creation_time));
      ++imported;
    }
    if (done % kImportProgressStep == 0) {
      c->Progress(0.5 + 0.5 * static_cast<double>(done) / entries.size(),
                  "Imported " + base::NumberToString(imported) + " bookmarks");
    }
  }
  model.EndExtensiveChanges();
  return imported;
}

void OnParsed(
    std::shared_ptr<ServiceCall> c,
    base::WeakPtr<BookmarkModel> model,
    std::unique_ptr<base::SequenceBound<ContentBookmarkParser>> parser,
    user_data_importer::BookmarkParser::BookmarkParsingResult result) {
  using Error = user_data_importer::BookmarkParser::BookmarkParsingError;
  // A call cancelled while the file was parsed has been answered already;
  // nothing it read is added.
  if (!model || c->cancelled()) {
    return;
  }
  if (!result.has_value()) {
    if (result.error() == Error::kFailedToReadFile ||
        result.error() == Error::kParsingFailed) {
      c->InvalidArguments("the file could not be read as bookmarks HTML");
    } else {
      c->Reject(BDK_DOMAIN_PLATFORM, "failed",
                "the bookmarks file could not be parsed");
    }
    return;
  }
  c->Progress(0.5, "Read " + base::NumberToString(result->bookmarks.size()) +
                       " entries");
  const int imported = AddImported(c, *model, result->bookmarks);
  c->Resolve("{\"imported\":" + base::NumberToString(imported) + "}");
}

// Parses the file with Chromium's bookmarks parser, which runs its HTML parser
// in a utility process, as Chromium's own import does. The parser works off
// the UI thread, on a sequence of its own.
void Import(const std::shared_ptr<ServiceCall>& c, BookmarkModel& model) {
  const base::FilePath path =
      base::FilePath::FromUTF8Unsafe(c->StringOr("path", ""));
  if (path.empty()) {
    c->InvalidArguments("the path is empty");
    return;
  }
  c->Progress(0.0, "Reading " + path.BaseName().AsUTF8Unsafe());
  auto parser = std::make_unique<base::SequenceBound<ContentBookmarkParser>>(
      base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()}));
  base::SequenceBound<ContentBookmarkParser>* const raw = parser.get();
  auto reply = base::BindPostTaskToCurrentDefault(
      base::BindOnce(&OnParsed, c, model.AsWeakPtr(), std::move(parser)));
  raw->PostTaskWithThisObject(base::BindOnce(
      [](base::FilePath path,
         user_data_importer::BookmarkParser::BookmarkParsingCallback reply,
         ContentBookmarkParser* parser) {
        parser->Parse(path, std::move(reply));
      },
      path, std::move(reply)));
}

// Profiles with an export under way: the writer drops a second request for a
// profile without calling back.
std::set<const Profile*>& ExportsInFlight() {
  static base::NoDestructor<std::set<const Profile*>> in_flight;
  return *in_flight;
}

void Export(const std::shared_ptr<ServiceCall>& c, BookmarkModel&) {
  const base::FilePath path =
      base::FilePath::FromUTF8Unsafe(c->StringOr("path", ""));
  if (path.empty()) {
    c->InvalidArguments("the path is empty");
    return;
  }
  Profile* profile = c->original_profile();
  if (!ExportsInFlight().insert(profile).second) {
    c->Reject(BDK_DOMAIN_PLATFORM, "busy",
              "another export of these bookmarks is under way");
    return;
  }
  // The writer answers from a best-effort task, which may run after the
  // application closed the profile: the export keeps the profile loaded until
  // then (the call holds it, and Chromium's favicon fetcher points at it),
  // as BDK keeps an open profile (kBrowserWindow).
  auto keep_alive = std::make_unique<ScopedProfileKeepAlive>(
      profile, ProfileKeepAliveOrigin::kBrowserWindow);
  bookmark_html_writer::WriteBookmarks(
      profile, path,
      base::BindPostTaskToCurrentDefault(base::BindOnce(
          [](std::shared_ptr<ServiceCall> c, const Profile* profile,
             std::unique_ptr<ScopedProfileKeepAlive> keep_alive,
             bookmark_html_writer::Result result) {
            ExportsInFlight().erase(profile);
            if (result == bookmark_html_writer::Result::kSuccess) {
              c->ResolveNull();
            } else {
              c->Reject(BDK_DOMAIN_PLATFORM, "failed",
                        "the bookmarks file could not be written");
            }
            // The call goes before the profile can.
            c.reset();
          },
          c, profile, std::move(keep_alive))));
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
  table["bookmarks.import"] = &Run<Import>;
  table["bookmarks.export"] = &Run<Export>;
}

}  // namespace bdk
