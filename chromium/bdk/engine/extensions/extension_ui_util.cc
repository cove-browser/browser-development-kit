// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_ui_util.h"

#include <optional>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/strings/stringprintf.h"
#include "bdk/engine/engine_contents.h"
#include "chrome/browser/profiles/profile.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/image/image_skia_rep.h"

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
  // Encoded from the pixels as they are now. gfx::Image::As1xPNGBytes keeps
  // its first encoding in the image, and Chromium hands out the same image
  // for an extension's default action icon before and after the file loads
  // (ExtensionActionIconFactory caches it; IconImage fills its ImageSkia in
  // place): the icon would stay the blank placeholder for good.
  const gfx::ImageSkiaRep rep = image.AsImageSkia().GetRepresentation(1.0f);
  if (rep.is_null() || rep.scale() != 1.0f) {
    return std::string();
  }
  std::optional<std::vector<uint8_t>> png = gfx::PNGCodec::EncodeBGRASkBitmap(
      rep.GetBitmap(), /*discard_transparency=*/false);
  if (!png || png->empty()) {
    return std::string();
  }
  return "data:image/png;base64," + base::Base64Encode(*png);
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
