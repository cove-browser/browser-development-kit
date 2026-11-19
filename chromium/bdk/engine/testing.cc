// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The engine's test hooks (bdk_testing.h) and the scripted navigations behind
// them.

#include "bdk/engine/testing.h"

#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/strings/escape.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/current_thread.h"
#include "base/time/time.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/bdk_testing.h"
#include "bdk/engine/engine_contents.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/url_loader_request_interceptor.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "ui/gfx/geometry/point_f.h"
#include "url/gurl.h"

namespace bdk {

namespace {

struct Script {
  int32_t net_error = 0;
  std::string title;
};

// Read on the UI thread by the interceptor and written by the hooks, also on
// the UI thread; the lock keeps it safe if Chromium ever asks from another.
struct Scripts {
  base::Lock lock;
  std::map<std::string, Script> by_url;
  bool intercept_all = false;
};

Scripts& GetScripts() {
  static base::NoDestructor<Scripts> scripts;
  return *scripts;
}

std::atomic<size_t>& PendingTasks() {
  static std::atomic<size_t> pending{0};
  return pending;
}

bdk_testing_ui_outcome_t& LastOutcome() {
  static bdk_testing_ui_outcome_t outcome;
  return outcome;
}

void SetOutcome(uint32_t kind,
                uint32_t surface,
                const std::string& completion,
                const std::string& args) {
  bdk_testing_ui_outcome_t& o = LastOutcome();
  o = bdk_testing_ui_outcome_t();
  o.struct_size = sizeof o;
  o.kind = kind;
  o.surface = surface;
  base::span<char> c(o.completion);
  base::span<char> a(o.arguments_json);
  const size_t cn = std::min(completion.size(), c.size() - 1);
  const size_t an = std::min(args.size(), a.size() - 1);
  c.copy_prefix_from(base::span(completion).first(cn));
  a.copy_prefix_from(base::span(args).first(an));
}

// Answers one scripted request: a page, or a network error.
void Serve(Script script,
           const network::ResourceRequest& request,
           mojo::PendingReceiver<network::mojom::URLLoader> loader,
           mojo::PendingRemote<network::mojom::URLLoaderClient> client_remote) {
  mojo::Remote<network::mojom::URLLoaderClient> client(
      std::move(client_remote));
  if (script.net_error != 0) {
    client->OnComplete(network::URLLoaderCompletionStatus(script.net_error));
    return;
  }
  const std::string title =
      script.title.empty() ? request.url.spec() : script.title;
  const std::string body = "<!doctype html><title>" +
                           base::EscapeForHTML(title) + "</title><body></body>";
  auto head = network::mojom::URLResponseHead::New();
  head->headers = net::HttpResponseHeaders::TryToCreate(
      "HTTP/1.1 200 OK\nContent-Type: text/html; charset=utf-8\n");
  head->mime_type = "text/html";
  head->charset = "utf-8";
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  if (mojo::CreateDataPipe(static_cast<uint32_t>(body.size()), producer,
                           consumer) != MOJO_RESULT_OK ||
      producer->WriteAllData(base::as_byte_span(body)) != MOJO_RESULT_OK) {
    client->OnComplete(network::URLLoaderCompletionStatus(net::ERR_FAILED));
    return;
  }
  client->OnReceiveResponse(std::move(head), std::move(consumer), std::nullopt);
  network::URLLoaderCompletionStatus status(net::OK);
  client->OnComplete(status);
}

class ScriptedNavigationInterceptor
    : public content::URLLoaderRequestInterceptor {
 public:
  void MaybeCreateLoader(const network::ResourceRequest& request,
                         content::BrowserContext* browser_context,
                         LoaderCallback callback) override {
    std::optional<Script> script;
    {
      Scripts& scripts = GetScripts();
      base::AutoLock lock(scripts.lock);
      auto it = scripts.by_url.find(request.url.spec());
      if (it != scripts.by_url.end()) {
        script = it->second;
      } else if (scripts.intercept_all && request.url.SchemeIsHTTPOrHTTPS()) {
        script = Script();
      }
    }
    if (!script) {
      std::move(callback).Run({});
      return;
    }
    std::move(callback).Run(base::BindOnce(&Serve, std::move(*script)));
  }
};

std::shared_ptr<bdkcore::Contents> LiveContents(bdk_contents_t handle) {
  std::shared_ptr<bdkcore::Contents> c = bdkcore::contents_of(handle);
  return c && !c->dead && c->data ? c : nullptr;
}

}  // namespace

std::unique_ptr<content::URLLoaderRequestInterceptor>
CreateScriptedNavigationInterceptor() {
  Scripts& scripts = GetScripts();
  base::AutoLock lock(scripts.lock);
  if (scripts.by_url.empty() && !scripts.intercept_all) {
    return nullptr;
  }
  return std::make_unique<ScriptedNavigationInterceptor>();
}

void NoteTaskPosted() {
  PendingTasks().fetch_add(1);
}

void NoteTaskRun() {
  PendingTasks().fetch_sub(1);
}

}  // namespace bdk

