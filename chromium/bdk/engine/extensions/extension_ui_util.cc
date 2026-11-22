// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_ui_util.h"

#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/memory/ref_counted_memory.h"
#include "base/strings/stringprintf.h"
#include "bdk/engine/engine_contents.h"
#include "chrome/browser/profiles/profile.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "ui/gfx/image/image.h"

namespace bdk {

const extensions::Extension* EnabledExtension(Profile* profile,
                                              const std::string& id) {
  extensions::ExtensionRegistry* registry =
      extensions::ExtensionRegistry::Get(profile);
  return registry ? registry->enabled_extensions().GetByID(id) : nullptr;
}

int ExtensionTabId(content::WebContents* contents) {
  return sessions::SessionTabHelper::IdForTab(contents).id();
}

std::string PngDataUrl(const gfx::Image& image) {
  if (image.IsEmpty()) {
    return std::string();
  }
  scoped_refptr<base::RefCountedMemory> png = image.As1xPNGBytes();
  if (!png || png->size() == 0) {
    return std::string();
  }
  return "data:image/png;base64," +
         base::Base64Encode(base::span<const uint8_t>(*png));
}

std::string ColorHex(SkColor color) {
  return base::StringPrintf("#%02X%02X%02X%02X", SkColorGetR(color),
                            SkColorGetG(color), SkColorGetB(color),
                            SkColorGetA(color));
}

std::shared_ptr<bdkcore::Profile> BdkProfileOf(bdkcore::Runtime& rt,
                                               Profile* profile) {
  for (const std::shared_ptr<bdkcore::Profile>& p : rt.profiles) {
    auto* data = static_cast<EngineProfile*>(p->data.get());
    if (data && data->profile == profile) {
      return p;
    }
  }
  return nullptr;
}

std::shared_ptr<bdkcore::Window> RegisteredWindowOf(
    bdkcore::Runtime& rt,
    const bdk_native_handle_t& native) {
  if (!native.pointer) {
    return nullptr;
  }
  for (const std::shared_ptr<bdkcore::Window>& w : rt.windows) {
    if (w->native.kind == native.kind && w->native.pointer == native.pointer) {
      return w;
    }
  }
  return nullptr;
}

}  // namespace bdk
