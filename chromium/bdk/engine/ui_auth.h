// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_AUTH_H_
#define BDK_ENGINE_UI_AUTH_H_

#include <memory>

#include "content/public/browser/login_delegate.h"

namespace content {
class WebContents;
}  // namespace content

namespace net {
class AuthChallengeInfo;
}  // namespace net

namespace bdk {

// The login delegate of an HTTP or proxy authentication challenge: raises an
// `http_auth` UI request for the page (ui-requests.md section 5.3) and answers
// the network stack with what comes back. A challenge for no page of the
// engine, or one the network stack does not want prompted, is cancelled.
// Destroying the delegate (the request went away) withdraws the request.
std::unique_ptr<content::LoginDelegate> CreateBdkLoginDelegate(
    const net::AuthChallengeInfo& auth_info,
    content::WebContents* web_contents,
    bool do_not_prompt_for_login,
    content::LoginDelegate::LoginAuthRequiredCallback callback);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_AUTH_H_
