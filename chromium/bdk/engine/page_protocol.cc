// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/page_protocol.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "base/barrier_closure.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_view_util.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/engine/json_util.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/common/url_constants.h"
#include "url/gurl.h"

namespace bdk {

namespace {

// Chrome's extension scheme (extensions::kExtensionScheme), written out so the
// engine does not depend on //extensions for one string.
constexpr char kExtensionScheme[] = "chrome-extension";

std::string& InternalScheme() {
  static base::NoDestructor<std::string> scheme("bdk");
  return *scheme;
}

ProtocolResult Failure(const std::string& message) {
  ProtocolResult result;
  result.error_message = message;
  return result;
}

// Runs a barrier's closure when a command, answered or failed, is done.
void CountDown(base::RepeatingClosure barrier, ProtocolResult) {
  barrier.Run();
}

// The value of a quad's corner as the protocol writes it: an array of eight
// numbers, x and y of each corner.
std::optional<gfx::PointF> FirstCorner(const base::DictValue& box,
                                       const char* key) {
  const base::ListValue* quad = box.FindList(key);
  if (!quad || quad->size() < 2) {
    return std::nullopt;
  }
  std::optional<double> x = (*quad)[0].GetIfDouble();
  std::optional<double> y = (*quad)[1].GetIfDouble();
  if (!x || !y) {
    return std::nullopt;
  }
  return gfx::PointF(static_cast<float>(*x), static_cast<float>(*y));
}

}  // namespace

void SetPageProtocolRuntimeInfo(const std::string& resolved_json) {
  std::optional<base::DictValue> resolved = ParseJsonDict(resolved_json);
  if (!resolved) {
    return;
  }
  if (const std::string* scheme =
          resolved->FindStringByDottedPath("browser.internal_scheme")) {
    if (!scheme->empty()) {
      InternalScheme() = *scheme;
    }
  }
}

bool MayInspectUrl(const GURL& url, bool is_webui) {
  if (is_webui) {
    return false;
  }
  return !(url.SchemeIs(content::kChromeUIScheme) ||
           url.SchemeIs(content::kChromeUIUntrustedScheme) ||
           url.SchemeIs(content::kChromeDevToolsScheme) ||
           url.SchemeIs(kExtensionScheme) || url.SchemeIs(InternalScheme()));
}

bool MayInspectFrame(content::RenderFrameHost* frame) {
  return MayInspectUrl(frame->GetLastCommittedURL(),
                       frame->GetWebUI() != nullptr);
}

ProtocolResult::ProtocolResult() = default;
ProtocolResult::ProtocolResult(ProtocolResult&&) = default;
ProtocolResult& ProtocolResult::operator=(ProtocolResult&&) = default;
ProtocolResult::~ProtocolResult() = default;

EvaluateResult::EvaluateResult() = default;
EvaluateResult::EvaluateResult(EvaluateResult&&) = default;
EvaluateResult& EvaluateResult::operator=(EvaluateResult&&) = default;
EvaluateResult::~EvaluateResult() = default;

// static
World World::Main() {
  return World();
}

// static
World World::Isolated(std::string name) {
  World world;
  world.main = false;
  world.name = std::move(name);
  return world;
}

struct PageProtocol::PendingCall {
  std::string session_id;
  ProtocolCallback callback;
};

// The frame's inspector style sheet and the pieces of CSS that were added to
// it, in order. The protocol hands out one sheet per document, so a second
// insertion must write the first one's CSS again.
struct PageProtocol::Sheet {
  std::string session_id;
  std::string sheet_id;
  std::vector<std::pair<std::string, std::string>> chunks;
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(PageProtocol);

PageProtocol::PageProtocol(content::WebContents* web_contents)
    : content::WebContentsUserData<PageProtocol>(*web_contents),
      content::WebContentsObserver(web_contents) {}

PageProtocol::~PageProtocol() {
  if (host_) {
    scoped_refptr<content::DevToolsAgentHost> host = std::move(host_);
    host->DetachClient(this);
  }
  FailPending("", "the page was closed");
}

// static
PageProtocol* PageProtocol::For(content::WebContents* web_contents) {
  return PageProtocol::GetOrCreateForWebContents(web_contents);
}

// static
PageProtocol* PageProtocol::Existing(content::WebContents* web_contents) {
  return PageProtocol::FromWebContents(web_contents);
}

void PageProtocol::AddSessionObserver(SessionObserver* observer) {
  observers_.AddObserver(observer);
}

void PageProtocol::RemoveSessionObserver(SessionObserver* observer) {
  observers_.RemoveObserver(observer);
}

void PageProtocol::WhenReady(ReadyCallback ready) {
  waiters_.push_back(std::move(ready));
  // Later, so that an answer that is already known is not delivered from
  // inside the call that asked.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<PageProtocol> self) {
                       if (!self) {
                         return;
                       }
                       if (self->ready_) {
                         self->FinishAttach(Availability::kReady);
                       } else {
                         self->TryAttach();
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

void PageProtocol::KeepAttached() {
  keep_attached_ = true;
  if (!ready_ && !attaching_) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&PageProtocol::TryAttach, weak_factory_.GetWeakPtr()));
  }
}

void PageProtocol::TryAttach() {
  if (ready_ || attaching_) {
    return;
  }
  content::WebContents* web_contents = &GetWebContents();
  content::RenderFrameHost* main = web_contents->GetPrimaryMainFrame();
  if (!main || !MayInspectFrame(main)) {
    FinishAttach(Availability::kInternalPage);
    return;
  }
  attaching_ = true;
  host_ = content::DevToolsAgentHost::GetOrCreateFor(web_contents);
  if (!host_->AttachClient(this)) {
    host_.reset();
    attaching_ = false;
    FinishAttach(Availability::kUnavailable);
    return;
  }
  SetUpSession("", base::BindOnce(
                       [](base::WeakPtr<PageProtocol> self) {
                         if (!self) {
                           return;
                         }
                         // The client was detached while it was being set up.
                         self->FinishAttach(self->host_
                                                ? Availability::kReady
                                                : Availability::kUnavailable);
                       },
                       weak_factory_.GetWeakPtr()));
}

void PageProtocol::FinishAttach(Availability availability) {
  attaching_ = false;
  if (availability == Availability::kReady && !ready_) {
    ready_ = true;
    OnSessionReady("");
  }
  std::vector<ReadyCallback> waiters = std::move(waiters_);
  waiters_.clear();
  for (ReadyCallback& waiter : waiters) {
    std::move(waiter).Run(availability);
  }
}

void PageProtocol::SetUpSession(const std::string& session_id,
                                base::OnceClosure done) {
  base::RepeatingClosure barrier = base::BarrierClosure(4, std::move(done));
  Send(session_id, "Page.enable", base::DictValue(),
       base::BindOnce(&CountDown, barrier));
  Send(session_id, "Runtime.enable", base::DictValue(),
       base::BindOnce(&CountDown, barrier));
  base::DictValue auto_attach;
  auto_attach.Set("autoAttach", true);
  auto_attach.Set("waitForDebuggerOnStart", false);
  auto_attach.Set("flatten", true);
  Send(session_id, "Target.setAutoAttach", std::move(auto_attach),
       base::BindOnce(&CountDown, barrier));
  // Network idle and user scripts' CSS follow the page's load, in every
  // session its frames are in.
  base::DictValue lifecycle;
  lifecycle.Set("enabled", true);
  Send(session_id, "Page.setLifecycleEventsEnabled", std::move(lifecycle),
       base::BindOnce(&CountDown, barrier));
}

void PageProtocol::OnSessionReady(const std::string& session_id) {
  ready_sessions_.insert(session_id);
  for (SessionObserver& observer : observers_) {
    observer.OnSessionAttached(session_id);
  }
}

void PageProtocol::RemoveSession(const std::string& session_id) {
  const bool was_ready = ready_sessions_.erase(session_id) > 0;
  dom_sessions_.erase(session_id);
  css_sessions_.erase(session_id);
  contexts_.erase(session_id);
  if (auto it = target_by_session_.find(session_id);
      it != target_by_session_.end()) {
    session_by_target_.erase(it->second);
    target_by_session_.erase(it);
  }
  for (auto it = sheets_.begin(); it != sheets_.end();) {
    it = it->second->session_id == session_id ? sheets_.erase(it)
                                              : std::next(it);
  }
  FailPending(session_id, "the frame went away");
  if (was_ready) {
    for (SessionObserver& observer : observers_) {
      observer.OnSessionDetached(session_id);
    }
  }
}

void PageProtocol::FailPending(const std::string& session_id,
                               const std::string& message) {
  std::vector<ProtocolCallback> failed;
  for (auto it = pending_.begin(); it != pending_.end();) {
    // An empty id means every session: the page itself went away.
    if (session_id.empty() || it->second->session_id == session_id) {
      failed.push_back(std::move(it->second->callback));
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  for (ProtocolCallback& callback : failed) {
    if (callback) {
      std::move(callback).Run(Failure(message));
    }
  }
}

std::vector<std::string> PageProtocol::Sessions() const {
  return std::vector<std::string>(ready_sessions_.begin(),
                                  ready_sessions_.end());
}

void PageProtocol::Send(const std::string& session_id,
                        const std::string& method,
                        base::DictValue params,
                        ProtocolCallback callback) {
  if (!host_) {
    if (callback) {
      std::move(callback).Run(
          Failure("the page is not attached to the DevTools protocol"));
    }
    return;
  }
  const int id = ++last_id_;
  auto call = std::make_unique<PendingCall>();
  call->session_id = session_id;
  call->callback = std::move(callback);
  pending_[id] = std::move(call);

  base::DictValue message;
  message.Set("id", id);
  message.Set("method", method);
  message.Set("params", std::move(params));
  if (!session_id.empty()) {
    message.Set("sessionId", session_id);
  }
  const std::string json = ToJson(message);
  // The host may answer before this returns, and may close.
  scoped_refptr<content::DevToolsAgentHost> host = host_;
  host->DispatchProtocolMessage(this, base::as_byte_span(json));
}

void PageProtocol::DispatchProtocolMessage(
    content::DevToolsAgentHost* agent_host,
    base::span<const uint8_t> message) {
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
    ProtocolCallback callback = std::move(it->second->callback);
    pending_.erase(it);
    if (!callback) {
      return;
    }
    ProtocolResult result;
    if (const base::DictValue* error = parsed->FindDict("error")) {
      result.error_code = error->FindInt("code").value_or(0);
      if (const std::string* text = error->FindString("message")) {
        result.error_message = *text;
      }
    } else {
      result.ok = true;
      std::optional<base::Value> value = parsed->Extract("result");
      if (value && value->is_dict()) {
        result.result = std::move(*value).TakeDict();
      }
    }
    std::move(callback).Run(std::move(result));
    return;
  }
  const std::string* method = parsed->FindString("method");
  if (!method) {
    return;
  }
  const std::string* session = parsed->FindString("sessionId");
  const base::DictValue* params = parsed->FindDict("params");
  const base::DictValue empty;
  HandleEvent(session ? *session : std::string(), *method,
              params ? *params : empty);
}

void PageProtocol::HandleEvent(const std::string& session_id,
                               const std::string& method,
                               const base::DictValue& params) {
  if (method == "Target.attachedToTarget") {
    const std::string* child = params.FindString("sessionId");
    const base::DictValue* info = params.FindDict("targetInfo");
    if (!child || !info) {
      return;
    }
    const std::string* type = info->FindString("type");
    const std::string* target = info->FindString("targetId");
    if (!type || *type != content::DevToolsAgentHost::kTypeFrame || !target) {
      // Workers and the like are of no use here, and cost the page.
      base::DictValue detach;
      detach.Set("sessionId", *child);
      Send(session_id, "Target.detachFromTarget", std::move(detach),
           ProtocolCallback());
      return;
    }
    session_by_target_[*target] = *child;
    target_by_session_[*child] = *target;
    const std::string child_id = *child;
    SetUpSession(child_id,
                 base::BindOnce(
                     [](base::WeakPtr<PageProtocol> self, std::string child) {
                       // The session may have been detached while it was set
                       // up.
                       if (self && self->target_by_session_.contains(child)) {
                         self->OnSessionReady(child);
                       }
                     },
                     weak_factory_.GetWeakPtr(), child_id));
  } else if (method == "Target.detachedFromTarget") {
    if (const std::string* child = params.FindString("sessionId")) {
      RemoveSession(*child);
    }
  } else if (method == "Runtime.executionContextCreated") {
    if (const base::DictValue* context = params.FindDict("context")) {
      OnContextCreated(session_id, *context);
    }
  } else if (method == "Runtime.executionContextDestroyed") {
    if (std::optional<int> id = params.FindInt("executionContextId")) {
      std::vector<Context>& contexts = contexts_[session_id];
      std::erase_if(contexts, [&](const Context& c) { return c.id == *id; });
    }
  } else if (method == "Runtime.executionContextsCleared") {
    contexts_.erase(session_id);
  } else if (method == "Page.lifecycleEvent") {
    const std::string* frame = params.FindString("frameId");
    const std::string* loader = params.FindString("loaderId");
    const std::string* name = params.FindString("name");
    if (!frame || !name) {
      return;
    }
    content::RenderFrameHost* main = GetWebContents().GetPrimaryMainFrame();
    if (main && *frame == FrameId(main)) {
      lifecycle_seen_ = true;
      if (*name == "init") {
        network_idle_ = false;
      } else if (*name == "networkIdle") {
        network_idle_ = true;
      }
    }
    for (SessionObserver& observer : observers_) {
      observer.OnFrameLifecycle(session_id, *frame,
                                loader ? *loader : std::string(), *name);
    }
  } else if (method == "Page.frameNavigated") {
    // A new document has none of the old one's style sheets.
    if (const base::DictValue* frame = params.FindDict("frame")) {
      if (const std::string* id = frame->FindString("id")) {
        sheets_.erase(*id);
      }
    }
  } else if (method == "CSS.styleSheetRemoved") {
    if (const std::string* id = params.FindString("styleSheetId")) {
      for (auto it = sheets_.begin(); it != sheets_.end();) {
        it = (it->second->session_id == session_id &&
              it->second->sheet_id == *id)
                 ? sheets_.erase(it)
                 : std::next(it);
      }
    }
  }
}

void PageProtocol::OnContextCreated(const std::string& session_id,
                                    const base::DictValue& context) {
  std::optional<int> id = context.FindInt("id");
  const base::DictValue* aux = context.FindDict("auxData");
  if (!id || !aux) {
    return;
  }
  const std::string* frame = aux->FindString("frameId");
  if (!frame) {
    return;
  }
  Context entry;
  entry.id = *id;
  entry.frame_id = *frame;
  entry.is_default = aux->FindBool("isDefault").value_or(false);
  if (const std::string* name = context.FindString("name")) {
    entry.name = *name;
  }
  contexts_[session_id].push_back(std::move(entry));
}

void PageProtocol::AgentHostClosed(content::DevToolsAgentHost* agent_host) {
  // Chromium already detached the client (a navigation to an internal page,
  // a policy, the page's destruction).
  scoped_refptr<content::DevToolsAgentHost> keep = std::move(host_);
  const bool was_attaching = attaching_;
  ready_ = false;
  attaching_ = false;
  network_idle_ = false;
  lifecycle_seen_ = false;
  const std::vector<std::string> children = [&] {
    std::vector<std::string> ids;
    for (const auto& entry : target_by_session_) {
      ids.push_back(entry.first);
    }
    return ids;
  }();
  for (const std::string& child : children) {
    RemoveSession(child);
  }
  RemoveSession("");
  FailPending("", "the page's DevTools target closed");
  sheets_.clear();
  frame_of_css_.clear();
  if (was_attaching || !waiters_.empty()) {
    FinishAttach(Availability::kUnavailable);
  }
}

bool PageProtocol::MayAttachToURL(const GURL& url, bool is_webui) {
  return MayInspectUrl(url, is_webui);
}

bool PageProtocol::MayAttachToRenderFrameHost(
    content::RenderFrameHost* render_frame_host) {
  return MayInspectFrame(render_frame_host);
}

bool PageProtocol::IsTrusted() {
  return false;
}

bool PageProtocol::MayReadLocalFiles() {
  return false;
}

bool PageProtocol::MayWriteLocalFiles() {
  return false;
}

void PageProtocol::DidFinishNavigation(content::NavigationHandle* handle) {
  // A visit to an internal page detached the client; the next page that may
  // be inspected attaches it again.
  if (keep_attached_ && !host_ && handle->IsInPrimaryMainFrame() &&
      handle->HasCommitted() && !handle->IsSameDocument()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&PageProtocol::TryAttach, weak_factory_.GetWeakPtr()));
  }
}

