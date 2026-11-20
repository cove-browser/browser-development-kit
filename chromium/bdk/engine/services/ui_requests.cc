// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/ui_requests.h"

#include <array>
#include <utility>

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "content/public/browser/web_contents.h"

namespace bdk {

EngineContents* FindEngineContents(content::WebContents* contents) {
  for (EngineContents* page : EngineContents::All()) {
    if (page->web_contents() == contents) {
      return page;
    }
  }
  return nullptr;
}

std::shared_ptr<bdkcore::Request> RaiseUiRequest(EngineContents& page,
                                                 uint32_t surface,
                                                 const base::DictValue& payload,
                                                 RequestFinished on_finish) {
  std::shared_ptr<bdkcore::Contents> core = page.Core();
  std::shared_ptr<bdkcore::Runtime> rt = core ? core->rt.lock() : nullptr;
  if (!rt || rt->shutting) {
    return nullptr;
  }
  auto request = std::make_shared<bdkcore::Request>();
  request->surface = surface;
  request->contents = core->handle;
  request->payload = base::WriteJson(payload).value_or("{}");
  request->on_finish = std::move(on_finish);
  // Where the page is in its window; a request with a natural position
  // overrides it.
  const std::array<double, 4> frame = page.host_view().FrameInWindow();
  base::span<double, 4> anchor(request->anchor);
  anchor.copy_from(frame);
  return bdkcore::raise_ui_request(rt, request);
}

bdk_native_handle_t WindowOfPage(EngineContents& page) {
  return page.host_view().Window();
}

bool ArgumentIsTrue(const std::string& arguments, const char* key) {
  std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
      arguments, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  return dict && dict->FindBool(key).value_or(false);
}

std::string ArgumentString(const std::string& arguments, const char* key) {
  std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
      arguments, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  const std::string* value = dict ? dict->FindString(key) : nullptr;
  return value ? *value : std::string();
}

}  // namespace bdk
