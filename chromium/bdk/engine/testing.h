// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_TESTING_H_
#define BDK_ENGINE_TESTING_H_

#include <memory>

namespace content {
class URLLoaderRequestInterceptor;
}  // namespace content

namespace bdk {

// The interceptor that serves navigations the test hooks scripted
// (bdk_testing.h), or null when nothing is scripted.
std::unique_ptr<content::URLLoaderRequestInterceptor>
CreateScriptedNavigationInterceptor();

// Counts BDK tasks posted to the UI thread and not yet run, for
// bdk_testing_pending_work().
void NoteTaskPosted();
void NoteTaskRun();

// Implemented by the backend (engine_backend.cc): the next runtime starts with
// no persistent profiles, as if its data directory were new.
void ForgetPersistentProfilesForTesting();

}  // namespace bdk

#endif  // BDK_ENGINE_TESTING_H_
