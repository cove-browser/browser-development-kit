// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/providers/reputation_service.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "base/values.h"
#include "bdk/core/runtime/providers_internal.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/providers/provider_settings.h"
#include "chrome/browser/safe_browsing/safe_browsing_service.h"
#include "chrome/browser/safe_browsing/services_delegate.h"
#include "components/safe_browsing/buildflags.h"
#include "components/safe_browsing/content/browser/safe_browsing_service_interface.h"
#include "components/safe_browsing/core/browser/db/database_manager.h"
#include "components/safe_browsing/core/browser/db/sb_protocol_manager_util.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "url/gurl.h"

namespace bdk {

namespace {

using safe_browsing::SBThreatType;

SBThreatType ThreatOf(const std::string& verdict) {
  if (verdict == "malware") {
    return SBThreatType::SB_THREAT_TYPE_URL_MALWARE;
  }
  if (verdict == "phishing") {
    return SBThreatType::SB_THREAT_TYPE_URL_PHISHING;
  }
  if (verdict == "unwantedSoftware") {
    return SBThreatType::SB_THREAT_TYPE_URL_UNWANTED;
  }
  return SBThreatType::SB_THREAT_TYPE_SAFE;
}

// Answers Chromium's Safe Browsing checks from the core: the default
// provider's lists, or the application's check_url. Every check runs on the UI
// thread, where Safe Browsing calls the database manager. Download, extension,
// subresource and allowlist checks answer "safe" or "allowlisted": BDK's lists
// hold only page URLs, and an allowlist match keeps Chromium from asking
// Google in real time (the real-time lookups need enhanced protection, which
// ApplyPrivacyPrefs turns off, or Chrome branding).
class BdkDatabaseManager : public safe_browsing::SafeBrowsingDatabaseManager {
 public:
  BdkDatabaseManager()
      : safe_browsing::SafeBrowsingDatabaseManager(
            content::GetUIThreadTaskRunner({})) {}

  // SafeBrowsingDatabaseManager:
  void CancelCheck(Client* client) override {
    auto it = pending_.find(client);
    if (it == pending_.end()) {
      return;
    }
    if (it->second) {
      it->second->Cancel();
    }
    pending_.erase(it);
  }

  bool CanCheckUrl(const GURL& url) const override {
    return url.SchemeIsHTTPOrHTTPS();
  }

  bool CheckBrowseUrl(const GURL& url,
                      const safe_browsing::SBThreatTypeSet& threat_types,
                      Client* client,
                      safe_browsing::CheckBrowseUrlType check_type) override {
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    if (!rt || !CanCheckUrl(url)) {
      return true;
    }
    if (bdkcore::provider_is_app(*rt, bdkcore::Service::kReputation)) {
      return AskApplication(rt, url, threat_types, client);
    }
    const bdkcore::ReputationList* list = bdkcore::reputation_current_list(*rt);
    if (!list) {
      return true;
    }
    SBThreatType threat = ThreatOf(bdkcore::reputation_list_lookup(
        *list, url.spec(), base::Time::Now().InMillisecondsSinceUnixEpoch()));
    if (threat == SBThreatType::SB_THREAT_TYPE_SAFE ||
        !threat_types.contains(threat)) {
      return true;
    }
    // A verdict is answered later, as Chromium expects of a check that
    // returned false.
    pending_[client] = nullptr;
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&BdkDatabaseManager::Answer, weak_factory_.GetWeakPtr(),
                       client, url, threat));
    return false;
  }

  safe_browsing::AsyncMatch CheckCsdAllowlistUrl(const GURL& url,
                                                 Client* client) override {
    return safe_browsing::AsyncMatch::MATCH;
  }

  bool CheckDownloadUrl(const std::vector<GURL>& url_chain,
                        Client* client) override {
    return true;
  }

  bool CheckExtensionIDs(const std::set<std::string>& extension_ids,
                         Client* client) override {
    return true;
  }

  void CheckUrlForHighConfidenceAllowlist(
      const GURL& url,
      CheckUrlForHighConfidenceAllowlistCallback callback) override {
    std::move(callback).Run(/*url_on_high_confidence_allowlist=*/true,
                            /*logging_details=*/std::nullopt);
  }

  bool CheckUrlForSubresourceFilter(const GURL& url, Client* client) override {
    return true;
  }

  void MatchDownloadAllowlistUrl(
      const GURL& url,
      base::OnceCallback<void(bool)> callback) override {
    std::move(callback).Run(false);
  }

  safe_browsing::ThreatSource GetBrowseUrlThreatSource(
      safe_browsing::CheckBrowseUrlType check_type) const override {
    return safe_browsing::ThreatSource::UNKNOWN;
  }

  safe_browsing::ThreatSource GetNonBrowseUrlThreatSource() const override {
    return safe_browsing::ThreatSource::UNKNOWN;
  }

