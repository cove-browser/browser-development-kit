// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_PRESENTER_ANSWERS_H_
#define BDK_ENGINE_SERVICES_PRESENTER_ANSWERS_H_

#include <optional>
#include <string>

namespace bdk {

// A test can answer a default presenter instead of letting it show native UI
// (bdk_testing_control, "presenter"). Surfaces are named as in [ui.surfaces]
// ("passwords", "downloads", ...). An answer stays until it is replaced or
// cleared (set to an empty string).
void SetPresenterAnswerForTesting(const std::string& surface,
                                  const std::string& answer);

// How many times the default presenter of `surface` ran since the process
// began, with or without an answer.
int PresenterUseCountForTesting(const std::string& surface);

// Called by a default presenter when it is about to show: counts the use and
// returns the test's answer, if any.
std::optional<std::string> TakePresenterAnswer(const std::string& surface);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_PRESENTER_ANSWERS_H_
