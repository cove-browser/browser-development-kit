// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/presenter_answers.h"

#include <map>

#include "base/no_destructor.h"

namespace bdk {

namespace {

struct State {
  std::map<std::string, std::string> answers;
  std::map<std::string, int> uses;
};

State& GetState() {
  static base::NoDestructor<State> state;
  return *state;
}

}  // namespace

void SetPresenterAnswerForTesting(const std::string& surface,
                                  const std::string& answer) {
  if (answer.empty()) {
    GetState().answers.erase(surface);
  } else {
    GetState().answers[surface] = answer;
  }
}

int PresenterUseCountForTesting(const std::string& surface) {
  auto it = GetState().uses.find(surface);
  return it == GetState().uses.end() ? 0 : it->second;
}

std::optional<std::string> TakePresenterAnswer(const std::string& surface) {
  ++GetState().uses[surface];
  auto it = GetState().answers.find(surface);
  if (it == GetState().answers.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace bdk
