// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// contents.*: what needs the renderer. The core answers list, get, create,
// close, navigate, reload, stop, goBack, goForward, setMuted, setZoom,
// requestAttention and waitFor with only load, url or a timeout; everything
// else about a page is here (services.md section 5.15). Most of it is written
// against the DevTools protocol through the page's PageProtocol
// (page_protocol.h); small helper scripts run in an isolated world named
// "bdk-query", so the page's own scripts cannot see or change them.

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/page_protocol.h"
#include "bdk/engine/services/capture_service.h"
#include "bdk/engine/services/page_call.h"
#include "bdk/engine/services/services.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_save_item_data.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/save_page_type.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace bdk {

namespace {

constexpr char kQueryWorld[] = "bdk-query";
constexpr char kDefaultWorld[] = "bdk";
constexpr int kDefaultTimeoutMs = 30000;
constexpr int kDefaultQueryLimit = 100;
constexpr int kMaxQueryLimit = 1000;
constexpr base::TimeDelta kPollInterval = base::Milliseconds(100);
// How long a save may take to start before it is called failed.
constexpr base::TimeDelta kSaveStartTimeout = base::Seconds(30);
constexpr int kMaxAxNodes = 20000;
constexpr int kMaxAxDepth = 100;

// ---------------------------------------------------------------------------
// The helper library. Every helper script is this followed by a body that
// returns a value the protocol can copy: an object, `{error}` for an argument
// the page cannot use (a selector that does not parse) or `{none: true}` when
// no element matches. They run in the isolated world, where the page's own
// changes to built-ins (a replaced Array.from) are not visible.
// ---------------------------------------------------------------------------
constexpr char kLibrary[] = R"JS(
const cap = (s, n) => (s.length > n ? s.slice(0, n) : s);
const textOf = (el) =>
    (el.innerText !== undefined ? el.innerText : el.textContent) || '';
const box = (el) => {
  if (el.getClientRects().length === 0) return null;
  const r = el.getBoundingClientRect();
  return {x: r.x, y: r.y, width: r.width, height: r.height};
};
const find = (selector) => {
  try {
    return {list: Array.from(document.querySelectorAll(selector))};
  } catch (e) {
    return {error: 'invalid selector: ' + e.message};
  }
};
const first = (selector) => {
  const found = find(selector);
  if (found.error) return found;
  return found.list.length ? {element: found.list[0]} : {none: true};
};
const reveal = (el) =>
    el.scrollIntoView({block: 'center', inline: 'center', behavior: 'instant'});
)JS";

// The visible text of the document, for contents.text and snapshot.
constexpr char kTextBody[] =
    "const el = document.body || document.documentElement;"
    "return {text: el ? cap(textOf(el), 500000) : ''};";

std::string HelperExpression(const std::string& body) {
  return std::string("(() => {") + kLibrary + body + "})()";
}

// ---------------------------------------------------------------------------
// The page a call addresses.
// ---------------------------------------------------------------------------

using PageHandler = void (*)(const std::shared_ptr<ServiceCall>&,
                             PageProtocol&);

// Runs `handler` once the page's protocol client is attached. The client is
// attached by the first command that needs it, not when the page is created.
void WithPage(const std::shared_ptr<ServiceCall>& c, PageHandler handler) {
  content::WebContents* web_contents = LivePage(c);
  if (!web_contents) {
    return;
  }
  PageProtocol* protocol = PageProtocol::For(web_contents);
  protocol->WhenReady(base::BindOnce(
      [](std::shared_ptr<ServiceCall> c, base::WeakPtr<PageProtocol> protocol,
         PageHandler handler, Availability availability) {
        if (c->cancelled()) {
          return;
        }
        if (!protocol) {
          c->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_NOT_FOUND,
                    "the page is closed");
          return;
        }
        switch (availability) {
          case Availability::kReady:
            handler(c, *protocol);
            return;
          case Availability::kInternalPage:
            c->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_INTERNAL_PAGE,
                      "an internal page cannot be scripted");
            return;
          case Availability::kUnavailable:
            c->Reject(BDK_DOMAIN_PLATFORM, "unavailable",
                      "Chromium does not allow this page to be scripted");
            return;
        }
      },
      c, protocol->GetWeakPtr(), handler));
}

template <PageHandler handler>
void Run(const std::shared_ptr<ServiceCall>& c) {
  WithPage(c, handler);
}

// The frame the call names (its `frame` argument): `frame_id` is its protocol
// id, empty for the main frame. False, with the call rejected, when there is
// no such frame.
bool ResolveFrame(const std::shared_ptr<ServiceCall>& c,
                  PageProtocol& protocol,
                  std::string* frame_id) {
  std::optional<std::string> frame = c->String("frame");
  if (!frame || frame->empty()) {
    frame_id->clear();
    return true;
  }
  if (!protocol.FindFrame(*frame)) {
    c->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_NOT_FOUND,
              "there is no frame with that id");
    return false;
  }
  *frame_id = *frame;
  return true;
}

std::string ValueJson(const base::Value& value) {
  return base::WriteJson(value).value_or("null");
}

void RejectFailure(const std::shared_ptr<ServiceCall>& c,
                   const std::string& message) {
  c->Reject(BDK_DOMAIN_PLATFORM, "failed", message);
}

// Reports what evaluating an expression failed with, as the command that ran
// it should.
void RejectEvaluate(const std::shared_ptr<ServiceCall>& c,
                    const EvaluateResult& result) {
  switch (result.status) {
    case EvaluateResult::Status::kException:
      c->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_SCRIPT_ERROR, result.message);
      return;
    case EvaluateResult::Status::kNotSerializable:
      c->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_NOT_SERIALIZABLE, result.message);
      return;
    case EvaluateResult::Status::kFailed:
    case EvaluateResult::Status::kValue:
      RejectFailure(c, result.message);
      return;
  }
}

// What a helper script answered. True when `result` holds an object the
// command can read; otherwise the call has been rejected: a failure, a
// selector that does not parse, or no element.
bool CheckHelper(const std::shared_ptr<ServiceCall>& c,
                 const EvaluateResult& result,
                 const base::DictValue** out) {
  if (result.status != EvaluateResult::Status::kValue) {
    RejectEvaluate(c, result);
    return false;
  }
  const base::DictValue* dict = result.value.GetIfDict();
  if (!dict) {
    RejectFailure(c, "the page script answered with something unexpected");
    return false;
  }
  if (const std::string* error = dict->FindString("error")) {
    c->InvalidArguments(*error);
    return false;
  }
  if (dict->FindBool("none").value_or(false)) {
    c->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_NO_ELEMENT,
              "no element matches the selector");
    return false;
  }
  *out = dict;
  return true;
}

void RunHelper(PageProtocol& protocol,
               const std::string& frame_id,
               const std::string& body,
               EvaluateCallback done) {
  protocol.Evaluate(frame_id, World::Isolated(kQueryWorld),
                    HelperExpression(body), /*user_gesture=*/false,
                    std::move(done));
}

