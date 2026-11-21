// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSION_UI_UTIL_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSION_UI_UTIL_H_

#include <memory>
#include <string>

#include "bdk/core/include/bdk/bdk_base.h"
#include "bdk/core/runtime/core_internal.h"
#include "third_party/skia/include/core/SkColor.h"

class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace extensions {
class Extension;
}  // namespace extensions

namespace gfx {
class Image;
}  // namespace gfx

namespace bdk {

// What the extension UI of the engine (action state, popups, the default menu,
// context-menu items, options pages) shares.

// The extension `id` when it is installed and enabled in `profile`, else null.
const extensions::Extension* EnabledExtension(Profile* profile,
                                              const std::string& id);

// The tab identifier extensions use for `contents` (what tabs.query reports).
int ExtensionTabId(content::WebContents* contents);

// "data:image/png;base64,..." for `image`, or an empty string when it has no
// pixels.
std::string PngDataUrl(const gfx::Image& image);

// "#RRGGBBAA", the way the interface writes a colour.
std::string ColorHex(SkColor color);

// The BDK profile that wraps the Chromium `profile`, or null when the
// application has not opened it.
std::shared_ptr<bdkcore::Profile> BdkProfileOf(bdkcore::Runtime& rt,
                                               Profile* profile);

// The window the application registered for the native window `native`, or
// null.
std::shared_ptr<bdkcore::Window> RegisteredWindowOf(
    bdkcore::Runtime& rt,
    const bdk_native_handle_t& native);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSION_UI_UTIL_H_
