// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// debug.*: protocol sessions on a page (services.md section 5.17). Each
// session is a DevTools protocol client of its own, separate from the engine's
// internal one (page_protocol.h), so what a caller enables or detaches does
// not touch what BDK's commands rely on. The core has already refused the
// commands when [devtools] protocol is false, and the domains that reach
// beyond a page for sessions; the client is also untrusted, so the browser
// target, privileged schemes and local files are closed to it as well.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_view_util.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/core/runtime/automation_internal.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/page_protocol.h"
#include "bdk/engine/services/page_call.h"
#include "bdk/engine/services/services.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"

namespace bdk {

namespace {

// A page with more sessions than this is not a page being debugged but one
// being abused.
constexpr size_t kMaxSessionsPerPage = 8;

// Not in the interface's constants: a protocol method that failed.
constexpr char kProtocolError[] = "protocolError";

class DebugSessions;

// One session: a client attached to the page's agent host, owned by the
// page's DebugSessions.
class DebugClient : public content::DevToolsAgentHostClient {
 public:
  DebugClient(std::string id,
              std::shared_ptr<bdkcore::Caller> owner,
              std::shared_ptr<bdkcore::Contents> contents,
              base::WeakPtr<DebugSessions> sessions)
      : id_(std::move(id)),
        owner_(std::move(owner)),
        contents_(std::move(contents)),
        sessions_(std::move(sessions)) {}

  ~DebugClient() override = default;

  const std::string& id() const { return id_; }
  const std::shared_ptr<bdkcore::Caller>& owner() const { return owner_; }

  bool Attach(content::WebContents* web_contents) {
    host_ = content::DevToolsAgentHost::GetOrCreateFor(web_contents);
    if (!host_->AttachClient(this)) {
      host_.reset();
      return false;
    }
    return true;
  }

  // Sends `method` and answers `call` with its result.
  void Send(const std::shared_ptr<ServiceCall>& call,
            const std::string& method,
            base::DictValue params) {
    if (!host_) {
      call->Reject(BDK_DOMAIN_DEVTOOLS, "detached",
                   "the session is no longer attached");
      return;
    }
    const int id = ++last_id_;
    pending_[id] = call;
    base::DictValue message;
    message.Set("id", id);
    message.Set("method", method);
    message.Set("params", std::move(params));
    const std::string json = ToJson(message);
    scoped_refptr<content::DevToolsAgentHost> host = host_;
    host->DispatchProtocolMessage(this, base::as_byte_span(json));
  }

  // Detaches from the page. `reason` is told to the owner as debug.detached,
  // unless it is empty (the owner asked).
  void Close(const std::string& reason) {
    if (closed_) {
      return;
    }
    closed_ = true;
    if (host_) {
      scoped_refptr<content::DevToolsAgentHost> host = std::move(host_);
      host->DetachClient(this);
    }
    FailPending();
    if (!reason.empty()) {
      Emit("debug.detached", "{\"session\":" + JsonString(id_) +
                                 ",\"reason\":" + JsonString(reason) + "}");
    }
  }

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;
  bool MayAttachToURL(const GURL& url, bool is_webui) override {
    return MayInspectUrl(url, is_webui);
  }
  bool MayAttachToRenderFrameHost(
      content::RenderFrameHost* render_frame_host) override {
    return MayInspectFrame(render_frame_host);
  }
  // A session is an untrusted client: no browser target, no privileged
  // schemes, no local files.
  bool IsTrusted() override { return false; }
  bool MayReadLocalFiles() override { return false; }
  bool MayWriteLocalFiles() override { return false; }

 private:
  // Sends an event of the debug namespace to whoever attached, and no one else.
  void Emit(const std::string& name, const std::string& payload) {
    std::shared_ptr<bdkcore::Runtime> runtime = bdkcore::current_runtime();
    if (!runtime || owner_->revoked) {
      return;
    }
    bdkcore::emit_builtin_event_to(
        runtime, owner_, name,
        contents_->profile ? contents_->profile->handle : 0, contents_->handle,
        payload);
  }

  void FailPending() {
    std::map<int, std::shared_ptr<ServiceCall>> pending = std::move(pending_);
    pending_.clear();
    for (auto& entry : pending) {
      entry.second->Reject(BDK_DOMAIN_DEVTOOLS, "detached",
                           "the session was detached");
    }
  }

  const std::string id_;
  const std::shared_ptr<bdkcore::Caller> owner_;
  const std::shared_ptr<bdkcore::Contents> contents_;
  const base::WeakPtr<DebugSessions> sessions_;
  scoped_refptr<content::DevToolsAgentHost> host_;
  int last_id_ = 0;
  std::map<int, std::shared_ptr<ServiceCall>> pending_;
  bool closed_ = false;
};

// The sessions of one page. Owned by the page's WebContents, so they go with
// it.
class DebugSessions : public content::WebContentsUserData<DebugSessions> {
 public:
  ~DebugSessions() override {
    for (auto& entry : clients_) {
      entry.second->Close("targetClosed");
    }
  }

  base::WeakPtr<DebugSessions> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  size_t size() const { return clients_.size(); }