// ---------------------------------------------------------------------------
// contents.evaluate, text, query, frames, snapshot
// ---------------------------------------------------------------------------

void ReplyEvaluate(std::shared_ptr<ServiceCall> c, EvaluateResult result) {
  if (result.status != EvaluateResult::Status::kValue) {
    RejectEvaluate(c, result);
    return;
  }
  c->Resolve("{\"value\":" + ValueJson(result.value) + "}");
}

void Evaluate(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  std::string frame_id;
  if (!ResolveFrame(c, protocol, &frame_id)) {
    return;
  }
  // "page" is the frame's own world; any other name, or none, is an isolated
  // world, so a script cannot be confused by (or confuse) the page's.
  const std::string world = c->StringOr("world", "");
  protocol.Evaluate(
      frame_id,
      world == "page" ? World::Main()
                      : World::Isolated(world.empty() ? kDefaultWorld : world),
      c->StringOr("expression", ""), c->Bool("userGesture").value_or(false),
      base::BindOnce(&ReplyEvaluate, c));
}

void Text(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  std::string frame_id;
  if (!ResolveFrame(c, protocol, &frame_id)) {
    return;
  }
  RunHelper(
      protocol, frame_id, kTextBody,
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> c, EvaluateResult result) {
            const base::DictValue* dict = nullptr;
            if (!CheckHelper(c, result, &dict)) {
              return;
            }
            const std::string* text = dict->FindString("text");
            c->Resolve("{\"text\":" + JsonString(text ? *text : "") + "}");
          },
          c));
}

void Query(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  std::string frame_id;
  if (!ResolveFrame(c, protocol, &frame_id)) {
    return;
  }
  const int limit = std::clamp(
      static_cast<int>(c->Number("limit").value_or(kDefaultQueryLimit)), 1,
      kMaxQueryLimit);
  RunHelper(protocol, frame_id,
            "const found = find(" + JsonString(c->StringOr("selector", "")) +
                ");"
                "if (found.error) return found;"
                "return {elements: found.list.slice(0, " +
                base::NumberToString(limit) +
                ").map((el) => {"
                "  const attributes = Object.create(null);"
                "  for (const a of el.attributes) attributes[a.name] = a.value;"
                "  return {tag: el.localName, attributes,"
                "          text: cap(textOf(el).trim(), 1000), box: box(el)};"
                "})};",
            base::BindOnce(
                [](std::shared_ptr<ServiceCall> c, EvaluateResult result) {
                  const base::DictValue* dict = nullptr;
                  if (!CheckHelper(c, result, &dict)) {
                    return;
                  }
                  c->Resolve(ValueJson(result.value));
                },
                c));
}

void Frames(const std::shared_ptr<ServiceCall>& c) {
  content::WebContents* web_contents = LivePage(c);
  if (!web_contents) {
    return;
  }
  std::string out = "{\"frames\":[";
  bool first = true;
  // The frames of the page on screen; a page in the back-forward cache or
  // being prerendered is not part of it.
  web_contents->GetPrimaryMainFrame()->ForEachRenderFrameHost(
      [&](content::RenderFrameHost* frame) {
        content::RenderFrameHost* parent = frame->GetParent();
        out += first ? "" : ",";
        first = false;
        out += "{\"id\":" + JsonString(PageProtocol::FrameId(frame)) +
               ",\"parent\":" +
               (parent ? JsonString(PageProtocol::FrameId(parent)) : "null") +
               ",\"url\":" + JsonString(frame->GetLastCommittedURL().spec()) +
               ",\"origin\":" +
               JsonString(frame->GetLastCommittedOrigin().Serialize()) +
               ",\"name\":" + JsonString(frame->GetFrameName()) + "}";
      });
  out += "]}";
  c->Resolve(out);
}

// The accessibility tree, as {role, name, value?, children?}. A node the
// browser ignores is left out and its children take its place.
void AppendAxNode(const std::map<std::string, const base::DictValue*>& nodes,
                  const std::string& id,
                  int depth,
                  int* budget,
                  base::ListValue& out) {
  auto it = nodes.find(id);
  if (it == nodes.end() || *budget <= 0 || depth > kMaxAxDepth) {
    return;
  }
  const base::DictValue& node = *it->second;
  --*budget;
  const base::ListValue* child_ids = node.FindList("childIds");
  if (node.FindBool("ignored").value_or(false)) {
    if (child_ids) {
      for (const base::Value& child : *child_ids) {
        if (child.is_string()) {
          AppendAxNode(nodes, child.GetString(), depth, budget, out);
        }
      }
    }
    return;
  }
  auto text_of = [&](const char* key) -> std::string {
    const base::DictValue* field = node.FindDict(key);
    const base::Value* value = field ? field->Find("value") : nullptr;
    return value && value->is_string() ? value->GetString() : std::string();
  };
  base::DictValue entry;
  entry.Set("role", text_of("role"));
  entry.Set("name", text_of("name"));
  if (const base::DictValue* field = node.FindDict("value")) {
    if (const base::Value* value = field->Find("value")) {
      entry.Set("value", value->Clone());
    }
  }
  if (child_ids) {
    base::ListValue children;
    for (const base::Value& child : *child_ids) {
      if (child.is_string()) {
        AppendAxNode(nodes, child.GetString(), depth + 1, budget, children);
      }
    }
    if (!children.empty()) {
      entry.Set("children", std::move(children));
    }
  }
  out.Append(std::move(entry));
}

void ReplyAccessibility(std::shared_ptr<ServiceCall> c, ProtocolResult result) {
  const base::ListValue* list =
      result.ok ? result.result.FindList("nodes") : nullptr;
  if (!list) {
    RejectFailure(c, result.error_message.empty()
                         ? "the accessibility tree could not be read"
                         : result.error_message);
    return;
  }
  std::map<std::string, const base::DictValue*> nodes;
  std::string root_id;
  for (const base::Value& value : *list) {
    const base::DictValue* node = value.GetIfDict();
    const std::string* id = node ? node->FindString("nodeId") : nullptr;
    if (!id) {
      continue;
    }
    nodes[*id] = node;
    if (root_id.empty() && !node->FindString("parentId")) {
      root_id = *id;
    }
  }
  base::ListValue roots;
  int budget = kMaxAxNodes;
  AppendAxNode(nodes, root_id, 0, &budget, roots);
  std::string root = "null";
  if (roots.size() == 1) {
    root = ValueJson(roots[0]);
  } else if (!roots.empty()) {
    base::DictValue wrapper;
    wrapper.Set("role", "RootWebArea");
    wrapper.Set("name", "");
    wrapper.Set("children", std::move(roots));
    root = ToJson(wrapper);
  }
  c->Resolve("{\"kind\":\"accessibility\",\"root\":" + root + "}");
}

