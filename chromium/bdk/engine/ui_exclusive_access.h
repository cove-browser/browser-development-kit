// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_EXCLUSIVE_ACCESS_H_
#define BDK_ENGINE_UI_EXCLUSIVE_ACCESS_H_

namespace bdk {

class EngineContents;

// The exclusive_access notice (ui-requests.md 5.12, T-UIR-15) of a page that
// has just locked the pointer or, in fullscreen, the keyboard: {kind
// ("pointerLock", "keyboardLock"), origin, exitInstruction}, the instruction
// in Chromium's words. It is a notice the application acknowledges; its
// default presenter shows the instruction across the top of the page's
// window for a few seconds. `escape_held` is a keyboard lock that includes
// Escape, which then exits only when held.
enum class ExclusiveAccessKind { kPointerLock, kKeyboardLock };
void NoteExclusiveAccess(EngineContents& page,
                         ExclusiveAccessKind kind,
                         bool escape_held);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_EXCLUSIVE_ACCESS_H_