  DebugClient* Find(const std::string& id) {
    auto it = clients_.find(id);
    return it == clients_.end() ? nullptr : it->second.get();
  }
  void Add(std::unique_ptr<DebugClient> client) {
    clients_[client->id()] = std::move(client);
  }
  // Forgets the session, later: it may be this call's own client that asks.
  void RemoveSoon(const std::string& id) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&DebugSessions::Remove, weak_factory_.GetWeakPtr(), id));
  }
  void Remove(const std::string& id) {
    auto it = clients_.find(id);
    if (it == clients_.end()) {
      return;
    }
    std::unique_ptr<DebugClient> client = std::move(it->second);
    clients_.erase(it);
    client->Close("");
  }

 private:
  friend class content::WebContentsUserData<DebugSessions>;
  explicit DebugSessions(content::WebContents* web_contents)
      : content::WebContentsUserData<DebugSessions>(*web_contents) {}

  std::map<std::string, std::unique_ptr<DebugClient>> clients_;
  base::WeakPtrFactory<DebugSessions> weak_factory_{this};
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(DebugSessions);

void DebugClient::DispatchProtocolMessage(
    content::DevToolsAgentHost* agent_host,
    base::span<const uint8_t> message) {
  // The caller that attached was revoked (its session ended): nothing more is
  // sent to it, and it is detached, here, when its next event arrives.
  if (owner_->revoked) {
    if (sessions_) {
      sessions_->RemoveSoon(id_);
    }
    return;
  }
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(base::as_string_view(message));
  if (!parsed) {
    return;
  }
  if (std::optional<int> id = parsed->FindInt("id")) {
    auto it = pending_.find(*id);
    if (it == pending_.end()) {
      return;
    }
    std::shared_ptr<ServiceCall> call = std::move(it->second);
    pending_.erase(it);
    if (const base::DictValue* error = parsed->FindDict("error")) {
      const std::string* text = error->FindString("message");
      const std::optional<int> code = error->FindInt("code");
      call->Reject(BDK_DOMAIN_DEVTOOLS, kProtocolError,
                   (text ? *text : std::string("the method failed")) +
                       (code ? " (" + base::NumberToString(*code) + ")" : ""));
      return;
    }
    const base::DictValue* result = parsed->FindDict("result");
    call->Resolve("{\"result\":" + (result ? ToJson(*result) : "{}") + "}");
    return;
  }
  const std::string* method = parsed->FindString("method");
  if (!method) {
    return;
  }
  const base::DictValue* params = parsed->FindDict("params");
  Emit("debug.event", "{\"session\":" + JsonString(id_) +
                          ",\"method\":" + JsonString(*method) +
                          ",\"params\":" + (params ? ToJson(*params) : "{}") +
                          "}");
}

void DebugClient::AgentHostClosed(content::DevToolsAgentHost* agent_host) {
  // Chromium already detached the client: the page closed, or went to a page
  // that may not be debugged.
  host_.reset();
  Close("targetClosed");
  if (sessions_) {
    sessions_->RemoveSoon(id_);
  }
}

std::string NewSessionId() {
  static int last = 0;
  return "debug-" + base::NumberToString(++last);
}

void Attach(const std::shared_ptr<ServiceCall>& c) {
  content::WebContents* web_contents = LivePage(c);
  if (!web_contents) {
    return;
  }
  DebugSessions* sessions =
      DebugSessions::GetOrCreateForWebContents(web_contents);
  if (sessions->size() >= kMaxSessionsPerPage) {
    c->Reject(BDK_DOMAIN_BROKER, BDK_CODE_OVERLOADED,
              "this page has too many protocol sessions");
    return;
  }
  auto client = std::make_unique<DebugClient>(
      NewSessionId(), c->builtin().caller, c->builtin().contents,
      sessions->GetWeakPtr());
  if (!client->Attach(web_contents)) {
    c->Reject(BDK_DOMAIN_PLATFORM, "unavailable",
              "Chromium does not allow this page to be debugged");
    return;
  }
  const std::string id = client->id();
  sessions->Add(std::move(client));
  c->Resolve("{\"session\":" + JsonString(id) + "}");
}

// The session the call names, if it belongs to this page and this caller
// (another caller's session is as good as none); otherwise the call is
// rejected.
DebugClient* FindSession(const std::shared_ptr<ServiceCall>& c) {
  content::WebContents* web_contents = LivePage(c);
  if (!web_contents) {
    return nullptr;
  }
  DebugSessions* sessions = DebugSessions::FromWebContents(web_contents);
  DebugClient* client =
      sessions ? sessions->Find(c->StringOr("session", "")) : nullptr;
  if (!client || client->owner() != c->builtin().caller) {
    c->InvalidArguments("there is no such protocol session on this page");
    return nullptr;
  }
  return client;
}

void Send(const std::shared_ptr<ServiceCall>& c) {
  DebugClient* client = FindSession(c);
  if (!client) {
    return;
  }
  base::DictValue params;
  if (const base::Value* value = c->args().Find("params")) {
    // `json?`: any value, but a protocol method takes an object.
    if (value->is_dict()) {
      params = value->GetDict().Clone();
    } else if (!value->is_none()) {
      c->InvalidArguments("params must be an object");
      return;
    }
  }
  client->Send(c, c->StringOr("method", ""), std::move(params));
}

void Detach(const std::shared_ptr<ServiceCall>& c) {
  DebugClient* client = FindSession(c);
  if (!client) {
    return;
  }
  // Gone before the answer: no event follows it.
  DebugSessions::FromWebContents(LivePage(c))->Remove(client->id());
  c->ResolveNull();
}

}  // namespace

void AddDebugHandlers(HandlerTable& table) {
  table["debug.attach"] = &Attach;
  table["debug.send"] = &Send;
  table["debug.detach"] = &Detach;
}

}  // namespace bdk
