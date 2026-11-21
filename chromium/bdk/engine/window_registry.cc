// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/window_registry.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/callback.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/host_context.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/json_util.h"

namespace bdk {

// static
WindowRegistry& WindowRegistry::Get() {
  static base::NoDestructor<WindowRegistry> registry;
  return *registry;
}

WindowRegistry::WindowRegistry() = default;
WindowRegistry::~WindowRegistry() = default;

void WindowRegistry::NotePage(bdk_contents_t contents,
                              const bdkcore::Window* window) {
  ForgetPage(contents);
  if (window) {
    arrivals_.push_back({contents, window});
  }
}

void WindowRegistry::ForgetPage(bdk_contents_t contents) {
  std::erase_if(arrivals_, [contents](const Arrival& arrival) {
    return arrival.contents == contents;
  });
}

void WindowRegistry::NoteActive(const bdkcore::Window* window,
                                const HostContext* context) {
  std::erase_if(active_, [window](const Active& active) {
    return active.window == window;
  });
  active_.push_back({window, context});
}

void WindowRegistry::ForgetContext(const HostContext* context) {
  std::erase_if(active_, [context](const Active& active) {
    return active.context == context;
  });
}

void WindowRegistry::ForgetWindow(const bdkcore::Window* window) {
  std::erase_if(arrivals_, [window](const Arrival& arrival) {
    return arrival.window == window;
  });
  std::erase_if(active_, [window](const Active& active) {
    return active.window == window;
  });
}

bdk_window_t WindowRegistry::WindowOfPage(bdk_contents_t contents) const {
  for (const Arrival& arrival : arrivals_) {
    if (arrival.contents == contents) {
      return arrival.window->handle;
    }
  }
  return 0;
}

std::vector<bdk_contents_t> WindowRegistry::PagesOf(
    const bdkcore::Window* window) const {
  std::vector<bdk_contents_t> pages;
  for (const Arrival& arrival : arrivals_) {
    if (arrival.window == window) {
      pages.push_back(arrival.contents);
    }
  }
  return pages;
}

HostContext* WindowRegistry::ActiveContext(
    const bdkcore::Window* window) const {
  std::vector<HostContext*> contexts =
      HostContexts::Get().ContextsOfWindow(window);
  if (contexts.empty()) {
    return nullptr;
  }
  for (const Active& active : active_) {
    if (active.window != window) {
      continue;
    }
    for (HostContext* context : contexts) {
      if (context == active.context) {
        return context;
      }
    }
  }
  // None was activated: the newest context, as the list keeps them in
  // creation order.
  return contexts.back();
}

std::optional<std::array<double, 4>> WindowAnchor(const bdkcore::Window* window,
                                                  uint32_t role) {
  double rect[4] = {0, 0, 0, 0};
  if (!window || !bdkcore::window_anchor(*window, role, rect)) {
    return std::nullopt;
  }
  return std::array<double, 4>{rect[0], rect[1], rect[2], rect[3]};
}

std::optional<std::array<double, 4>> ViewWindowAnchor(HostView* view,
                                                      uint32_t role) {
  if (!view) {
    return std::nullopt;
  }
  return WindowAnchor(HostContexts::Get().FindWindow(view->Window()), role);
}

void RaiseWindowRequest(bdk_window_t window,
                        base::DictValue request,
                        base::OnceCallback<void(bool apply)> done) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  std::shared_ptr<bdkcore::Window> registered = bdkcore::window_of(window);
  if (!rt || !registered) {
    return;
  }
  // The core's callback is copyable; the answer runs once.
  auto answer =
      std::make_shared<base::OnceCallback<void(bool)>>(std::move(done));
  bdkcore::window_request(
      rt, registered, ToJson(request), [answer](bdk_ui_response_t response) {
        if (*answer) {
          std::move(*answer).Run(response == BDK_UI_USE_DEFAULT);
        }
      });
}

base::DictValue WindowRequest(const char* kind) {
  base::DictValue request;
  request.Set("kind", kind);
  return request;
}

}  // namespace bdk