void Snapshot(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  std::string frame_id;
  if (!ResolveFrame(c, protocol, &frame_id)) {
    return;
  }
  const std::string kind = c->StringOr("kind", "");
  if (kind == "text") {
    RunHelper(protocol, frame_id, kTextBody,
              base::BindOnce(
                  [](std::shared_ptr<ServiceCall> c, EvaluateResult result) {
                    const base::DictValue* dict = nullptr;
                    if (!CheckHelper(c, result, &dict)) {
                      return;
                    }
                    const std::string* text = dict->FindString("text");
                    c->Resolve("{\"kind\":\"text\",\"root\":" +
                               JsonString(text ? *text : "") + "}");
                  },
                  c));
  } else if (kind == "dom") {
    // At most 5000 nodes and 40 levels; the root says when it was cut.
    RunHelper(protocol, frame_id,
              "let budget = 5000;"
              "const walk = (n, depth) => {"
              "  if (budget <= 0) return null;"
              "  if (n.nodeType === 3) {"
              "    const t = n.textContent.trim();"
              "    if (!t) return null;"
              "    budget--;"
              "    return {type: 'text', text: cap(t, 500)};"
              "  }"
              "  if (n.nodeType !== 1) return null;"
              "  budget--;"
              "  const o = {type: 'element', tag: n.localName};"
              "  if (n.attributes.length) {"
              "    o.attributes = Object.create(null);"
              "    for (const a of n.attributes) o.attributes[a.name] = "
              "cap(a.value, 200);"
              "  }"
              "  if (depth >= 40) { o.truncated = true; return o; }"
              "  const kids = [];"
              "  for (const child of n.childNodes) {"
              "    const k = walk(child, depth + 1);"
              "    if (k) kids.push(k);"
              "  }"
              "  if (kids.length) o.children = kids;"
              "  return o;"
              "};"
              "const root = document.documentElement ? "
              "walk(document.documentElement, 0)"
              "                                       : null;"
              "if (root && budget <= 0) root.truncated = true;"
              "return {root};",
              base::BindOnce(
                  [](std::shared_ptr<ServiceCall> c, EvaluateResult result) {
                    const base::DictValue* dict = nullptr;
                    if (!CheckHelper(c, result, &dict)) {
                      return;
                    }
                    const base::Value* root = dict->Find("root");
                    c->Resolve("{\"kind\":\"dom\",\"root\":" +
                               (root ? ValueJson(*root) : "null") + "}");
                  },
                  c));
  } else {
    base::DictValue params;
    if (!frame_id.empty()) {
      params.Set("frameId", frame_id);
    }
    protocol.SendToFrame(frame_id, "Accessibility.getFullAXTree",
                         std::move(params),
                         base::BindOnce(&ReplyAccessibility, c));
  }
}

// ---------------------------------------------------------------------------
// contents.insertCSS and removeCSS
// ---------------------------------------------------------------------------

void InsertCss(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  std::string frame_id;
  if (!ResolveFrame(c, protocol, &frame_id)) {
    return;
  }
  protocol.InsertCss(frame_id, c->StringOr("css", ""),
                     base::BindOnce(
                         [](std::shared_ptr<ServiceCall> c, std::string id,
                            std::string error) {
                           if (id.empty()) {
                             RejectFailure(c, error);
                             return;
                           }
                           c->Resolve("{\"id\":" + JsonString(id) + "}");
                         },
                         c));
}

void RemoveCss(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  protocol.RemoveCss(
      c->StringOr("id", ""),
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> c, bool known) {
            if (!known) {
              c->InvalidArguments("no style sheet was inserted with that id");
              return;
            }
            c->ResolveNull();
          },
          c));
}

// ---------------------------------------------------------------------------
// Input: contents.click, hover, type, press, scroll
// ---------------------------------------------------------------------------

struct InputMessage {
  std::string method;
  base::DictValue params;
};

struct SendAllState {
  size_t remaining = 0;
  std::string error;
  base::OnceCallback<void(std::string)> done;
};

// Sends the messages in order in one session and calls `done` with the first
// failure's message, or an empty string, once every one has been answered.
void SendAll(PageProtocol& protocol,
             const std::string& session_id,
             std::vector<InputMessage> messages,
             base::OnceCallback<void(std::string)> done) {
  auto state = std::make_shared<SendAllState>();
  state->remaining = messages.size();
  state->done = std::move(done);
  for (InputMessage& message : messages) {
    protocol.Send(
        session_id, message.method, std::move(message.params),
        base::BindOnce(
            [](std::shared_ptr<SendAllState> state, ProtocolResult result) {
              if (!result.ok && state->error.empty()) {
                state->error = result.error_message;
              }
              if (--state->remaining == 0) {
                std::move(state->done).Run(state->error);
              }
            },
            state));
  }
}

void ReplyInput(std::shared_ptr<ServiceCall> c, std::string error) {
  if (!error.empty()) {
    RejectFailure(c, error);
    return;
  }
  c->ResolveNull();
}

// `x` and `y`, measured in the frame, as a point in the main frame's viewport:
// calls `done` with it, or rejects the call.
void OffsetPoint(std::shared_ptr<ServiceCall> c,
                 base::WeakPtr<PageProtocol> protocol,
                 std::string frame_id,
                 double x,
                 double y,
                 base::OnceCallback<void(gfx::PointF)> done) {
  if (!protocol) {
    c->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_NOT_FOUND, "the page is closed");
    return;
  }
  protocol->FrameOffset(
      frame_id, base::BindOnce(
                    [](std::shared_ptr<ServiceCall> c, double x, double y,
                       base::OnceCallback<void(gfx::PointF)> done,
                       std::optional<gfx::PointF> offset) {
                      if (!offset) {
                        RejectFailure(
                            c, "the frame's place on the page is unknown");
                        return;
                      }
                      std::move(done).Run(
                          gfx::PointF(static_cast<float>(x) + offset->x(),
                                      static_cast<float>(y) + offset->y()));
                    },
                    c, x, y, std::move(done)));
}

