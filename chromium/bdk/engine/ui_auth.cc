// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_auth.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "content/public/browser/web_contents.h"
#include "net/base/auth.h"

namespace bdk {

namespace {

class BdkLoginDelegate : public content::LoginDelegate {
 public:
  BdkLoginDelegate(const net::AuthChallengeInfo& auth_info,
                   base::WeakPtr<EngineContents> page,
                   content::LoginDelegate::LoginAuthRequiredCallback callback)
      : page_(std::move(page)), callback_(std::move(callback)) {
    base::DictValue payload;
    payload.Set("challenger", auth_info.challenger.Serialize());
    if (!auth_info.realm.empty()) {
      payload.Set("realm", auth_info.realm);
    }
    payload.Set("isProxy", auth_info.is_proxy);
    payload.Set("scheme", auth_info.scheme);
    // BDK has no saved credentials for a challenge until its password
    // manager (T-UIR-04) offers them.
    payload.Set("savedCredentialsAvailable", false);
    payload_ = std::move(payload);
    // Never from inside the network stack's own call.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&BdkLoginDelegate::Raise, weak_factory_.GetWeakPtr()));
  }

  ~BdkLoginDelegate() override {
    // The challenge went away (the load was cancelled, or the page closed).
    if (request_) {
      bdkcore::withdraw_ui_request(*request_);
    }
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
  }

 private:
  void Raise() {
    if (!page_) {
      Answer(std::nullopt);
      return;
    }
    request_ = RaiseUiRequest(
        page_->Core(), BDK_SURFACE_HTTP_AUTH, payload_,
        base::BindOnce(&BdkLoginDelegate::OnEnded, weak_factory_.GetWeakPtr()),
        &page_->host_view());
    if (!request_) {
      Answer(std::nullopt);
    }
  }

  void OnEnded(const UiResult& result) {
    switch (result.outcome) {
      case bdkcore::UiOutcome::kCompleted: {
        const std::string* user = result.args.FindString("username");
        const std::string* password = result.args.FindString("password");
        if (result.completion == "accept" && user && password) {
          Answer(AuthAnswer{*user, *password});
        } else {
          Answer(std::nullopt);
        }
        return;
      }
      case bdkcore::UiOutcome::kDefaulted:
        dismiss_ = UiPresenters::Get().ShowAuthPrompt(
            &page_->host_view(), payload_,
            base::BindOnce(&BdkLoginDelegate::Answer,
                           weak_factory_.GetWeakPtr()));
        return;
      case bdkcore::UiOutcome::kWithdrawn:
        return;  // the delegate is going; nothing waits for an answer
      default:
        Answer(std::nullopt);
        return;
    }
  }

  void Answer(std::optional<AuthAnswer> answer) {
    request_.reset();
    if (!callback_) {
      return;
    }
    if (answer) {
      std::move(callback_).Run(
          net::AuthCredentials(base::UTF8ToUTF16(answer->username),
                               base::UTF8ToUTF16(answer->password)));
    } else {
      std::move(callback_).Run(std::nullopt);
    }
  }

  base::WeakPtr<EngineContents> page_;
  content::LoginDelegate::LoginAuthRequiredCallback callback_;
  base::DictValue payload_;
  std::shared_ptr<bdkcore::Request> request_;
  Dismisser dismiss_;
  base::WeakPtrFactory<BdkLoginDelegate> weak_factory_{this};
};

}  // namespace

std::unique_ptr<content::LoginDelegate> CreateBdkLoginDelegate(
    const net::AuthChallengeInfo& auth_info,
    content::WebContents* web_contents,
    bool do_not_prompt_for_login,
    content::LoginDelegate::LoginAuthRequiredCallback callback) {
  EngineContents* page =
      web_contents ? EngineContents::FromWebContents(web_contents) : nullptr;
  if (do_not_prompt_for_login) {
    page = nullptr;
  }
  return std::make_unique<BdkLoginDelegate>(
      auth_info, page ? page->GetWeakPtr() : base::WeakPtr<EngineContents>(),
      std::move(callback));
}

}  // namespace bdk