extern "C" {

BDK_EXPORT bdk_status_t bdk_testing_script_navigation(bdk_runtime_t runtime,
                                                      bdk_string_t url,
                                                      int32_t net_error,
                                                      bdk_string_t title) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  if (!bdkcore::runtime_of(runtime)) {
    return BDK_ERR_STALE_HANDLE;
  }
  if (!bdkcore::string_ok(url) || !bdkcore::string_ok(title)) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  bdk::Scripts& scripts = bdk::GetScripts();
  base::AutoLock lock(scripts.lock);
  scripts.by_url[GURL(bdkcore::str(url)).spec()] =
      bdk::Script{net_error, bdkcore::str(title)};
  return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_testing_intercept_all(uint8_t on) {
  bdk::Scripts& scripts = bdk::GetScripts();
  base::AutoLock lock(scripts.lock);
  scripts.intercept_all = on != 0;
  if (!on) {
    scripts.by_url.clear();
  }
  return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_testing_raise_ui_request(bdk_runtime_t runtime,
                                                     uint32_t surface,
                                                     bdk_contents_t contents,
                                                     bdk_string_t payload) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::runtime_of(runtime);
  if (!rt) {
    return BDK_ERR_STALE_HANDLE;
  }
  if ((contents && !bdkcore::contents_of(contents)) ||
      !bdkcore::string_ok(payload)) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  auto request = std::make_shared<bdkcore::Request>();
  request->surface = surface;
  request->contents = contents;
  request->payload = bdkcore::str(payload);
  request->on_finish = [surface](bdkcore::UiOutcome outcome,
                                 const std::string& completion,
                                 const std::string& args) {
    bdk::SetOutcome(static_cast<uint32_t>(outcome), surface, completion, args);
  };
  bdk::SetOutcome(0, surface, "", "");
  bdkcore::raise_ui_request(rt, request);
  return BDK_OK;
}

BDK_EXPORT bdk_status_t
bdk_testing_withdraw_ui_requests(bdk_runtime_t runtime) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::runtime_of(runtime);
  if (!rt) {
    return BDK_ERR_STALE_HANDLE;
  }
  bdkcore::withdraw_contents_requests(rt, 0);
  return BDK_OK;
}

BDK_EXPORT bdk_status_t
bdk_testing_last_ui_outcome(bdk_runtime_t runtime,
                            bdk_testing_ui_outcome_t* out) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  if (!bdkcore::runtime_of(runtime)) {
    return BDK_ERR_STALE_HANDLE;
  }
  if (!out) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  *out = bdk::LastOutcome();
  return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_testing_simulate_key(bdk_contents_t contents,
                                                 bdk_native_handle_t key_event,
                                                 uint8_t* out_reserved) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  std::shared_ptr<bdkcore::Contents> c = bdkcore::contents_of(contents);
  if (!c) {
    return BDK_ERR_STALE_HANDLE;
  }
  const bool reserved = bdkcore::reserve_key(*c, key_event);
  if (!reserved) {
    bdkcore::report_unhandled_key(*c, key_event);
  }
  if (out_reserved) {
    *out_reserved = reserved ? 1 : 0;
  }
  return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_testing_execute_script(bdk_contents_t contents,
                                                   bdk_string_t script,
                                                   uint8_t user_gesture) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  std::shared_ptr<bdkcore::Contents> c = bdk::LiveContents(contents);
  if (!c) {
    return BDK_ERR_STALE_HANDLE;
  }
  if (!bdkcore::string_ok(script)) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  content::RenderFrameHost* frame =
      bdk::EngineContents::From(*c)->web_contents()->GetPrimaryMainFrame();
  const std::u16string js = base::UTF8ToUTF16(bdkcore::str(script));
  if (user_gesture) {
    frame->ExecuteJavaScriptWithUserGestureForTests(
        js, base::NullCallback(), content::ISOLATED_WORLD_ID_GLOBAL);
  } else {
    frame->ExecuteJavaScriptForTests(js, base::NullCallback(),
                                     content::ISOLATED_WORLD_ID_GLOBAL);
  }
  return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_testing_click(bdk_contents_t contents,
                                          double x,
                                          double y,
                                          uint32_t button) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  std::shared_ptr<bdkcore::Contents> c = bdk::LiveContents(contents);
  if (!c) {
    return BDK_ERR_STALE_HANDLE;
  }
  content::RenderWidgetHostView* view =
      bdk::EngineContents::From(*c)->web_contents()->GetRenderWidgetHostView();
  if (!view) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  const bool right = button == 2;
  const auto pointer_button = right
                                  ? blink::WebPointerProperties::Button::kRight
                                  : blink::WebPointerProperties::Button::kLeft;
  const int modifiers = right ? blink::WebInputEvent::kRightButtonDown
                              : blink::WebInputEvent::kLeftButtonDown;
  const gfx::PointF point(static_cast<float>(x), static_cast<float>(y));
  for (auto type : {blink::WebInputEvent::Type::kMouseDown,
                    blink::WebInputEvent::Type::kMouseUp}) {
    blink::WebMouseEvent event(
        type, point, point, pointer_button,
        /*click_count=*/1,
        type == blink::WebInputEvent::Type::kMouseDown ? modifiers : 0,
        base::TimeTicks::Now());
    view->GetRenderWidgetHost()->ForwardMouseEvent(event);
  }
  return BDK_OK;
}

BDK_EXPORT void bdk_testing_forget_profiles(void) {
  bdk::ForgetPersistentProfilesForTesting();
}

BDK_EXPORT void bdk_testing_run_for(double seconds) {
  base::CurrentThread::ScopedAllowApplicationTasksInNativeNestedLoop allow;
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

BDK_EXPORT size_t bdk_testing_pending_work(void) {
  size_t pending = bdk::PendingTasks().load();
  if (std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime()) {
    for (const auto& c : rt->contents) {
      bdk::EngineContents* page =
          c->data ? bdk::EngineContents::From(*c) : nullptr;
      if (page && (page->web_contents()->IsLoading() || page->closing())) {
        ++pending;
      }
    }
  }
  return pending;
}

}  // extern "C"
