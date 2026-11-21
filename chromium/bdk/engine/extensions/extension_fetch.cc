// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_fetch.h"

#include <map>
#include <memory>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "url/gurl.h"

namespace bdk {

namespace {

struct Served {
  std::string body;
  base::FilePath file;
};

// Written and read on the UI thread.
std::map<std::string, Served>& ServedByUrl() {
  static base::NoDestructor<std::map<std::string, Served>> served;
  return *served;
}

net::NetworkTrafficAnnotationTag Annotation() {
  return net::DefineNetworkTrafficAnnotation("bdk_extension_fetch", R"(
    semantics {
      sender: "BDK extension installation"
      description:
        "Downloads an extension's update manifest and package when the "
        "application installs or updates an extension from an update URL or "
        "a store it has enabled."
      trigger:
        "The application installs an extension from an update URL or a named "
        "store, or checks an installed extension for updates."
      data: "The extension identifier and the browser version."
      destination: OTHER
      destination_other: "The update URL or store the application configured"
    }
    policy {
      cookies_allowed: NO
      setting:
        "The application's bdk.toml: [extensions.install] update_urls and "
        "stores."
      policy_exception_justification: "Not implemented."
    })");
}

std::unique_ptr<network::ResourceRequest> RequestFor(const GURL& url) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  return request;
}

scoped_refptr<network::SharedURLLoaderFactory> FactoryOf(Profile* profile) {
  return profile->GetDefaultStoragePartition()
      ->GetURLLoaderFactoryForBrowserProcess();
}

void FinishString(std::unique_ptr<network::SimpleURLLoader> loader,
                  scoped_refptr<network::SharedURLLoaderFactory> factory,
                  FetchStringCallback callback,
                  std::optional<std::string> body) {
  std::move(callback).Run(std::move(body));
}

void FinishFile(std::unique_ptr<network::SimpleURLLoader> loader,
                scoped_refptr<network::SharedURLLoaderFactory> factory,
                FetchFileCallback callback,
                base::FilePath path) {
  std::move(callback).Run(std::move(path));
}

base::FilePath CopyToTemporaryFile(base::FilePath source) {
  base::FilePath temporary;
  if (!base::CreateTemporaryFile(&temporary)) {
    return base::FilePath();
  }
  if (!base::CopyFile(source, temporary)) {
    base::DeleteFile(temporary);
    return base::FilePath();
  }
  return temporary;
}

base::FilePath WriteTemporaryFile(std::string body) {
  base::FilePath temporary;
  if (!base::CreateTemporaryFile(&temporary)) {
    return base::FilePath();
  }
  if (!base::WriteFile(temporary, body)) {
    base::DeleteFile(temporary);
    return base::FilePath();
  }
  return temporary;
}

}  // namespace

void FetchString(Profile* profile,
                 const GURL& url,
                 size_t max_size,
                 FetchStringCallback callback) {
  auto served = ServedByUrl().find(url.spec());
  if (served != ServedByUrl().end()) {
    std::optional<std::string> body = served->second.body;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](FetchStringCallback callback, std::optional<std::string> body) {
              std::move(callback).Run(std::move(body));
            },
            std::move(callback), std::move(body)));
    return;
  }
  scoped_refptr<network::SharedURLLoaderFactory> factory = FactoryOf(profile);
  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(RequestFor(url), Annotation());
  network::SimpleURLLoader* raw = loader.get();
  raw->DownloadToString(factory.get(),
                        base::BindOnce(&FinishString, std::move(loader),
                                       factory, std::move(callback)),
                        max_size);
}

void FetchToFile(Profile* profile,
                 const GURL& url,
                 FetchFileCallback callback) {
  auto served = ServedByUrl().find(url.spec());
  if (served != ServedByUrl().end()) {
    const Served& entry = served->second;
    if (!entry.file.empty()) {
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock()},
          base::BindOnce(&CopyToTemporaryFile, entry.file),
          std::move(callback));
    } else {
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock()},
          base::BindOnce(&WriteTemporaryFile, entry.body), std::move(callback));
    }
    return;
  }
  scoped_refptr<network::SharedURLLoaderFactory> factory = FactoryOf(profile);
  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(RequestFor(url), Annotation());
  network::SimpleURLLoader* raw = loader.get();
  raw->DownloadToTempFile(
      factory.get(), base::BindOnce(&FinishFile, std::move(loader), factory,
                                    std::move(callback)));
}

void ServeFetchForTesting(const std::string& url,
                          const std::string& body,
                          const base::FilePath& file) {
  const std::string key = GURL(url).spec();
  if (body.empty() && file.empty()) {
    ServedByUrl().erase(key);
    return;
  }
  ServedByUrl()[key] = Served{body, file};
}

}  // namespace bdk
