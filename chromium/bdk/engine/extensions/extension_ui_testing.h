// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSION_UI_TESTING_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSION_UI_TESTING_H_

#include <string>

#include "base/values.h"

namespace bdk {

// The `extui.*` commands of bdk_testing_command (bdk_testing.h), which drive
// and inspect the extension UI: loading an unpacked fixture, reading what
// Chromium granted, closing a popup as a blur would. Returns false when the
// command is unknown or its arguments are bad.
bool RunExtensionUiTestCommand(const std::string& name,
                               const base::DictValue& args,
                               base::DictValue& result);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSION_UI_TESTING_H_
