// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/interstitial_prompts.h"

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/services/password_prompts.h"
#include "bdk/engine/services/ui_requests.h"
#include "components/safe_browsing/content/browser/safe_browsing_blocking_page.h"
#include "components/security_interstitials/content/security_interstitial_page.h"
#include "components/security_interstitials/content/security_interstitial_tab_helper.h"
#include "components/security_interstitials/content/ssl_blocking_page.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "net/base/net_errors.h"
#include "net/cert/x509_certificate.h"
#include "net/ssl/ssl_info.h"

namespace bdk {

namespace {

using security_interstitials::SecurityInterstitialPage;

// The commands the interstitial pages take (SecurityInterstitialCommand).
constexpr char kCommandDontProceed[] = "0";
constexpr char kCommandProceed[] = "1";

// The request raised for a page's current interstitial, so that the next
// navigation can withdraw it.
std::map<content::WebContents*, std::shared_ptr<bdkcore::Request>>& Open() {
  static base::NoDestructor<
      std::map<content::WebContents*, std::shared_ptr<bdkcore::Request>>>
      open;
  return *open;
}

SecurityInterstitialPage* CommittedInterstitial(
    content::WebContents* contents) {
  auto* helper =
      security_interstitials::SecurityInterstitialTabHelper::FromWebContents(
          contents);
  return helper
             ? helper
                   ->GetBlockingPageForCurrentlyCommittedNavigationForTesting()
             : nullptr;
}

// A boolean the page put in its load-time data ("overridable": true). Chromium
// does not expose it elsewhere, and it is the value the page's own proceed
// button obeys; the page enforces it again when told to proceed.
bool FlagInPage(const std::string& html, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  for (size_t at = html.find(needle); at != std::string::npos;
       at = html.find(needle, at + 1)) {
    size_t value = at + needle.size();
    while (value < html.size() && html[value] == ' ') {
      ++value;
    }
    if (html.compare(value, 4, "true") == 0) {
      return true;
    }
  }
  return false;
}

std::string CertificateErrorName(int net_error) {
  switch (net_error) {
    case net::ERR_CERT_DATE_INVALID:
      return "dateInvalid";
    case net::ERR_CERT_AUTHORITY_INVALID:
      return "authorityInvalid";
    case net::ERR_CERT_COMMON_NAME_INVALID:
      return "commonNameInvalid";
    case net::ERR_CERT_REVOKED:
      return "revoked";
    case net::ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN:
      return "pinningFailure";
    default:
      return net::ErrorToShortString(net_error);
  }
}

base::DictValue CertificateSummary(const net::SSLInfo& info) {
  base::DictValue summary;
  if (info.cert) {
    summary.Set("id",
                base::HexEncode(info.cert->CalculateChainFingerprint256()));
    summary.Set("subject", info.cert->subject().GetDisplayName());
    summary.Set("issuer", info.cert->issuer().GetDisplayName());
  }
  return summary;
}

void Raise(base::WeakPtr<content::WebContents> weak_contents,
           GURL url,
           int net_error,
           std::optional<net::SSLInfo> ssl_info) {
  content::WebContents* contents = weak_contents.get();
  if (!contents) {
    return;
  }
  EngineContents* page = FindEngineContents(contents);
  SecurityInterstitialPage* interstitial = CommittedInterstitial(contents);
  if (!page || !interstitial) {
    return;
  }
  const bool is_certificate =
      interstitial->GetTypeForTesting() == SSLBlockingPage::kTypeForTesting;
  const bool is_reputation =
      interstitial->GetTypeForTesting() ==
      safe_browsing::SafeBrowsingBlockingPage::kTypeForTesting;
  if (!is_certificate && !is_reputation) {
    return;  // another interstitial: Chromium's page is all there is
  }
  const std::string html = interstitial->GetHTMLContents();
  const bool overridable = FlagInPage(html, "overridable");
  base::DictValue payload;
  payload.Set("url", url.spec());
  payload.Set("isOverridable", overridable);
  uint32_t surface;
  if (is_certificate) {
    surface = BDK_SURFACE_CERTIFICATE_ERRORS;
    const std::string error = CertificateErrorName(net_error);
    payload.Set("error", error);
    payload.Set("certificate",
                ssl_info ? CertificateSummary(*ssl_info) : base::DictValue());
    payload.Set(
        "text",
        base::DictValue()
            .Set("title", "Your connection is not private")
            .Set("message", "The certificate of " + std::string(url.host()) +
                                " could not be verified (" + error + ")."));
  } else {
    surface = BDK_SURFACE_REPUTATION_WARNINGS;
    const bool phishing = FlagInPage(html, "phishing");
    payload.Set("verdict", phishing ? "phishing" : "malware");
    payload.Set("provider", base::DictValue().Set("name", "safe-browsing"));
    payload.Set("text",
                base::DictValue()
                    .Set("title", phishing ? "Deceptive site ahead"
                                           : "Dangerous site ahead")
                    .Set("message",
                         std::string(url.host()) + " is reported as " +
                             (phishing ? "a phishing site." : "malicious.")));
  }
  base::WeakPtr<content::WebContents> weak = contents->GetWeakPtr();
  Open()[contents] = RaiseUiRequest(
      *page, surface, payload,
      [weak, url](bdkcore::UiOutcome outcome, const std::string& completion,
                  const std::string&) {
        // Whatever does not say "proceed" goes back (the router already turns
        // a rejection, a dropped request and a released one into "go_back").
        // A withdrawn request means the page moved on by itself.
        if (!weak || outcome == bdkcore::UiOutcome::kWithdrawn ||
            outcome == bdkcore::UiOutcome::kDefaulted) {
          return;  // Chromium's page stays; nothing was decided
        }
        SecurityInterstitialPage* page = CommittedInterstitial(weak.get());
        if (!page || weak->GetLastCommittedURL() != url) {
          return;
        }
        const bool proceed = outcome == bdkcore::UiOutcome::kCompleted &&
                             completion == "proceed";
        // The page refuses a proceed it does not allow (HSTS, pinning) as well.
        page->CommandReceived(proceed ? kCommandProceed : kCommandDontProceed);
      });
}

}  // namespace

void OnPrimaryNavigationFinished(EngineContents& page,
                                 content::NavigationHandle* handle) {
  content::WebContents* contents = page.web_contents();
  SchedulePasswordPromptCheck(contents);
  auto previous = Open().find(contents);
  if (previous != Open().end()) {
    bdkcore::withdraw_ui_request(previous->second);
    Open().erase(previous);
  }
  if (!handle->HasCommitted() || !handle->IsErrorPage()) {
    return;
  }
  // The interstitial is associated with the navigation while it finishes;
  // raise once it is settled.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&Raise, contents->GetWeakPtr(), handle->GetURL(),
                     static_cast<int>(handle->GetNetErrorCode()),
                     handle->GetSSLInfo()));
}

}  // namespace bdk
