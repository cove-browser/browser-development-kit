// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// devtools.open, devtools.close and devtools.state (T-AUT-05,
// interface/devtools.json): the application opening and closing a page's
// DevTools front end itself, as page.devTools.present() and close() do in
// Swift (swift-api.md 14). The core has refused devtools.open when
// [devtools] frontend is "disabled" and checked that a page is named.

#include <memory>
#include <string>

#include "bdk/engine/engine_contents.h"
#include "bdk/engine/identity.h"
#include "bdk/engine/services/service_call.h"
#include "bdk/engine/services/services.h"

namespace bdk {

namespace {

EngineContents* PageOf(const std::shared_ptr<ServiceCall>& call) {
  const std::shared_ptr<bdkcore::Contents>& c = call->builtin().contents;
  if (!c || c->dead || c->closed || !c->data) {
    call->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "the page is gone");
    return nullptr;
  }
  return EngineContents::From(*c);
}

void Open(const std::shared_ptr<ServiceCall>& call) {
  EngineContents* page = PageOf(call);
  if (!page) {
    return;
  }
  if (!DevToolsEnabled()) {
    // [ui.surfaces] devtools = "disabled" has the same effect as the
    // front end being disabled: there is nothing to present.
    call->Reject(BDK_DOMAIN_DEVTOOLS, BDK_CODE_DISABLED,
                 "DevTools are disabled in bdk.toml");
    return;
  }
  // The request it raises decides where the front end goes; the call does
  // not wait for it (devtools.stateChanged reports the outcome).
  page->OpenDevTools();
  call->ResolveNull();
}

void Close(const std::shared_ptr<ServiceCall>& call) {
  if (EngineContents* page = PageOf(call)) {
    page->CloseDevTools();
    call->ResolveNull();
  }
}

void State(const std::shared_ptr<ServiceCall>& call) {
  EngineContents* page = PageOf(call);
  if (!page) {
    return;
  }
  const std::string frontend = page->DevToolsFrontendId();
  call->Resolve(
      std::string("{\"open\":") + (page->DevToolsOpen() ? "true" : "false") +
      ",\"frontend\":" +
      (frontend.empty() ? std::string("null") : JsonString(frontend)) + "}");
}

}  // namespace

void AddDevToolsHandlers(HandlerTable& table) {
  table["devtools.open"] = &Open;
  table["devtools.close"] = &Close;
  table["devtools.state"] = &State;
}

}  // namespace bdk
