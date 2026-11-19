// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_BDK_OVERLAY_H_
#define BDK_BDK_OVERLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

// Returns a constant. Its only job is to exist: `chromium:build`
// runs `nm` on the built framework and fails if this symbol is missing, which
// proves that //bdk was compiled and linked into the framework. The symbol is
// also listed in chrome/app/framework.exports by the series, because the
// framework hides every symbol that is not listed there.
__attribute__((visibility("default"))) int bdk_overlay_marker(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BDK_BDK_OVERLAY_H_