// Where a pointer command acts, in the main frame's viewport (what
// Input.dispatchMouseEvent takes, routed to out-of-process frames by hit
// testing): the centre of the element `selector` names after scrolling it into
// view, or the point (x, y) of the frame. Calls `done` with the point, or
// rejects the call.
void ResolvePoint(const std::shared_ptr<ServiceCall>& c,
                  PageProtocol& protocol,
                  const std::string& frame_id,
                  base::OnceCallback<void(gfx::PointF)> done) {
  std::optional<std::string> selector = c->String("selector");
  if (!selector) {
    std::optional<double> x = c->Number("x");
    std::optional<double> y = c->Number("y");
    if (!x || !y) {
      c->InvalidArguments("give a selector, or both x and y");
      return;
    }
    OffsetPoint(c, protocol.GetWeakPtr(), frame_id, *x, *y, std::move(done));
    return;
  }
  RunHelper(protocol, frame_id,
            "const r = first(" + JsonString(*selector) +
                ");"
                "if (!r.element) return r;"
                "reveal(r.element);"
                "const b = box(r.element);"
                "if (!b) return {error: 'the element has no box to act on'};"
                "return {x: b.x + b.width / 2, y: b.y + b.height / 2};",
            base::BindOnce(
                [](std::shared_ptr<ServiceCall> c,
                   base::WeakPtr<PageProtocol> protocol, std::string frame_id,
                   base::OnceCallback<void(gfx::PointF)> done,
                   EvaluateResult result) {
                  const base::DictValue* dict = nullptr;
                  if (!CheckHelper(c, result, &dict)) {
                    return;
                  }
                  OffsetPoint(
                      c, protocol, frame_id, dict->FindDouble("x").value_or(0),
                      dict->FindDouble("y").value_or(0), std::move(done));
                },
                c, protocol.GetWeakPtr(), frame_id, std::move(done)));
}

constexpr int kModifierAlt = 1;
constexpr int kModifierControl = 2;
constexpr int kModifierMeta = 4;
constexpr int kModifierShift = 8;

void PointerClick(std::shared_ptr<ServiceCall> c,
                  base::WeakPtr<PageProtocol> protocol,
                  gfx::PointF point) {
  if (!protocol) {
    return;
  }
  const std::string button = c->StringOr("button", "left");
  const int buttons = button == "right" ? 2 : button == "middle" ? 4 : 1;
  const int count =
      std::clamp(static_cast<int>(c->Number("clickCount").value_or(1)), 1, 3);
  auto mouse = [&](const char* type, int clicks, int pressed) {
    InputMessage message;
    message.method = "Input.dispatchMouseEvent";
    message.params.Set("type", type);
    message.params.Set("x", point.x());
    message.params.Set("y", point.y());
    message.params.Set("button", button);
    message.params.Set("buttons", pressed);
    message.params.Set("clickCount", clicks);
    return message;
  };
  std::vector<InputMessage> messages;
  messages.push_back(mouse("mouseMoved", 0, 0));
  for (int i = 1; i <= count; ++i) {
    messages.push_back(mouse("mousePressed", i, buttons));
    messages.push_back(mouse("mouseReleased", i, 0));
  }
  SendAll(*protocol, "", std::move(messages), base::BindOnce(&ReplyInput, c));
}

void PointerHover(std::shared_ptr<ServiceCall> c,
                  base::WeakPtr<PageProtocol> protocol,
                  gfx::PointF point) {
  if (!protocol) {
    return;
  }
  InputMessage message;
  message.method = "Input.dispatchMouseEvent";
  message.params.Set("type", "mouseMoved");
  message.params.Set("x", point.x());
  message.params.Set("y", point.y());
  std::vector<InputMessage> messages;
  messages.push_back(std::move(message));
  SendAll(*protocol, "", std::move(messages), base::BindOnce(&ReplyInput, c));
}

void Click(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  std::string frame_id;
  if (!ResolveFrame(c, protocol, &frame_id)) {
    return;
  }
  ResolvePoint(c, protocol, frame_id,
               base::BindOnce(&PointerClick, c, protocol.GetWeakPtr()));
}

void Hover(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  std::string frame_id;
  if (!ResolveFrame(c, protocol, &frame_id)) {
    return;
  }
  ResolvePoint(c, protocol, frame_id,
               base::BindOnce(&PointerHover, c, protocol.GetWeakPtr()));
}

void InsertText(std::shared_ptr<ServiceCall> c,
                base::WeakPtr<PageProtocol> protocol,
                std::string frame_id) {
  if (!protocol) {
    return;
  }
  base::DictValue params;
  params.Set("text", c->StringOr("text", ""));
  protocol->SendToFrame(
      frame_id, "Input.insertText", std::move(params),
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> c, ProtocolResult result) {
            ReplyInput(c, result.ok ? "" : result.error_message);
          },
          c));
}

void Type(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  std::string frame_id;
  if (!ResolveFrame(c, protocol, &frame_id)) {
    return;
  }
  std::optional<std::string> selector = c->String("selector");
  if (!selector) {
    // Into whatever has focus.
    InsertText(c, protocol.GetWeakPtr(), frame_id);
    return;
  }
  RunHelper(protocol, frame_id,
            "const r = first(" + JsonString(*selector) +
                ");"
                "if (!r.element) return r;"
                "reveal(r.element);"
                "r.element.focus();"
                "return {};",
            base::BindOnce(
                [](std::shared_ptr<ServiceCall> c,
                   base::WeakPtr<PageProtocol> protocol, std::string frame_id,
                   EvaluateResult result) {
                  const base::DictValue* dict = nullptr;
                  if (!CheckHelper(c, result, &dict)) {
                    return;
                  }
                  InsertText(c, protocol, frame_id);
                },
                c, protocol.GetWeakPtr(), frame_id));
}

// A key as the protocol describes it.
struct KeyInfo {
  const char* name;
  const char* code;
  int virtual_key;
  const char* text;
};

constexpr KeyInfo kNamedKeys[] = {
    {"Enter", "Enter", 13, "\r"},
    {"Tab", "Tab", 9, ""},
    {"Escape", "Escape", 27, ""},
    {"Backspace", "Backspace", 8, ""},
    {"Delete", "Delete", 46, ""},
    {"Insert", "Insert", 45, ""},
    {" ", "Space", 32, " "},
    {"ArrowUp", "ArrowUp", 38, ""},
    {"ArrowDown", "ArrowDown", 40, ""},
    {"ArrowLeft", "ArrowLeft", 37, ""},
    {"ArrowRight", "ArrowRight", 39, ""},
    {"Home", "Home", 36, ""},
    {"End", "End", 35, ""},
    {"PageUp", "PageUp", 33, ""},
    {"PageDown", "PageDown", 34, ""},
    {"F1", "F1", 112, ""},
    {"F2", "F2", 113, ""},
    {"F3", "F3", 114, ""},
    {"F4", "F4", 115, ""},
    {"F5", "F5", 116, ""},
    {"F6", "F6", 117, ""},
    {"F7", "F7", 118, ""},
    {"F8", "F8", 119, ""},
    {"F9", "F9", 120, ""},
    {"F10", "F10", 121, ""},
    {"F11", "F11", 122, ""},
    {"F12", "F12", 123, ""},
};

// Other spellings of the names above.
std::string CanonicalKeyName(const std::string& name) {
  struct Alias {
    const char* from;
    const char* to;
  };
  static constexpr Alias kAliases[] = {
      {"Return", "Enter"},     {"Esc", "Escape"},     {"Space", " "},
      {"Up", "ArrowUp"},       {"Down", "ArrowDown"}, {"Left", "ArrowLeft"},
      {"Right", "ArrowRight"}, {"Del", "Delete"},     {"Ins", "Insert"},
  };
  for (const Alias& alias : kAliases) {
    if (base::EqualsCaseInsensitiveASCII(name, alias.from)) {
      return alias.to;
    }
  }
  return name;
}

