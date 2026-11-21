// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/pages/bundled_page_source.h"

#include <memory>
#include <optional>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "bdk/core/runtime/routes.h"
#include "bdk/engine/pages/page_broker_js.h"
#include "bdk/engine/pages/page_routes.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "url/gurl.h"

namespace bdk {
namespace {

constexpr char kSdkPrefix[] = "@bdk/internal/";
constexpr char kBindingsPath[] = "@bdk/page_broker.mojom-webui.js";
constexpr char kIndex[] = "index.html";

// BDK's own new-tab page: Chromium's needs Google services
// (configuration.md 4.1). No script, so it needs no exception to the policy.
constexpr char kNewTabHtml[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>New Tab</title></head><body></body></html>";

struct MimeType {
  const char* extension;
  const char* type;
};
constexpr MimeType kMimeTypes[] = {
    {".html", "text/html"},       {".htm", "text/html"},
    {".js", "text/javascript"},   {".mjs", "text/javascript"},
    {".css", "text/css"},         {".json", "application/json"},
    {".map", "application/json"}, {".svg", "image/svg+xml"},
    {".png", "image/png"},        {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},      {".gif", "image/gif"},
    {".webp", "image/webp"},      {".avif", "image/avif"},
    {".ico", "image/x-icon"},     {".woff2", "font/woff2"},
    {".woff", "font/woff"},       {".ttf", "font/ttf"},
    {".otf", "font/otf"},         {".wasm", "application/wasm"},
    {".txt", "text/plain"},
};

// The request's path below the host, decoded, without the leading slash and
// without the query. nullopt when it could leave the page's directory.
std::optional<std::string> RequestPath(const GURL& url) {
  std::string path = base::UnescapeBinaryURLComponent(url.path());
  if (path.empty() || path[0] != '/') {
    return std::nullopt;
  }
  path.erase(0, 1);
  if (path.find('\0') != std::string::npos ||
      path.find('\\') != std::string::npos ||
      base::FilePath(path).ReferencesParent() || base::StartsWith(path, "/")) {
    return std::nullopt;
  }
  return path;
}

bool HasExtension(const std::string& path) {
  size_t slash = path.rfind('/');
  size_t dot = path.rfind('.');
  return dot != std::string::npos &&
         (slash == std::string::npos || dot > slash);
}

// Reads `relative` under `root`, refusing anything outside it after links are
// resolved, a directory, and a file over the limit. With `fallback`, a path
// with no extension and no file reads `root`/index.html. Blocking: runs on the
// thread pool.
scoped_refptr<base::RefCountedMemory> ReadPageFile(base::FilePath root,
                                                   std::string relative,
                                                   bool fallback) {
  const base::FilePath real_root = base::MakeAbsoluteFilePath(root);
  if (real_root.empty()) {
    return nullptr;
  }
  auto read =
      [&](const std::string& path) -> scoped_refptr<base::RefCountedMemory> {
    base::FilePath file = base::MakeAbsoluteFilePath(
        root.AppendASCII(path.empty() ? kIndex : path));
    if (file.empty() || !real_root.IsParent(file) ||
        base::DirectoryExists(file)) {
      return nullptr;
    }
    std::string data;
    if (!base::ReadFileToStringWithMaxSize(
            file, &data, static_cast<size_t>(kMaxPageFileBytes))) {
      return nullptr;
    }
    return base::MakeRefCounted<base::RefCountedString>(std::move(data));
  };
  if (scoped_refptr<base::RefCountedMemory> data = read(relative)) {
    return data;
  }
  if (fallback && !HasExtension(relative)) {
    return read(kIndex);
  }
  return nullptr;
}

scoped_refptr<base::RefCountedMemory> FromString(std::string_view text) {
  return base::MakeRefCounted<base::RefCountedString>(std::string(text));
}

}  // namespace

BundledPageSource::BundledPageSource(std::string_view host) : host_(host) {}

BundledPageSource::~BundledPageSource() = default;

std::string BundledPageSource::GetSource() {
  return host_;
}

void BundledPageSource::StartDataRequest(
    const GURL& url,
    const content::WebContents::Getter& wc_getter,
    GotDataCallback callback) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  bdkcore::BundledSource source;
  std::optional<std::string> path = RequestPath(url);
  if (!rt || !path || !bdkcore::bundled_source(*rt, host_, &source)) {
    std::move(callback).Run(nullptr);
    return;
  }
  if (source.builtin_new_tab) {
    std::move(callback).Run(FromString(kNewTabHtml));
    return;
  }
  if (*path == kBindingsPath) {
    std::move(callback).Run(FromString(kPageBrokerMojomWebUiJs));
    return;
  }
  const base::FilePath resources =
      PagesBundlePath().Append("Contents").Append("Resources");
  base::FilePath root;
  std::string relative;
  bool fallback = false;
  if (base::StartsWith(*path, kSdkPrefix)) {
    root = resources.Append("BDK").Append("js").Append("internal");
    relative = path->substr(sizeof(kSdkPrefix) - 1);
    if (relative.empty()) {
      std::move(callback).Run(nullptr);
      return;
    }
  } else {
    root = resources.AppendASCII(source.path);
    relative = *path;
    fallback = true;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ReadPageFile, root, relative, fallback),
      std::move(callback));
}

std::string BundledPageSource::GetMimeType(const GURL& url) {
  const std::string path = url.path();
  for (const MimeType& m : kMimeTypes) {
    if (base::EndsWith(path, m.extension,
                       base::CompareCase::INSENSITIVE_ASCII)) {
      return m.type;
    }
  }
  // The page itself (`/`, or a client-side route that serves index.html).
  return HasExtension(path) ? "application/octet-stream" : "text/html";
}

bool BundledPageSource::AllowCaching() {
  // A development build rewrites pages while the application runs.
  return false;
}

std::string BundledPageSource::GetContentSecurityPolicy(
    network::mojom::CSPDirectiveName directive) {
  using Name = network::mojom::CSPDirectiveName;
  switch (directive) {
    case Name::DefaultSrc:
      return "default-src 'self';";
    case Name::ScriptSrc:
      return "script-src 'self' chrome://resources;";
    case Name::StyleSrc:
      return "style-src 'self' chrome://resources;";
    case Name::ImgSrc:
      return "img-src 'self' data: chrome://resources chrome://favicon2;";
    case Name::FontSrc:
      return "font-src 'self' chrome://resources;";
    case Name::ConnectSrc:
      return "connect-src 'self';";
    case Name::ObjectSrc:
      return "object-src 'none';";
    case Name::ChildSrc:
      return "child-src 'none';";
    case Name::FrameSrc:
      return "frame-src 'none';";
    case Name::FrameAncestors:
      return "frame-ancestors 'none';";
    case Name::BaseURI:
      return "base-uri 'self';";
    case Name::FormAction:
      return "form-action 'none';";
    case Name::RequireTrustedTypesFor:
      return "require-trusted-types-for 'script';";
    case Name::TrustedTypes:
      return "trusted-types;";
    case Name::WorkerSrc:
      return "worker-src 'self';";
    default:
      return content::URLDataSource::GetContentSecurityPolicy(directive);
  }
}

bool BundledPageSource::ShouldServeMimeTypeAsContentTypeHeader() {
  return true;
}

}  // namespace bdk
