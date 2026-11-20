// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_PASSWORD_PROMPTS_H_
#define BDK_ENGINE_SERVICES_PASSWORD_PROMPTS_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/ui/views/tab_dialogs_views.h"
#include "content/public/browser/web_contents.h"

namespace bdk {

// The TabDialogs of a BDK page. Everything is Chromium's but the passwords
// bubble: the save and update prompt of the password manager becomes a UI
// request of the `passwords` surface (ui-requests.md 5.8), answered by the
// application or by a small native sheet.
class BdkTabDialogs : public TabDialogsViews {
 public:
  BdkTabDialogs(tabs::TabInterface& tab, content::WebContents* contents);
  ~BdkTabDialogs() override;

  // The tab finished a navigation: a new page may ask a new question, and the
  // password manager may already have one pending.
  void OnPageNavigated();

  // TabDialogs:
  void ShowManagePasswordsBubble(bool user_action) override;
  void HideManagePasswordsBubble() override;

 private:
  // What the user (or the application on their behalf) chose.
  enum class Choice { kSave, kNotNow, kNever, kNoInteraction };

  void Finished(bool is_update, const UiResult& result);
  // Gives the password manager the answer, if it still waits for one (the
  // page may have moved on while a sheet was open), and closes the bubble.
  void Apply(bool is_update, Choice choice);

  base::WeakPtr<content::WebContents> contents_;
  std::shared_ptr<bdkcore::Request> request_;
  // The current question was answered (the password manager's state may stay
  // pending a moment longer).
  bool answered_ = false;
  base::WeakPtrFactory<BdkTabDialogs> weak_factory_{this};
};

// Tells the tab's password prompts that its page finished a navigation.
void OnPageNavigatedForPasswords(content::WebContents* contents);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_PASSWORD_PROMPTS_H_