struct ResolvedKey {
  std::string key;
  std::string code;
  std::string text;
  int virtual_key = 0;
};

// False when the name is neither in the table nor one character.
bool ResolveKey(const std::string& name, bool shift, ResolvedKey* out) {
  const std::string canonical = CanonicalKeyName(name);
  for (const KeyInfo& info : kNamedKeys) {
    if (base::EqualsCaseInsensitiveASCII(canonical, info.name)) {
      out->key = info.name;
      out->code = info.code;
      out->text = info.text;
      out->virtual_key = info.virtual_key;
      return true;
    }
  }
  if (name.size() != 1) {
    return false;
  }
  const char ch = name[0];
  out->key = name;
  out->text = name;
  if (base::IsAsciiAlpha(ch)) {
    const char upper = base::ToUpperASCII(ch);
    out->code = std::string("Key") + upper;
    out->virtual_key = upper;
    if (shift) {
      out->key = out->text = std::string(1, upper);
    }
  } else if (base::IsAsciiDigit(ch)) {
    out->code = std::string("Digit") + ch;
    out->virtual_key = ch;
  }
  return true;
}

void Press(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  int modifiers = 0;
  if (const base::ListValue* list = c->List("modifiers")) {
    for (const base::Value& value : *list) {
      const std::string name = value.is_string() ? value.GetString() : "";
      if (base::EqualsCaseInsensitiveASCII(name, "alt") ||
          base::EqualsCaseInsensitiveASCII(name, "option")) {
        modifiers |= kModifierAlt;
      } else if (base::EqualsCaseInsensitiveASCII(name, "control") ||
                 base::EqualsCaseInsensitiveASCII(name, "ctrl")) {
        modifiers |= kModifierControl;
      } else if (base::EqualsCaseInsensitiveASCII(name, "meta") ||
                 base::EqualsCaseInsensitiveASCII(name, "command") ||
                 base::EqualsCaseInsensitiveASCII(name, "cmd")) {
        modifiers |= kModifierMeta;
      } else if (base::EqualsCaseInsensitiveASCII(name, "shift")) {
        modifiers |= kModifierShift;
      } else {
        c->InvalidArguments("unknown modifier '" + name +
                            "': use alt, control, meta or shift");
        return;
      }
    }
  }
  ResolvedKey key;
  if (!ResolveKey(c->StringOr("key", ""), (modifiers & kModifierShift) != 0,
                  &key)) {
    c->InvalidArguments("unknown key '" + c->StringOr("key", "") +
                        "': use a key name such as Enter or ArrowDown, or one "
                        "character");
    return;
  }
  // A key that is held with Control, Alt or Meta types nothing.
  const bool types =
      !key.text.empty() &&
      (modifiers & (kModifierControl | kModifierAlt | kModifierMeta)) == 0;
  // The protocol does not turn shortcuts into editing: say what they mean.
  base::ListValue commands;
  if ((modifiers & (kModifierControl | kModifierMeta)) != 0) {
    static constexpr struct {
      const char* key;
      const char* command;
    } kShortcuts[] = {{"a", "selectAll"},
                      {"c", "copy"},
                      {"v", "paste"},
                      {"x", "cut"},
                      {"z", "undo"}};
    for (const auto& shortcut : kShortcuts) {
      if (base::EqualsCaseInsensitiveASCII(key.key, shortcut.key)) {
        commands.Append(shortcut.command);
      }
    }
  }
  auto event = [&](const char* type, bool with_text) {
    InputMessage message;
    message.method = "Input.dispatchKeyEvent";
    message.params.Set("type", type);
    message.params.Set("modifiers", modifiers);
    message.params.Set("key", key.key);
    message.params.Set("code", key.code);
    message.params.Set("windowsVirtualKeyCode", key.virtual_key);
    message.params.Set("nativeVirtualKeyCode", key.virtual_key);
    if (with_text) {
      message.params.Set("text", key.text);
      message.params.Set("unmodifiedText", key.text);
    }
    return message;
  };
  std::vector<InputMessage> messages;
  messages.push_back(event(types ? "keyDown" : "rawKeyDown", types));
  if (!commands.empty()) {
    messages.back().params.Set("commands", std::move(commands));
  }
  messages.push_back(event("keyUp", false));
  SendAll(protocol, "", std::move(messages), base::BindOnce(&ReplyInput, c));
}

void Scroll(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  std::string frame_id;
  if (!ResolveFrame(c, protocol, &frame_id)) {
    return;
  }
  const std::optional<std::string> selector = c->String("selector");
  const double dx = c->Number("dx").value_or(0);
  const double dy = c->Number("dy").value_or(0);
  if (!selector && dx == 0 && dy == 0) {
    c->InvalidArguments("give a selector, or dx or dy");
    return;
  }
  // Scrolled by script, in the frame, so that it works the same in a frame in
  // another process. With a selector and no distance the element is brought
  // into view; with a distance, that element scrolls by it.
  const std::string deltas = "left: " + base::NumberToString(dx) +
                             ", top: " + base::NumberToString(dy) +
                             ", behavior: 'instant'";
  RunHelper(protocol, frame_id,
            selector ? "const r = first(" + JsonString(*selector) +
                           ");"
                           "if (!r.element) return r;" +
                           (dx == 0 && dy == 0
                                ? std::string("reveal(r.element);")
                                : "r.element.scrollBy({" + deltas + "});") +
                           "return {};"
                     : "window.scrollBy({" + deltas + "}); return {};",
            base::BindOnce(
                [](std::shared_ptr<ServiceCall> c, EvaluateResult result) {
                  const base::DictValue* dict = nullptr;
                  if (!CheckHelper(c, result, &dict)) {
                    return;
                  }
                  c->ResolveNull();
                },
                c));
}

// ---------------------------------------------------------------------------
// contents.waitFor (the conditions the core does not answer)
// ---------------------------------------------------------------------------

// Matches `text` against a pattern whose only special character is `*`.
bool GlobMatch(const std::string& pattern, const std::string& text) {
  size_t p = 0, t = 0, star = std::string::npos, mark = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = t;
    } else if (p < pattern.size() && pattern[p] == text[t]) {
      ++p;
      ++t;
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

bool Truthy(const base::Value& value) {
  if (value.is_bool()) {
    return value.GetBool();
  }
  if (value.is_int() || value.is_double()) {
    return value.GetDouble() != 0 && !std::isnan(value.GetDouble());
  }
  if (value.is_string()) {
    return !value.GetString().empty();
  }
  return value.is_dict() || value.is_list();
}

// One contents.waitFor in progress: every condition given must hold. Checked
// every 100 ms until they do, the call is cancelled or the time is up.
struct WaitFor {
  std::shared_ptr<ServiceCall> call;
  base::WeakPtr<PageProtocol> protocol;
  std::string frame_id;
  std::optional<std::string> load;
  std::optional<std::string> url;
  std::optional<std::string> selector;
  std::optional<std::string> script;
  bool network_idle = false;
  base::TimeTicks deadline;
  bool finished = false;
  std::string last_error;
};

void PollWaitFor(std::shared_ptr<WaitFor> wait);

void WaitForLater(std::shared_ptr<WaitFor> wait) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce(&PollWaitFor, std::move(wait)), kPollInterval);
}