// static
std::string PageProtocol::FrameId(content::RenderFrameHost* frame) {
  return frame->GetDevToolsFrameToken().ToString();
}

content::RenderFrameHost* PageProtocol::FindFrame(const std::string& frame_id) {
  content::RenderFrameHost* main = GetWebContents().GetPrimaryMainFrame();
  if (!main || frame_id.empty()) {
    return main;
  }
  content::RenderFrameHost* found = nullptr;
  main->ForEachRenderFrameHostWithAction([&](content::RenderFrameHost* frame) {
    if (FrameId(frame) == frame_id) {
      found = frame;
      return content::RenderFrameHost::FrameIterationAction::kStop;
    }
    return content::RenderFrameHost::FrameIterationAction::kContinue;
  });
  return found;
}

content::RenderFrameHost* PageProtocol::LocalRootOf(
    content::RenderFrameHost* frame) {
  for (content::RenderFrameHost* f = frame; f; f = f->GetParent()) {
    if (!f->GetParent() || session_by_target_.contains(FrameId(f))) {
      return f;
    }
  }
  return nullptr;
}

std::optional<std::string> PageProtocol::SessionOf(
    content::RenderFrameHost* frame) {
  content::RenderFrameHost* root = LocalRootOf(frame);
  if (!root) {
    return std::nullopt;
  }
  if (root == GetWebContents().GetPrimaryMainFrame()) {
    return std::string();
  }
  auto it = session_by_target_.find(FrameId(root));
  if (it == session_by_target_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void PageProtocol::SendToFrame(const std::string& frame_id,
                               const std::string& method,
                               base::DictValue params,
                               ProtocolCallback callback) {
  content::RenderFrameHost* frame = FindFrame(frame_id);
  if (!frame) {
    if (callback) {
      std::move(callback).Run(Failure("there is no frame with that id"));
    }
    return;
  }
  std::optional<std::string> session = SessionOf(frame);
  if (!session) {
    if (callback) {
      std::move(callback).Run(
          Failure("the frame's DevTools session is not ready yet"));
    }
    return;
  }
  Send(*session, method, std::move(params), std::move(callback));
}

std::optional<int> PageProtocol::MainContext(
    const std::string& session_id,
    const std::string& frame_id) const {
  auto it = contexts_.find(session_id);
  if (it == contexts_.end()) {
    return std::nullopt;
  }
  for (const Context& context : it->second) {
    if (context.is_default && context.frame_id == frame_id) {
      return context.id;
    }
  }
  return std::nullopt;
}

void PageProtocol::EnsureWorld(
    const std::string& frame_id,
    const std::string& name,
    base::OnceCallback<void(std::optional<int>, std::string)> callback) {
  content::RenderFrameHost* frame = FindFrame(frame_id);
  std::optional<std::string> session =
      frame ? SessionOf(frame) : std::optional<std::string>();
  if (!session) {
    std::move(callback).Run(std::nullopt, "there is no such frame");
    return;
  }
  const std::string real_id = FrameId(frame);
  auto it = contexts_.find(*session);
  if (it != contexts_.end()) {
    for (const Context& context : it->second) {
      if (!context.is_default && context.frame_id == real_id &&
          context.name == name) {
        std::move(callback).Run(context.id, "");
        return;
      }
    }
  }
  base::DictValue params;
  params.Set("frameId", real_id);
  params.Set("worldName", name);
  Send(*session, "Page.createIsolatedWorld", std::move(params),
       base::BindOnce(
           [](base::OnceCallback<void(std::optional<int>, std::string)> done,
              ProtocolResult result) {
             std::optional<int> id =
                 result.ok ? result.result.FindInt("executionContextId")
                           : std::nullopt;
             std::move(done).Run(id, id ? "" : result.error_message);
           },
           std::move(callback)));
}

namespace {

// Runtime.evaluate's answer as contents.evaluate reports it.
EvaluateResult MapEvaluate(ProtocolResult reply) {
  EvaluateResult out;
  if (!reply.ok) {
    out.message = reply.error_message;
    // V8 answers a result it cannot serialise as a protocol error.
    if (reply.error_message.find("returned by value") != std::string::npos ||
        reply.error_message.find("chain is too long") != std::string::npos) {
      out.status = EvaluateResult::Status::kNotSerializable;
    }
    return out;
  }
  if (const base::DictValue* exception =
          reply.result.FindDict("exceptionDetails")) {
    out.status = EvaluateResult::Status::kException;
    const base::DictValue* thrown = exception->FindDict("exception");
    const std::string* description =
        thrown ? thrown->FindString("description") : nullptr;
    if (description) {
      out.message = *description;
    } else if (const std::string* text = exception->FindString("text")) {
      out.message = *text;
    }
    return out;
  }
  const base::DictValue* object = reply.result.FindDict("result");
  if (!object) {
    out.message = "the page answered without a result";
    return out;
  }
  if (const std::string* unserializable =
          object->FindString("unserializableValue")) {
    out.status = EvaluateResult::Status::kNotSerializable;
    out.message = "the value (" + *unserializable + ") cannot be sent as JSON";
    return out;
  }
  const std::string* type = object->FindString("type");
  if (type && *type == "undefined") {
    out.status = EvaluateResult::Status::kValue;
    return out;
  }
  if (const base::Value* value = object->Find("value")) {
    out.status = EvaluateResult::Status::kValue;
    out.value = value->Clone();
    return out;
  }
  out.status = EvaluateResult::Status::kNotSerializable;
  out.message = "the value is a " + (type ? *type : std::string("value")) +
                " and cannot be returned by value";
  return out;
}

}  // namespace

void PageProtocol::Evaluate(const std::string& frame_id,
                            const World& world,
                            const std::string& expression,
                            bool user_gesture,
                            EvaluateCallback callback) {
  EvaluateIn(frame_id, world, expression, user_gesture, /*retry=*/true,
             std::move(callback));
}

void PageProtocol::EvaluateIn(const std::string& frame_id,
                              const World& world,
                              const std::string& expression,
                              bool user_gesture,
                              bool retry,
                              EvaluateCallback callback) {
  content::RenderFrameHost* frame = FindFrame(frame_id);
  std::optional<std::string> session =
      frame ? SessionOf(frame) : std::optional<std::string>();
  if (!session) {
    EvaluateResult failed;
    failed.message = frame ? "the frame's DevTools session is not ready yet"
                           : "there is no frame with that id";
    std::move(callback).Run(std::move(failed));
    return;
  }
  const std::string real_id = FrameId(frame);
  const bool is_local_root = LocalRootOf(frame) == frame;

  // Sends the evaluation once the context is known (empty id: the session's
  // own main frame, which Runtime.evaluate defaults to).
  auto run = [](base::WeakPtr<PageProtocol> self, std::string session,
                std::string real_id, World world, std::string expression,
                bool user_gesture, bool retry, EvaluateCallback callback,
                std::optional<int> context, std::string error) {
    if (!self) {
      EvaluateResult failed;
      failed.message = "the page was closed";
      std::move(callback).Run(std::move(failed));
      return;
    }
    if (!context && !error.empty()) {
      EvaluateResult failed;
      failed.message = error;
      std::move(callback).Run(std::move(failed));
      return;
    }
    base::DictValue params;
    params.Set("expression", expression);
    params.Set("returnByValue", true);
    params.Set("awaitPromise", true);
    params.Set("userGesture", user_gesture);
    if (context) {
      params.Set("contextId", *context);
    }
    self->Send(
        session, "Runtime.evaluate", std::move(params),
        base::BindOnce(
            [](base::WeakPtr<PageProtocol> self, std::string session,
               std::string real_id, World world, std::string expression,
               bool user_gesture, bool retry, EvaluateCallback callback,
               ProtocolResult reply) {
              // A navigation destroyed the context a cached id named: forget
              // it and look again, once.
              if (self && retry && !reply.ok &&
                  reply.error_message.find("Cannot find context") !=
                      std::string::npos) {
                std::vector<Context>& contexts = self->contexts_[session];
                std::erase_if(contexts, [&](const Context& c) {
                  return c.frame_id == real_id && c.name == world.name &&
                         c.is_default == world.main;
                });
                self->EvaluateIn(real_id, world, expression, user_gesture,
                                 /*retry=*/false, std::move(callback));
                return;
              }
              std::move(callback).Run(MapEvaluate(std::move(reply)));
            },
            self, session, real_id, world, expression, user_gesture, retry,
            std::move(callback)));
  };

  if (world.main) {
    std::optional<int> context = MainContext(*session, real_id);
    if (!context && !is_local_root) {
      EvaluateResult failed;
      failed.message = "the frame has no script context yet";
      std::move(callback).Run(std::move(failed));
      return;
    }
    run(weak_factory_.GetWeakPtr(), *session, real_id, world, expression,
        user_gesture, retry, std::move(callback), context, "");
    return;
  }
  EnsureWorld(
      real_id, world.name,
      base::BindOnce(run, weak_factory_.GetWeakPtr(), *session, real_id, world,
                     expression, user_gesture, retry, std::move(callback)));
}

void PageProtocol::EnsureDom(const std::string& session_id,
                             base::OnceClosure done) {
  if (dom_sessions_.contains(session_id)) {
    std::move(done).Run();
    return;
  }
  Send(session_id, "DOM.enable", base::DictValue(),
       base::BindOnce(
           [](base::WeakPtr<PageProtocol> self, std::string session_id,
              base::OnceClosure done, ProtocolResult result) {
             if (self && result.ok) {
               self->dom_sessions_.insert(session_id);
             }
             // A failure shows in the command that needed it.
             std::move(done).Run();
           },
           weak_factory_.GetWeakPtr(), session_id, std::move(done)));
}

void PageProtocol::EnsureCss(const std::string& session_id,
                             base::OnceClosure done) {
  EnsureDom(
      session_id,
      base::BindOnce(
          [](base::WeakPtr<PageProtocol> self, std::string session_id,
             base::OnceClosure done) {
            if (!self || self->css_sessions_.contains(session_id)) {
              std::move(done).Run();
              return;
            }
            self->Send(
                session_id, "CSS.enable", base::DictValue(),
                base::BindOnce(
                    [](base::WeakPtr<PageProtocol> self, std::string session_id,
                       base::OnceClosure done, ProtocolResult result) {
                      if (self && result.ok) {
                        self->css_sessions_.insert(session_id);
                      }
                      std::move(done).Run();
                    },
                    self, session_id, std::move(done)));
          },
          weak_factory_.GetWeakPtr(), session_id, std::move(done)));
}

void PageProtocol::FrameOffset(const std::string& frame_id,
                               OffsetCallback callback) {
  content::RenderFrameHost* frame = FindFrame(frame_id);
  if (!frame) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  if (frame == GetWebContents().GetPrimaryMainFrame()) {
    std::move(callback).Run(gfx::PointF());
    return;
  }
  content::RenderFrameHost* parent = frame->GetParent();
  std::optional<std::string> session =
      parent ? SessionOf(parent) : std::optional<std::string>();
  if (!session) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  const std::string real_id = FrameId(frame);
  EnsureDom(
      *session,
      base::BindOnce(
          [](base::WeakPtr<PageProtocol> self, std::string real_id,
             std::string session, OffsetCallback callback) {
            if (!self) {
              std::move(callback).Run(std::nullopt);
              return;
            }
            base::DictValue params;
            params.Set("frameId", real_id);
            self->Send(session, "DOM.getFrameOwner", std::move(params),
                       base::BindOnce(&PageProtocol::OnFrameOwner, self,
                                      real_id, session, std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), real_id, *session, std::move(callback)));
}

void PageProtocol::OnFrameOwner(const std::string& frame_id,
                                const std::string& session_id,
                                OffsetCallback callback,
                                ProtocolResult owner) {
  std::optional<int> node =
      owner.ok ? owner.result.FindInt("backendNodeId") : std::nullopt;
  if (!node) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  base::DictValue params;
  params.Set("backendNodeId", *node);
  Send(
      session_id, "DOM.getBoxModel", std::move(params),
      base::BindOnce(&PageProtocol::OnFrameOwnerBox, weak_factory_.GetWeakPtr(),
                     frame_id, std::move(callback)));
}

void PageProtocol::OnFrameOwnerBox(const std::string& frame_id,
                                   OffsetCallback callback,
                                   ProtocolResult box) {
  const base::DictValue* model =
      box.ok ? box.result.FindDict("model") : nullptr;
  std::optional<gfx::PointF> corner =
      model ? FirstCorner(*model, "content") : std::nullopt;
  content::RenderFrameHost* frame = FindFrame(frame_id);
  content::RenderFrameHost* parent = frame ? frame->GetParent() : nullptr;
  if (!corner || !parent) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  // The box is relative to the viewport of the parent's local root; add where
  // that is.
  content::RenderFrameHost* root = LocalRootOf(parent);
  if (!root || root == GetWebContents().GetPrimaryMainFrame()) {
    std::move(callback).Run(corner);
    return;
  }
  FrameOffset(FrameId(root),
              base::BindOnce(
                  [](gfx::PointF corner, OffsetCallback callback,
                     std::optional<gfx::PointF> offset) {
                    if (!offset) {
                      std::move(callback).Run(std::nullopt);
                      return;
                    }
                    std::move(callback).Run(gfx::PointF(
                        corner.x() + offset->x(), corner.y() + offset->y()));
                  },
                  *corner, std::move(callback)));
}

void PageProtocol::InsertCss(const std::string& frame_id,
                             const std::string& css,
                             CssCallback callback) {
  content::RenderFrameHost* frame = FindFrame(frame_id);
  std::optional<std::string> session =
      frame ? SessionOf(frame) : std::optional<std::string>();
  if (!session) {
    std::move(callback).Run("", "there is no such frame");
    return;
  }
  const std::string real_id = FrameId(frame);
  EnsureCss(
      *session,
      base::BindOnce(
          [](base::WeakPtr<PageProtocol> self, std::string session,
             std::string real_id, std::string css, CssCallback callback) {
            if (!self) {
              std::move(callback).Run("", "the page was closed");
              return;
            }
            base::DictValue params;
            params.Set("frameId", real_id);
            self->Send(
                session, "CSS.createStyleSheet", std::move(params),
                base::BindOnce(
                    [](base::WeakPtr<PageProtocol> self, std::string session,
                       std::string real_id, std::string css,
                       CssCallback callback, ProtocolResult created) {
                      const std::string* sheet_id =
                          created.ok ? created.result.FindString("styleSheetId")
                                     : nullptr;
                      if (!self || !sheet_id) {
                        std::move(callback).Run(
                            "", created.error_message.empty()
                                    ? "the style sheet could not be created"
                                    : created.error_message);
                        return;
                      }
                      std::unique_ptr<Sheet>& sheet = self->sheets_[real_id];
                      if (!sheet || sheet->sheet_id != *sheet_id ||
                          sheet->session_id != session) {
                        sheet = std::make_unique<Sheet>();
                        sheet->session_id = session;
                        sheet->sheet_id = *sheet_id;
                      }
                      const std::string id =
                          "css-" + base::NumberToString(++self->last_css_);
                      sheet->chunks.emplace_back(id, css);
                      self->frame_of_css_[id] = real_id;
                      self->WriteSheet(
                          real_id,
                          base::BindOnce(
                              [](base::WeakPtr<PageProtocol> self,
                                 std::string real_id, std::string id,
                                 CssCallback callback, bool written) {
                                if (written) {
                                  std::move(callback).Run(id, "");
                                  return;
                                }
                                // Take the piece back: it never applied.
                                if (self) {
                                  self->frame_of_css_.erase(id);
                                  auto it = self->sheets_.find(real_id);
                                  if (it != self->sheets_.end()) {
                                    std::erase_if(it->second->chunks,
                                                  [&](const auto& chunk) {
                                                    return chunk.first == id;
                                                  });
                                  }
                                }
                                std::move(callback).Run(
                                    "", "the style sheet could not be written");
                              },
                              self, real_id, id, std::move(callback)));
                    },
                    self, session, real_id, css, std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), *session, real_id, css,
          std::move(callback)));
}

void PageProtocol::WriteSheet(const std::string& frame_id,
                              base::OnceCallback<void(bool)> done) {
  auto it = sheets_.find(frame_id);
  if (it == sheets_.end()) {
    std::move(done).Run(false);
    return;
  }
  std::string text;
  for (const auto& chunk : it->second->chunks) {
    text += chunk.second;
    text += "\n";
  }
  base::DictValue params;
  params.Set("styleSheetId", it->second->sheet_id);
  params.Set("text", text);
  Send(it->second->session_id, "CSS.setStyleSheetText", std::move(params),
       base::BindOnce(
           [](base::OnceCallback<void(bool)> done, ProtocolResult result) {
             std::move(done).Run(result.ok);
           },
           std::move(done)));
}

void PageProtocol::RemoveCss(const std::string& id,
                             base::OnceCallback<void(bool)> done) {
  auto known = frame_of_css_.find(id);
  if (known == frame_of_css_.end()) {
    std::move(done).Run(false);
    return;
  }
  const std::string frame_id = known->second;
  frame_of_css_.erase(known);
  auto it = sheets_.find(frame_id);
  bool removed = false;
  if (it != sheets_.end()) {
    removed = std::erase_if(it->second->chunks, [&](const auto& chunk) {
                return chunk.first == id;
              }) > 0;
  }
  if (!removed) {
    // The document the CSS was in is gone, and the CSS with it.
    std::move(done).Run(true);
    return;
  }
  WriteSheet(frame_id, base::BindOnce([](base::OnceCallback<void(bool)> done,
                                         bool) { std::move(done).Run(true); },
                                      std::move(done)));
}

bool PageProtocol::NetworkIdle() {
  // If no lifecycle event was seen for the current load (Chromium did not
  // replay the ones before the client attached), an idle page is one that is
  // not loading.
  return network_idle_ || (!lifecycle_seen_ && !GetWebContents().IsLoading());
}

}  // namespace bdk