  bool IsDatabaseReady() const override { return true; }

 private:
  ~BdkDatabaseManager() override = default;

  bool AskApplication(const std::shared_ptr<bdkcore::Runtime>& rt,
                      const GURL& url,
                      const safe_browsing::SBThreatTypeSet& threat_types,
                      Client* client) {
    bdkcore::ProviderHandlers handlers;
    handlers.done = [self = weak_factory_.GetWeakPtr(), client, url,
                     types =
                         threat_types](const bdkcore::ProviderReply& reply) {
      if (!self) {
        return;
      }
      SBThreatType threat = SBThreatType::SB_THREAT_TYPE_SAFE;
      if (reply.ok()) {
        std::optional<base::DictValue> answer =
            ParseJsonDict(reply.result_json);
        const std::string* verdict =
            answer ? answer->FindString("verdict") : nullptr;
        if (verdict) {
          threat = ThreatOf(*verdict);
        }
      } else {
        // As Safe Browsing does, a check that fails lets the page load.
        LOG(WARNING) << "the reputation provider could not check a URL: "
                     << reply.message;
      }
      if (!types.contains(threat)) {
        threat = SBThreatType::SB_THREAT_TYPE_SAFE;
      }
      self->Answer(client, url, threat);
    };
    std::string input = "{\"url\":\"" + bdkcore::json_escape(url.spec()) +
                        "\",\"kind\":\"navigation\"}";
    pending_[client] =
        bdkcore::provider_invoke(rt, bdkcore::Service::kReputation, "check_url",
                                 input, std::move(handlers));
    return false;
  }

  void Answer(Client* client, const GURL& url, SBThreatType threat) {
    auto it = pending_.find(client);
    if (it == pending_.end()) {
      return;  // cancelled
    }
    pending_.erase(it);
    client->OnCheckBrowseUrlResult(url, threat);
  }

  std::map<Client*, std::shared_ptr<bdkcore::ProviderRequest>> pending_;
  base::WeakPtrFactory<BdkDatabaseManager> weak_factory_{this};
};

// Chrome's Safe Browsing service with BDK's database manager; the rest of the
// service (the blocking pages, the UI manager, download protection) is
// Chromium's own.
class BdkSafeBrowsingService
    : public safe_browsing::SafeBrowsingService,
      public safe_browsing::ServicesDelegate::ServicesCreator {
 public:
  BdkSafeBrowsingService() {
    // The only way to give the service a database manager without patching
    // Chromium: ServicesDelegate takes a creator, as its tests do.
    services_delegate_ =
        safe_browsing::ServicesDelegate::CreateForTest(this, this);
  }

  // ServicesDelegate::ServicesCreator:
  bool CanCreateDatabaseManager() override { return true; }
#if BUILDFLAG(SAFE_BROWSING_DOWNLOAD_PROTECTION)
  bool CanCreateDownloadProtectionService() override { return false; }
  safe_browsing::DownloadProtectionService* CreateDownloadProtectionService()
      override {
    return nullptr;
  }
#endif
  bool CanCreateIncidentReportingService() override { return false; }
  scoped_refptr<safe_browsing::SafeBrowsingDatabaseManager>
  CreateDatabaseManager() override {
    return base::MakeRefCounted<BdkDatabaseManager>();
  }
  safe_browsing::IncidentReportingService* CreateIncidentReportingService()
      override {
    return nullptr;
  }

 protected:
  ~BdkSafeBrowsingService() override = default;
};

class BdkSafeBrowsingServiceFactory
    : public safe_browsing::SafeBrowsingServiceFactory {
 public:
  safe_browsing::SafeBrowsingServiceInterface* CreateSafeBrowsingService()
      override {
    return new BdkSafeBrowsingService();
  }
};

}  // namespace

void InstallReputationService() {
  if (CurrentProviderSettings().reputation == "google") {
    return;  // Chromium's own Safe Browsing, with the application's key
  }
  static base::NoDestructor<BdkSafeBrowsingServiceFactory> factory;
  safe_browsing::SafeBrowsingServiceInterface::RegisterFactory(factory.get());
  if (!CurrentProviderSettings().reputation_endpoint.empty()) {
    // The list download and its signing key wait for the lists BDK hosts.
    LOG(WARNING) << "services.reputation.endpoint is set, but this runtime "
                    "does not download lists yet (T-PRV-11)";
  }
}

bool LoadReputationList(std::string_view json, std::string* error) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt) {
    *error = "the runtime is not running";
    return false;
  }
  bdkcore::ReputationList list;
  return bdkcore::reputation_list_parse(std::string(json), &list, error) &&
         bdkcore::reputation_set_list(*rt, std::move(list), error);
}

}  // namespace bdk