void WaitForDone(const std::shared_ptr<WaitFor>& wait) {
  wait->finished = true;
  wait->call->ResolveNull();
}

void WaitForTimedOut(const std::shared_ptr<WaitFor>& wait) {
  if (wait->finished || wait->call->cancelled()) {
    return;
  }
  wait->finished = true;
  std::string message = "timed out waiting for the page";
  if (!wait->last_error.empty()) {
    message += " (last error: " + wait->last_error + ")";
  }
  wait->call->Reject(BDK_DOMAIN_BROKER, BDK_CODE_TIMEOUT, message);
}

// Checks the script predicate, the last one, then finishes or tries again.
void CheckWaitScript(std::shared_ptr<WaitFor> wait) {
  if (!wait->script) {
    WaitForDone(wait);
    return;
  }
  PageProtocol* protocol = wait->protocol.get();
  if (!protocol) {
    return;
  }
  protocol->Evaluate(
      wait->frame_id, World::Main(), *wait->script, /*user_gesture=*/false,
      base::BindOnce(
          [](std::shared_ptr<WaitFor> wait, EvaluateResult result) {
            if (wait->finished) {
              return;
            }
            if (result.status == EvaluateResult::Status::kValue &&
                Truthy(result.value)) {
              WaitForDone(wait);
              return;
            }
            // A script that throws is tried again: the page may not have
            // defined what it uses yet. The last failure is reported if time
            // runs out.
            wait->last_error = result.message;
            WaitForLater(wait);
          },
          wait));
}

void CheckWaitSelector(std::shared_ptr<WaitFor> wait) {
  if (!wait->selector) {
    CheckWaitScript(wait);
    return;
  }
  PageProtocol* protocol = wait->protocol.get();
  if (!protocol) {
    return;
  }
  RunHelper(
      *protocol, wait->frame_id,
      "try { return {present: document.querySelector(" +
          JsonString(*wait->selector) +
          ") !== null}; } catch (e) { return {error: 'invalid selector: ' + "
          "e.message}; }",
      base::BindOnce(
          [](std::shared_ptr<WaitFor> wait, EvaluateResult result) {
            if (wait->finished) {
              return;
            }
            const base::DictValue* dict = result.value.GetIfDict();
            if (dict && dict->FindString("error")) {
              wait->finished = true;
              wait->call->InvalidArguments(*dict->FindString("error"));
              return;
            }
            if (dict && dict->FindBool("present").value_or(false)) {
              CheckWaitScript(wait);
              return;
            }
            wait->last_error = result.message;
            WaitForLater(wait);
          },
          wait));
}

void PollWaitFor(std::shared_ptr<WaitFor> wait) {
  if (wait->finished || wait->call->cancelled()) {
    return;
  }
  PageProtocol* protocol = wait->protocol.get();
  if (!protocol) {
    wait->finished = true;
    wait->call->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_NOT_FOUND,
                       "the page is closed");
    return;
  }
  // Before any condition: a selector or script that keeps failing must not
  // retry past the deadline.
  if (base::TimeTicks::Now() >= wait->deadline) {
    WaitForTimedOut(wait);
    return;
  }
  content::WebContents& web_contents = protocol->GetWebContents();
  const bool load_ok =
      !wait->load || *wait->load == "committed" || !web_contents.IsLoading();
  const bool url_ok =
      !wait->url ||
      GlobMatch(*wait->url, web_contents.GetLastCommittedURL().spec());
  const bool idle_ok = !wait->network_idle || protocol->NetworkIdle();
  if (load_ok && url_ok && idle_ok) {
    CheckWaitSelector(wait);
    return;
  }
  WaitForLater(wait);
}

void WaitForCommand(const std::shared_ptr<ServiceCall>& c,
                    PageProtocol& protocol) {
  std::string frame_id;
  if (!ResolveFrame(c, protocol, &frame_id)) {
    return;
  }
  auto wait = std::make_shared<WaitFor>();
  wait->call = c;
  wait->protocol = protocol.GetWeakPtr();
  wait->frame_id = frame_id;
  wait->load = c->String("load");
  wait->url = c->String("url");
  wait->selector = c->String("selector");
  wait->script = c->String("script");
  wait->network_idle = c->Bool("networkIdle").value_or(false);
  const double timeout = c->Number("timeoutMs").value_or(kDefaultTimeoutMs);
  wait->deadline = base::TimeTicks::Now() + base::Milliseconds(timeout);
  // The deadline holds even while a check is in flight: a script predicate
  // whose promise never settles does not come back to PollWaitFor.
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce(&WaitForTimedOut, wait),
      base::Milliseconds(timeout));
  // Stops the polling; weak, because the call holds this function and a
  // strong reference would make a cycle.
  c->control().OnCancel([weak = std::weak_ptr<WaitFor>(wait)] {
    if (std::shared_ptr<WaitFor> wait = weak.lock()) {
      wait->finished = true;
    }
  });
  PollWaitFor(wait);
}

// ---------------------------------------------------------------------------
// contents.screenshot, pdf, save
// ---------------------------------------------------------------------------

// The `path` argument, if any: an absolute path with no "..". False, with the
// call rejected, when it is not. `path` is empty when none was given.
bool ReadPath(const std::shared_ptr<ServiceCall>& c, base::FilePath* path) {
  std::optional<std::string> text = c->String("path");
  if (!text || text->empty()) {
    path->clear();
    return true;
  }
  base::FilePath parsed = base::FilePath::FromUTF8Unsafe(*text);
  if (!parsed.IsAbsolute() || parsed.ReferencesParent()) {
    c->InvalidArguments("path must be an absolute path without '..'");
    return false;
  }
  *path = parsed;
  return true;
}

// Answers a capture: the bytes go back with the call, or into the file the
// caller named (then no bytes are sent).
void FinishCapture(std::shared_ptr<ServiceCall> c,
                   std::string mime,
                   base::FilePath path,
                   std::vector<uint8_t> bytes) {
  const std::string head =
      "{\"mimeType\":" + JsonString(mime) +
      ",\"byteLength\":" + base::NumberToString(bytes.size());
  if (path.empty()) {
    c->control().ResolveBinary(
        head + "}",
        std::string(base::as_string_view(base::span<const uint8_t>(bytes))));
    return;
  }
  WriteCaptureFile(path, std::move(bytes),
                   base::BindOnce(
                       [](std::shared_ptr<ServiceCall> c, std::string head,
                          base::FilePath path, bool written) {
                         if (!written) {
                           RejectFailure(c, "the file could not be written");
                           return;
                         }
                         c->Resolve(head + ",\"path\":" +
                                    JsonString(path.AsUTF8Unsafe()) + "}");
                       },
                       c, head, path));
}

