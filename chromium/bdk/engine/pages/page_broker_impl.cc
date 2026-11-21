// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/pages/page_broker_impl.h"

#include <utility>

#include "base/values.h"
#include "bdk/core/runtime/broker_internal.h"
#include "bdk/core/runtime/routes.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/json_util.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {
namespace {

std::string ErrorJson(const std::string& domain,
                      const std::string& code,
                      const std::string& message,
                      const std::string& details_json) {
  base::DictValue error;
  error.Set("domain", domain);
  error.Set("code", code);
  error.Set("message", message);
  std::string out = ToJson(error);
  if (!details_json.empty() && details_json != "{}") {
    // Splice the details in as JSON, not as a string.
    out.insert(out.size() - 1, ",\"details\":" + details_json);
  }
  return out;
}

std::string Text(bdk_string_t s) {
  return s.data ? std::string(s.data, s.length) : std::string();
}

// What a call carries through the core's C callbacks.
struct CallContext {
  base::WeakPtr<PageBrokerImpl> broker;
  uint64_t call_id = 0;
  mojom::PageBroker::InvokeCallback callback;
};

void OnCallResult(void* ctx,
                  bdk_error_t error,
                  bdk_string_t json,
                  bdk_buffer_t binary) {
  auto* call = static_cast<CallContext*>(ctx);
  if (call->broker) {
    call->broker->OnResult(call->call_id);
  }
  if (error) {
    std::move(call->callback)
        .Run(false, ErrorJson(Text(bdk_error_domain(error)),
                              Text(bdk_error_code(error)),
                              Text(bdk_error_message(error)),
                              Text(bdk_error_details_json(error))));
    return;
  }
  // Binary results are not used by any command a page may call.
  std::move(call->callback).Run(true, Text(json));
}

void OnCallProgress(void* ctx, double fraction, bdk_string_t message) {
  auto* call = static_cast<CallContext*>(ctx);
  if (call->broker) {
    call->broker->OnProgress(call->call_id, fraction, Text(message));
  }
}

void ReleaseCall(void* ctx) {
  delete static_cast<CallContext*>(ctx);
}

struct SubscriptionContext {
  base::WeakPtr<PageBrokerImpl> broker;
  uint64_t subscription_id = 0;
};

void OnSubscribedEvent(void* ctx,
                       uint32_t event_id,
                       bdk_string_t name,
                       bdk_string_t payload) {
  auto* sub = static_cast<SubscriptionContext*>(ctx);
  if (sub->broker) {
    sub->broker->OnEvent(sub->subscription_id, Text(payload));
  }
}

void ReleaseSubscription(void* ctx) {
  delete static_cast<SubscriptionContext*>(ctx);
}

}  // namespace

std::shared_ptr<bdkcore::Caller> BindPageCaller(content::RenderFrameHost* frame,
                                                std::string* why) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt || !frame) {
    *why = "no runtime";
    return nullptr;
  }
  EngineContents* page = EngineContents::FromWebContents(
      content::WebContents::FromRenderFrameHost(frame));
  std::shared_ptr<bdkcore::Contents> contents = page ? page->Core() : nullptr;
  if (!contents || !contents->profile || !contents->profile->open) {
    *why = "the frame is not in a page of an open BDK profile";
    return nullptr;
  }
  const GURL& url = frame->GetLastCommittedURL();
  content::RenderProcessHost* process = frame->GetProcess();
  auto* policy = content::ChildProcessSecurityPolicy::GetInstance();
  if (!policy->HasWebUIBindings(process->GetID())) {
    *why = "the frame's process has no WebUI bindings";
    return nullptr;
  }
  if (!policy->CanAccessDataForOrigin(process->GetDeprecatedID(),
                                      url::Origin::Create(url))) {
    *why = "the frame's process is not locked to the page's origin";
    return nullptr;
  }
  return bdkcore::page_caller_bind(*rt, url.spec(), contents->profile->handle,
                                   frame->IsInPrimaryMainFrame(), why);
}

