// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_PANEL_TESTING_H_
#define BDK_ENGINE_EXTENSIONS_PANEL_TESTING_H_

#include <optional>
#include <string>

#include "base/values.h"

namespace bdk {

// The test commands for windows, side panels, anchored prompts and the
// extension fixtures (bdk_testing.h, bdk_testing_command). Returns nothing
// when `name` is not one of them, else whether the command and its arguments
// were good; `result` is what the command answers.
std::optional<bool> RunExtensionPanelCommand(const std::string& name,
                                             const base::DictValue& args,
                                             base::DictValue& result);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_PANEL_TESTING_H_