void ReplyScreenshot(std::shared_ptr<ServiceCall> c,
                     std::string mime,
                     base::FilePath path,
                     base::ScopedClosureRunner hold,
                     ProtocolResult result) {
  // The page may go back to being hidden.
  hold.RunAndReset();
  const std::string* data =
      result.ok ? result.result.FindString("data") : nullptr;
  std::optional<std::vector<uint8_t>> bytes;
  if (data) {
    bytes = base::Base64Decode(*data);
  }
  if (!bytes) {
    RejectFailure(c, result.error_message.empty()
                         ? "the screenshot could not be read"
                         : result.error_message);
    return;
  }
  FinishCapture(c, mime, path, std::move(*bytes));
}

// Page.captureScreenshot with `params`, then the answer.
void CaptureWithParams(const std::shared_ptr<ServiceCall>& c,
                       PageProtocol& protocol,
                       base::DictValue params,
                       const std::string& mime,
                       const base::FilePath& path) {
  // A page that is hidden is not drawn. Keep it rendering for the capture:
  // stay_hidden false tells the renderer it is visible; nothing is shown on
  // screen and the system is not kept awake.
  base::ScopedClosureRunner hold =
      protocol.GetWebContents().IncrementCapturerCount(
          gfx::Size(), /*stay_hidden=*/false, /*stay_awake=*/false,
          /*is_activity=*/false);
  protocol.Send(
      "", "Page.captureScreenshot", std::move(params),
      base::BindOnce(&ReplyScreenshot, c, mime, path, std::move(hold)));
}

void Screenshot(const std::shared_ptr<ServiceCall>& c, PageProtocol& protocol) {
  base::FilePath path;
  if (!ReadPath(c, &path)) {
    return;
  }
  const std::string area = c->StringOr("area", "visible");
  const std::string format = c->StringOr("format", "png");
  base::DictValue params;
  params.Set("format", format);
  if (format == "jpeg") {
    params.Set("quality",
               std::clamp(static_cast<int>(c->Number("quality").value_or(80)),
                          0, 100));
  }
  params.Set("fromSurface", true);
  const std::string mime = format == "jpeg" ? "image/jpeg" : "image/png";

  if (area == "rect") {
    const base::DictValue* rect = c->Dict("rect");
    const double width = rect ? rect->FindDouble("width").value_or(0) : 0;
    const double height = rect ? rect->FindDouble("height").value_or(0) : 0;
    if (!rect || width <= 0 || height <= 0) {
      c->InvalidArguments("area 'rect' needs a rect with a positive size");
      return;
    }
    base::DictValue clip;
    clip.Set("x", rect->FindDouble("x").value_or(0));
    clip.Set("y", rect->FindDouble("y").value_or(0));
    clip.Set("width", width);
    clip.Set("height", height);
    clip.Set("scale", 1);
    params.Set("clip", std::move(clip));
    params.Set("captureBeyondViewport", true);
    CaptureWithParams(c, protocol, std::move(params), mime, path);
    return;
  }
  if (area == "fullPage") {
    protocol.Send(
        "", "Page.getLayoutMetrics", base::DictValue(),
        base::BindOnce(
            [](std::shared_ptr<ServiceCall> c,
               base::WeakPtr<PageProtocol> protocol, base::DictValue params,
               std::string mime, base::FilePath path, ProtocolResult metrics) {
              const base::DictValue* size =
                  metrics.ok ? metrics.result.FindDict("cssContentSize")
                             : nullptr;
              if (!protocol || !size) {
                RejectFailure(c, "the size of the page could not be read");
                return;
              }
              base::DictValue clip;
              clip.Set("x", 0);
              clip.Set("y", 0);
              clip.Set("width", size->FindDouble("width").value_or(0));
              clip.Set("height", size->FindDouble("height").value_or(0));
              clip.Set("scale", 1);
              params.Set("clip", std::move(clip));
              params.Set("captureBeyondViewport", true);
              CaptureWithParams(c, *protocol, std::move(params), mime, path);
            },
            c, protocol.GetWeakPtr(), std::move(params), mime, path));
    return;
  }
  CaptureWithParams(c, protocol, std::move(params), mime, path);
}

void Pdf(const std::shared_ptr<ServiceCall>& c) {
  content::WebContents* web_contents = LivePage(c);
  if (!web_contents) {
    return;
  }
  base::FilePath path;
  if (!ReadPath(c, &path)) {
    return;
  }
  PdfOptions options;
  options.landscape = c->Bool("landscape");
  options.print_background = c->Bool("printBackground");
  options.scale = c->Number("scale");
  options.paper_width = c->Number("paperWidth");
  options.paper_height = c->Number("paperHeight");
  options.margin_top = c->Number("marginTop");
  options.margin_bottom = c->Number("marginBottom");
  options.margin_left = c->Number("marginLeft");
  options.margin_right = c->Number("marginRight");
  options.page_ranges = c->StringOr("pageRanges", "");
  c->Progress(0, "Printing");
  CapturePdf(
      web_contents, options,
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> c, base::FilePath path,
             std::optional<std::vector<uint8_t>> bytes, std::string error) {
            if (!bytes) {
              // A page range that does not parse is the caller's;
              // anything else is Chromium's.
              if (base::ToLowerASCII(error).find("range") !=
                  std::string::npos) {
                c->InvalidArguments(error);
              } else {
                RejectFailure(c, error);
              }
              return;
            }
            c->Progress(1, "Printed");
            FinishCapture(c, "application/pdf", path, std::move(*bytes));
          },
          c, path));
}