PageBrokerImpl::PageBrokerImpl(
    content::RenderFrameHost* frame,
    std::shared_ptr<bdkcore::Caller> caller,
    mojo::PendingReceiver<mojom::PageBroker> receiver)
    : frame_id_(frame->GetGlobalId()),
      caller_(std::move(caller)),
      receiver_(this, std::move(receiver)) {}

PageBrokerImpl::~PageBrokerImpl() {
  // The document is going: its calls are cancelled and its subscriptions end.
  bdkcore::broker_revoke_caller(caller_);
}

content::RenderFrameHost* PageBrokerImpl::CurrentFrame() {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_id_);
  if (!rt || !frame || !frame->IsInPrimaryMainFrame() ||
      !bdkcore::page_caller_current(*rt, *caller_,
                                    frame->GetLastCommittedURL().spec())) {
    return nullptr;
  }
  return frame;
}

void PageBrokerImpl::SetClient(mojo::PendingRemote<mojom::PageClient> client) {
  client_.reset();
  client_.Bind(std::move(client));
}

void PageBrokerImpl::Invoke(uint64_t call_id,
                            const std::string& command,
                            const std::string& arguments_json,
                            InvokeCallback callback) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  content::RenderFrameHost* frame = CurrentFrame();
  if (!rt || !frame || calls_.count(call_id)) {
    std::move(callback).Run(
        false, ErrorJson(BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED,
                         "the page no longer holds the broker", ""));
    return;
  }
  auto* ctx = new CallContext;
  ctx->broker = weak_factory_.GetWeakPtr();
  ctx->call_id = call_id;
  ctx->callback = std::move(callback);
  bdkcore::CommandRef ref;
  ref.name = command;
  // The gesture is the frame's, read here; the page cannot claim one.
  bdk_call_t call =
      bdkcore::broker_submit(rt, caller_, ref, arguments_json, nullptr,
                             frame->HasTransientUserActivation(), &OnCallResult,
                             &OnCallProgress, ctx, &ReleaseCall);
  if (!call) {
    std::move(ctx->callback)
        .Run(false, ErrorJson(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                              "the call was refused", ""));
    delete ctx;
    return;
  }
  calls_[call_id] = call;
}

void PageBrokerImpl::Cancel(uint64_t call_id) {
  auto it = calls_.find(call_id);
  if (it != calls_.end()) {
    bdk_call_cancel(it->second);
  }
}

void PageBrokerImpl::Subscribe(uint64_t subscription_id,
                               const std::string& event,
                               const std::string& filter_json,
                               SubscribeCallback callback) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt || !CurrentFrame() || subscriptions_.count(subscription_id)) {
    std::move(callback).Run(
        false, ErrorJson(BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED,
                         "the page no longer holds the broker", ""));
    return;
  }
  // Filters are not applied to a page's subscriptions yet: a page gets every
  // event of its own profile that it holds the capability for.
  auto* ctx =
      new SubscriptionContext{weak_factory_.GetWeakPtr(), subscription_id};
  std::string domain, code, message;
  bdk_subscription_t sub = bdkcore::broker_subscribe_as(
      rt, caller_, event, &OnSubscribedEvent, ctx, &ReleaseSubscription,
      &domain, &code, &message);
  if (!sub) {
    delete ctx;
    std::move(callback).Run(false, ErrorJson(domain, code, message, ""));
    return;
  }
  subscriptions_[subscription_id] = sub;
  std::move(callback).Run(true, std::string());
}

void PageBrokerImpl::Unsubscribe(uint64_t subscription_id) {
  auto it = subscriptions_.find(subscription_id);
  if (it == subscriptions_.end()) {
    return;
  }
  bdk_unsubscribe(it->second);
  subscriptions_.erase(it);
}

void PageBrokerImpl::OnResult(uint64_t call_id) {
  calls_.erase(call_id);
}

void PageBrokerImpl::OnProgress(uint64_t call_id,
                                double fraction,
                                const std::string& message) {
  if (client_) {
    client_->OnProgress(call_id, fraction, message);
  }
}

void PageBrokerImpl::OnEvent(uint64_t subscription_id,
                             const std::string& payload_json) {
  if (client_ && CurrentFrame()) {
    client_->OnEvent(subscription_id, payload_json);
  }
}

}  // namespace bdk
