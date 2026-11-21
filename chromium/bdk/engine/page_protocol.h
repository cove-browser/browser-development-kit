// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_PAGE_PROTOCOL_H_
#define BDK_ENGINE_PAGE_PROTOCOL_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/gfx/geometry/point_f.h"

class GURL;

namespace content {
class NavigationHandle;
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace bdk {

// BDK's own use of the DevTools protocol (architecture.md section 6, M7):
// contents.evaluate, query, text, snapshot, CSS, input, screenshots and user
// scripts are written against Chrome's protocol rather than against private
// renderer interfaces, so they behave as the tools people already know.
//
// Reads "internalScheme" from the resolved configuration, the way
// SetRuntimeInfo does, for MayInspect below. Called when the runtime starts.
void SetPageProtocolRuntimeInfo(const std::string& resolved_json);

// Whether a page at `url` (a WebUI page or not) may be inspected: not WebUI,
// not chrome://, chrome-untrusted://, devtools://, chrome-extension:// and not
// the application's own internal scheme. The same rule every protocol client
// of BDK, the internal one and the debug sessions, applies.
bool MayInspectUrl(const GURL& url, bool is_webui);
bool MayInspectFrame(content::RenderFrameHost* frame);

// A reply to one protocol command.
struct ProtocolResult {
  ProtocolResult();
  ProtocolResult(ProtocolResult&&);
  ProtocolResult& operator=(ProtocolResult&&);
  ~ProtocolResult();

  bool ok = false;
  // The command's result, when it succeeded.
  base::DictValue result;
  // When it failed: the protocol's error code (0 when the failure was not the
  // protocol's: the page went away, say) and message.
  int error_code = 0;
  std::string error_message;
};
using ProtocolCallback = base::OnceCallback<void(ProtocolResult)>;

// What Runtime.evaluate answered, mapped to what contents.evaluate reports.
struct EvaluateResult {
  enum class Status {
    // `value` holds the value (null for undefined).
    kValue,
    // The script threw; `message` is the exception's description.
    kException,
    // The value cannot be returned by value (a function, a DOM node, NaN...).
    kNotSerializable,
    // The command failed: the frame is gone, the page went away...
    kFailed,
  };

  EvaluateResult();
  EvaluateResult(EvaluateResult&&);
  EvaluateResult& operator=(EvaluateResult&&);
  ~EvaluateResult();

  Status status = Status::kFailed;
  base::Value value;
  std::string message;
};
using EvaluateCallback = base::OnceCallback<void(EvaluateResult)>;

// Where a script runs in a frame: its main world, or a named isolated world
// (a separate JavaScript global that sees the same DOM).
struct World {
  static World Main();
  static World Isolated(std::string name);

  bool main = true;
  std::string name;
};

// Why a page could not be attached to.
enum class Availability {
  kReady,
  // The page is internal (WebUI, chrome://, devtools://, the application's
  // scheme): BDK never scripts it.
  kInternalPage,
  // Chromium refused (a policy, a page being torn down).
  kUnavailable,
};
using ReadyCallback = base::OnceCallback<void(Availability)>;

// The internal protocol client of one page. It is attached lazily, by the
// first command that needs it (or because the page's profile has user scripts,
// see user_scripts.h): an attached client is not free, since it keeps the
// page's protocol agents alive and makes Chromium report to it. It is detached
// when the page goes away. Owned by the page's WebContents.
//
// It attaches to the page target (DevToolsAgentHost::GetOrCreateFor), which
// answers Runtime, Page, DOM, CSS, Input and Accessibility directly; the tab
// target would need Target.attachToTarget first. Out-of-process iframes are
// auto-attached with flattened sessions, and messages are routed by session.
// A frame's DevTools frame token is both its protocol frame id and, for the
// local root of an out-of-process iframe, its target id.
//
// All of it runs on the UI thread.
class PageProtocol : public content::WebContentsUserData<PageProtocol>,
                     public content::DevToolsAgentHostClient,
                     public content::WebContentsObserver {
 public:
  // Told when a protocol session of the page gets ready or goes away: the
  // main session (id "") when the client attaches or Chromium detaches it,
  // and each out-of-process iframe's. User scripts register themselves in
  // every session this way.
  class SessionObserver : public base::CheckedObserver {
   public:
    virtual void OnSessionAttached(const std::string& session_id) = 0;
    virtual void OnSessionDetached(const std::string& session_id) = 0;
    // A frame of the page reached a lifecycle stage ("init", "commit",
    // "DOMContentLoaded", "load", "networkIdle"...) in a document that
    // `loader_id` names.
    virtual void OnFrameLifecycle(const std::string& session_id,
                                  const std::string& frame_id,
                                  const std::string& loader_id,
                                  const std::string& name) {}
  };

  ~PageProtocol() override;

  // The page's client, created on first use. Nothing attaches until WhenReady
  // or KeepAttached.
  static PageProtocol* For(content::WebContents* web_contents);
  // The page's client, or null if none was ever created.
  static PageProtocol* Existing(content::WebContents* web_contents);

  base::WeakPtr<PageProtocol> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  void AddSessionObserver(SessionObserver* observer);
  void RemoveSessionObserver(SessionObserver* observer);

  // Runs `ready` once the page is attached and its domains are enabled, or
  // with the reason it cannot be. Always runs later, never from inside the
  // call.
  void WhenReady(ReadyCallback ready);
  // Attaches now and re-attaches after a navigation that detached the client
  // (a visit to an internal page), for as long as the page lives.
  void KeepAttached();
  bool attached() const { return ready_; }

  // The ids of the sessions that are ready: "" for the page, then each
  // out-of-process iframe's.
  std::vector<std::string> Sessions() const;

  // Sends `method` in session `session_id` ("" is the page's). Must be
  // attached (WhenReady); otherwise the callback gets a failure. `callback`
  // may be null.
  void Send(const std::string& session_id,
            const std::string& method,
            base::DictValue params,
            ProtocolCallback callback);

  // The frame with this protocol frame id; the primary main frame for an
  // empty id; null when there is none.
  content::RenderFrameHost* FindFrame(const std::string& frame_id);
  // The protocol frame id of `frame`.
  static std::string FrameId(content::RenderFrameHost* frame);
  // The session that serves `frame`: its local root's. Null when the frame
  // lives in another process and its session has not been reported yet.
  std::optional<std::string> SessionOf(content::RenderFrameHost* frame);

  // Sends `method` in the session that serves `frame_id`.
  void SendToFrame(const std::string& frame_id,
                   const std::string& method,
                   base::DictValue params,
                   ProtocolCallback callback);

  // Evaluates `expression` in `frame_id` ("" for the main frame) in `world`,
  // awaiting a returned promise and returning the value by value.
  void Evaluate(const std::string& frame_id,
                const World& world,
                const std::string& expression,
                bool user_gesture,
                EvaluateCallback callback);

  // Where `frame_id`'s viewport origin is in the main frame's viewport, in CSS
  // pixels: add it to a point measured in the frame to get the point
  // Input.dispatch* takes. Null when it cannot be found.
  using OffsetCallback = base::OnceCallback<void(std::optional<gfx::PointF>)>;
  void FrameOffset(const std::string& frame_id, OffsetCallback callback);

  // Adds CSS to the frame through its inspector style sheet, which a page's
  // content security policy does not apply to. `callback` gets an opaque id,
  // or an empty id and the reason. Several additions share the frame's one
  // sheet; the id removes just that one.
  using CssCallback =
      base::OnceCallback<void(std::string id, std::string error)>;
  void InsertCss(const std::string& frame_id,
                 const std::string& css,
                 CssCallback callback);
  // Removes what `id` added. `callback` is told whether the id was known
  // (a navigation forgets the sheets of the frames it replaces).
  void RemoveCss(const std::string& id, base::OnceCallback<void(bool)> done);

  // Whether the page's main frame has reached network idle (no network
  // activity for 500 ms after load), from Page.lifecycleEvent.
  bool NetworkIdle();

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;
  bool MayAttachToURL(const GURL& url, bool is_webui) override;
  bool MayAttachToRenderFrameHost(
      content::RenderFrameHost* render_frame_host) override;
  // BDK's own client needs nothing beyond the page: it is not trusted with
  // the browser target, privileged schemes or local files.
  bool IsTrusted() override;
  bool MayReadLocalFiles() override;
  bool MayWriteLocalFiles() override;

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override;

 private:
  friend class content::WebContentsUserData<PageProtocol>;
  struct PendingCall;
  struct Sheet;

  explicit PageProtocol(content::WebContents* web_contents);

  void TryAttach();
  void FinishAttach(Availability availability);
  // Enables Page, Runtime and auto-attach in a session, then calls `done`.
  void SetUpSession(const std::string& session_id, base::OnceClosure done);
  void OnSessionReady(const std::string& session_id);
  void RemoveSession(const std::string& session_id);
  void FailPending(const std::string& session_id, const std::string& message);
  void HandleEvent(const std::string& session_id,
                   const std::string& method,
                   const base::DictValue& params);
  void OnContextCreated(const std::string& session_id,
                        const base::DictValue& context);

  // The nearest frame at or above `frame` that has a session of its own: the
  // primary main frame, or the local root of an out-of-process iframe.
  content::RenderFrameHost* LocalRootOf(content::RenderFrameHost* frame);

  // The execution context of an isolated world in a frame, created if needed.
  void EnsureWorld(const std::string& frame_id,
                   const std::string& name,
                   base::OnceCallback<void(std::optional<int>, std::string)>);
  // The id of the frame's main-world context, if one was reported.
  std::optional<int> MainContext(const std::string& session_id,
                                 const std::string& frame_id) const;
  void EvaluateIn(const std::string& frame_id,
                  const World& world,
                  const std::string& expression,
                  bool user_gesture,
                  bool retry,
                  EvaluateCallback callback);

  // DOM, then CSS, enabled in a session the first time something needs them.
  void EnsureDom(const std::string& session_id, base::OnceClosure done);
  void EnsureCss(const std::string& session_id, base::OnceClosure done);
  void OnFrameOwner(const std::string& frame_id,
                    const std::string& session_id,
                    OffsetCallback callback,
                    ProtocolResult owner);
  void OnFrameOwnerBox(const std::string& frame_id,
                       OffsetCallback callback,
                       ProtocolResult box);
  void WriteSheet(const std::string& frame_id,
                  base::OnceCallback<void(bool)> done);

  scoped_refptr<content::DevToolsAgentHost> host_;
  // The page's main session is set up and ready for commands.
  bool ready_ = false;
  bool attaching_ = false;
  bool keep_attached_ = false;
  std::vector<ReadyCallback> waiters_;
  int last_id_ = 0;
  std::map<int, std::unique_ptr<PendingCall>> pending_;
  // Out-of-process iframes: target id (the local root's frame token) to
  // session id, and back.
  std::map<std::string, std::string> session_by_target_;
  std::map<std::string, std::string> target_by_session_;
  // Sessions that are set up (the main one is "").
  std::set<std::string> ready_sessions_;
  std::set<std::string> dom_sessions_;
  std::set<std::string> css_sessions_;
  // Execution contexts per session: id, frame id and world name ("" for the
  // main world).
  struct Context {
    int id = 0;
    std::string frame_id;
    std::string name;
    bool is_default = false;
  };
  std::map<std::string, std::vector<Context>> contexts_;
  // The inspector style sheet of each frame, and which frame each CSS id is
  // in.
  std::map<std::string, std::unique_ptr<Sheet>> sheets_;
  std::map<std::string, std::string> frame_of_css_;
  int last_css_ = 0;
  bool network_idle_ = false;
  bool lifecycle_seen_ = false;
  base::ObserverList<SessionObserver> observers_;
  base::WeakPtrFactory<PageProtocol> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace bdk

#endif  // BDK_ENGINE_PAGE_PROTOCOL_H_