// Waits for the download item Chromium makes for a saved page, which is how
// it reports the save: WebContents::SavePage has no completion callback. The
// next save-package item the profile's download manager creates is this
// call's (a second save in the same profile at the same moment could be
// confused with it). Deletes itself when it has answered.
//
// Chromium completes the item even when the page's own file failed: an
// "HTML only" save fetches the page again, and a page that cannot be fetched
// again (one an interceptor served, say) leaves no file. So a completed save
// succeeds only when the item lists its main file among the files written.
// An MHTML save is written in one piece and lists no files.
class SaveWatch : public content::DownloadManager::Observer,
                  public download::DownloadItem::Observer {
 public:
  SaveWatch(content::DownloadManager* manager,
            std::shared_ptr<ServiceCall> call,
            base::FilePath file,
            bool lists_files)
      : manager_(manager),
        call_(std::move(call)),
        file_(std::move(file)),
        lists_files_(lists_files) {
    manager_->AddObserver(this);
    start_timeout_.Start(
        FROM_HERE, kSaveStartTimeout,
        base::BindOnce(&SaveWatch::TimedOut, base::Unretained(this)));
  }

  ~SaveWatch() override {
    if (item_) {
      item_->RemoveObserver(this);
    }
    if (manager_) {
      manager_->RemoveObserver(this);
    }
  }

  base::WeakPtr<SaveWatch> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // The call was cancelled: stop watching. The save itself carries on.
  void Abandon() { Finish(std::string(), /*answer=*/false); }
  // Answers the call with a failure now.
  void Fail(const std::string& error) { Finish(error); }

  // content::DownloadManager::Observer:
  void OnDownloadCreated(content::DownloadManager*,
                         download::DownloadItem* item) override {
    // Another save in the profile may start at the same time: its item is
    // not this one's. A save item's target is the file SavePage was given.
    if (item_ || !item->IsSavePackageDownload() ||
        item->GetTargetFilePath() != file_) {
      return;
    }
    item_ = item;
    item_->AddObserver(this);
    start_timeout_.Stop();
    OnDownloadUpdated(item_);
  }
  void ManagerGoingDown(content::DownloadManager*) override {
    manager_ = nullptr;
    Finish("the page was not saved: the profile closed");
  }

  // download::DownloadItem::Observer:
  void OnDownloadUpdated(download::DownloadItem* item) override {
    switch (item->GetState()) {
      case download::DownloadItem::COMPLETE:
        Finish(WroteTheFile(item) ? std::string()
                                  : "the page was not saved: its file could "
                                    "not be written");
        return;
      case download::DownloadItem::CANCELLED:
      case download::DownloadItem::INTERRUPTED:
        Finish("the page was not saved");
        return;
      case download::DownloadItem::IN_PROGRESS:
      case download::DownloadItem::MAX_DOWNLOAD_STATE:
        return;
    }
  }
  void OnDownloadDestroyed(download::DownloadItem* item) override {
    item->RemoveObserver(this);
    item_ = nullptr;
    Finish("the page was not saved");
  }

 private:
  void TimedOut() { Finish("the page was not saved: Chromium did not start"); }

  bool WroteTheFile(download::DownloadItem* item) const {
    if (!lists_files_) {
      return true;
    }
    const std::vector<download::DownloadSaveItemData::ItemInfo>* files =
        download::DownloadSaveItemData::GetItemData(item);
    const base::FilePath& main = item->GetTargetFilePath();
    return files && std::ranges::any_of(*files, [&main](const auto& info) {
             return info.file_path == main;
           });
  }

  // Answers once (an empty `error` is success) and has this deleted later,
  // not from inside the observer call that got here.
  void Finish(const std::string& error, bool answer = true) {
    if (done_) {
      return;
    }
    done_ = true;
    start_timeout_.Stop();
    if (answer) {
      if (error.empty()) {
        call_->ResolveNull();
      } else {
        call_->Reject(BDK_DOMAIN_PLATFORM, "failed", error);
      }
    }
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  raw_ptr<content::DownloadManager> manager_;
  raw_ptr<download::DownloadItem> item_ = nullptr;
  std::shared_ptr<ServiceCall> call_;
  const base::FilePath file_;
  const bool lists_files_;
  base::OneShotTimer start_timeout_;
  bool done_ = false;
  base::WeakPtrFactory<SaveWatch> weak_factory_{this};
};

void Save(const std::shared_ptr<ServiceCall>& c) {
  content::WebContents* web_contents = LivePage(c);
  if (!web_contents) {
    return;
  }
  const std::optional<std::string> text = c->String("path");
  const base::FilePath file = base::FilePath::FromUTF8Unsafe(text.value_or(""));
  if (!file.IsAbsolute() || file.ReferencesParent()) {
    c->InvalidArguments("path must be an absolute path without '..'");
    return;
  }
  const std::string format = c->StringOr("format", "completePage");
  content::SavePageType type = content::SAVE_PAGE_TYPE_AS_COMPLETE_HTML;
  if (format == "htmlOnly") {
    type = content::SAVE_PAGE_TYPE_AS_ONLY_HTML;
  } else if (format == "singleFile") {
    type = content::SAVE_PAGE_TYPE_AS_MHTML;
  }
  if (!web_contents->IsSavable()) {
    c->Reject(BDK_DOMAIN_PLATFORM, "failed", "this page cannot be saved");
    return;
  }
  // Chromium wants a directory for the page's files even for the types that
  // write none: "<name>_files" next to the file, as Chrome names it.
  const base::FilePath directory =
      base::FilePath(file.RemoveExtension().value() + "_files");
  auto* watch =
      new SaveWatch(web_contents->GetBrowserContext()->GetDownloadManager(), c,
                    file, type != content::SAVE_PAGE_TYPE_AS_MHTML);
  c->control().OnCancel([weak = watch->GetWeakPtr()] {
    if (weak) {
      weak->Abandon();
    }
  });
  if (!web_contents->SavePage(file, directory, type)) {
    // A save that did not start has no download item to wait for.
    watch->Fail("the page could not be saved");
  }
}

}  // namespace

content::WebContents* LivePage(const std::shared_ptr<ServiceCall>& c) {
  const std::shared_ptr<bdkcore::Contents>& contents = c->builtin().contents;
  if (!contents) {
    c->InvalidArguments("this command needs a page");
    return nullptr;
  }
  if (contents->dead || contents->closed || !contents->data) {
    c->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_NOT_FOUND, "the page is closed");
    return nullptr;
  }
  content::WebContents* web_contents =
      EngineContents::From(*contents)->web_contents();
  content::RenderFrameHost* main = web_contents->GetPrimaryMainFrame();
  if (main && !MayInspectFrame(main)) {
    c->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_INTERNAL_PAGE,
              "an internal page cannot be scripted");
    return nullptr;
  }
  return web_contents;
}

void AddContentsHandlers(HandlerTable& table) {
  table["contents.waitFor"] = &Run<&WaitForCommand>;
  table["contents.frames"] = &Frames;
  table["contents.text"] = &Run<&Text>;
  table["contents.snapshot"] = &Run<&Snapshot>;
  table["contents.evaluate"] = &Run<&Evaluate>;
  table["contents.query"] = &Run<&Query>;
  table["contents.insertCSS"] = &Run<&InsertCss>;
  table["contents.removeCSS"] = &Run<&RemoveCss>;
  table["contents.click"] = &Run<&Click>;
  table["contents.hover"] = &Run<&Hover>;
  table["contents.type"] = &Run<&Type>;
  table["contents.press"] = &Run<&Press>;
  table["contents.scroll"] = &Run<&Scroll>;
  table["contents.screenshot"] = &Run<&Screenshot>;
  table["contents.pdf"] = &Pdf;
  table["contents.save"] = &Save;
}

}  // namespace bdk
