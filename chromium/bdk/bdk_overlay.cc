// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/bdk_overlay.h"

int bdk_overlay_marker(void) {
  // Not 1: the linker folds identical functions, and bdk_abi_version() also
  // returns 1. Two exports at one address break the framework's export-order
  // check.
  return 0x42444b;  // "BDK"
}
