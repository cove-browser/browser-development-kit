// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_PAGES_PAGE_BROKER_IMPL_H_
#define BDK_ENGINE_PAGES_PAGE_BROKER_IMPL_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "bdk/core/include/bdk/bdk_base.h"
#include "bdk/engine/pages/page_broker.mojom.h"
#include "content/public/browser/global_routing_id.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace bdkcore {
struct Caller;
}

namespace content {
class RenderFrameHost;
}

namespace bdk {

// Binds the page broker for a frame of a bundled page (T-WUI-04): checks, at
// bind time, the frame's committed URL against the route table, that it is
// the primary main frame, that its process has WebUI bindings and may hold
// the page's origin, and that the page is one of BDK's in a profile BDK has
// open. Returns the caller, or null with `why` when the frame may not have
// one; then the renderer is to be terminated.
std::shared_ptr<bdkcore::Caller> BindPageCaller(content::RenderFrameHost* frame,
                                                std::string* why);

// One internal page's connection to the broker. Every call goes through the
// core's broker (bdkcore::broker_submit) as the page's caller, which checks
// the capability, the scope and the gesture; this class re-checks on every
// call that the frame still shows the page the caller was bound for. It is
// owned by the page's WebUI controller, which Chromium destroys when the
// document goes, so privilege ends on navigation: the caller is revoked
// then, cancelling its calls and ending its subscriptions.
class PageBrokerImpl : public mojom::PageBroker {
 public:
  PageBrokerImpl(content::RenderFrameHost* frame,
                 std::shared_ptr<bdkcore::Caller> caller,
                 mojo::PendingReceiver<mojom::PageBroker> receiver);
  PageBrokerImpl(const PageBrokerImpl&) = delete;
  PageBrokerImpl& operator=(const PageBrokerImpl&) = delete;
  ~PageBrokerImpl() override;

  // mojom::PageBroker:
  void SetClient(mojo::PendingRemote<mojom::PageClient> client) override;
  void Invoke(uint64_t call_id,
              const std::string& command,
              const std::string& arguments_json,
              InvokeCallback callback) override;
  void Cancel(uint64_t call_id) override;
  void Subscribe(uint64_t subscription_id,
                 const std::string& event,
                 const std::string& filter_json,
                 SubscribeCallback callback) override;
  void Unsubscribe(uint64_t subscription_id) override;

  // For the C callbacks of the core.
  void OnResult(uint64_t call_id);
  void OnProgress(uint64_t call_id,
                  double fraction,
                  const std::string& message);
  void OnEvent(uint64_t subscription_id, const std::string& payload_json);

 private:
  // The frame, if it still shows the page the caller was bound for.
  content::RenderFrameHost* CurrentFrame();

  const content::GlobalRenderFrameHostId frame_id_;
  const std::shared_ptr<bdkcore::Caller> caller_;
  mojo::Receiver<mojom::PageBroker> receiver_;
  mojo::Remote<mojom::PageClient> client_;
  std::map<uint64_t, bdk_call_t> calls_;
  std::map<uint64_t, bdk_subscription_t> subscriptions_;
  base::WeakPtrFactory<PageBrokerImpl> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_PAGES_PAGE_BROKER_IMPL_H_
