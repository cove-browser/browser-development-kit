// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// cookies.*: the cookie manager of the profile's default storage partition. A
// private profile's partition is its own. A "site" is the registrable domain
// of a cookie's domain (the host itself for an IP address or a single label).

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "bdk/engine/services/browsing_data.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browsing_data_filter_builder.h"
#include "content/public/browser/browsing_data_remover.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_constants.h"
#include "net/cookies/cookie_options.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "url/gurl.h"

namespace bdk {

namespace {

constexpr size_t kDefaultLimit = 100;
constexpr char kCursorPrefix[] = "c:";

network::mojom::CookieManager* ManagerOf(const ServiceCall& c) {
  return c.profile()
      ->GetDefaultStoragePartition()
      ->GetCookieManagerForBrowserProcess();
}

std::string HostOf(const std::string& domain) {
  return domain.starts_with(".") ? domain.substr(1) : domain;
}

std::string SiteOfHost(const std::string& host) {
  std::string site = net::registry_controlled_domains::GetDomainAndRegistry(
      host, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  return site.empty() ? host : site;
}

// The site an argument names: a host, or a URL whose host is used.
std::string SiteArg(const ServiceCall& c) {
  std::string text = c.StringOr("site", "");
  GURL url(text);
  return SiteOfHost(url.is_valid() && url.has_host() ? std::string(url.host())
                                                     : text);
}

const char* SameSiteName(net::CookieSameSite same_site) {
  switch (same_site) {
    case net::CookieSameSite::NO_RESTRICTION:
      return "none";
    case net::CookieSameSite::LAX_MODE:
      return "lax";
    case net::CookieSameSite::STRICT_MODE:
      return "strict";
    default:
      return nullptr;
  }
}

net::CookieSameSite ParseSameSite(const std::string& name) {
  if (name == "none") {
    return net::CookieSameSite::NO_RESTRICTION;
  }
  if (name == "lax") {
    return net::CookieSameSite::LAX_MODE;
  }
  if (name == "strict") {
    return net::CookieSameSite::STRICT_MODE;
  }
  return net::CookieSameSite::UNSPECIFIED;
}

std::string CookieJson(const net::CanonicalCookie& cookie, bool with_value) {
  const char* same_site = SameSiteName(cookie.SameSite());
  return "{\"name\":" + JsonString(cookie.Name()) +
         (with_value ? ",\"value\":" + JsonString(cookie.Value())
                     : std::string()) +
         ",\"domain\":" + JsonString(cookie.Domain()) +
         ",\"path\":" + JsonString(cookie.Path()) + ",\"expires\":" +
         (cookie.IsPersistent() ? JsonTime(cookie.ExpiryDate())
                                : std::string("null")) +
         ",\"secure\":" + (cookie.SecureAttribute() ? "true" : "false") +
         ",\"httpOnly\":" + (cookie.IsHttpOnly() ? "true" : "false") +
         ",\"sameSite\":" +
         (same_site ? JsonString(same_site) : std::string("null")) + "}";
}

void Sites(const std::shared_ptr<ServiceCall>& c) {
  size_t offset = 0;
  if (std::optional<std::string> cursor = c->String("cursor")) {
    if (!cursor->starts_with(kCursorPrefix) ||
        !base::StringToSizeT(cursor->substr(sizeof(kCursorPrefix) - 1),
                             &offset)) {
      c->InvalidArguments("the cursor is not one this call returned");
      return;
    }
  }
  const size_t limit = static_cast<size_t>(std::max(
      1.0, c->Number("limit").value_or(static_cast<double>(kDefaultLimit))));
  ManagerOf(*c)->GetAllCookies(base::BindOnce(
      [](std::shared_ptr<ServiceCall> c, size_t offset, size_t limit,
         const std::vector<net::CanonicalCookie>& cookies) {
        std::map<std::string, int> counts;
        for (const net::CanonicalCookie& cookie : cookies) {
          ++counts[SiteOfHost(HostOf(cookie.Domain()))];
        }
        std::string out = "{\"items\":[";
        size_t index = 0;
        size_t written = 0;
        for (const auto& [site, count] : counts) {
          if (index++ < offset) {
            continue;
          }
          if (written == limit) {
            break;
          }
          // Storage other than cookies is not measured: storageBytes is 0.
          out += std::string(written ? "," : "") +
                 "{\"site\":" + JsonString(site) +
                 ",\"cookieCount\":" + base::NumberToString(count) +
                 ",\"storageBytes\":0}";
          ++written;
        }
        const size_t next = offset + written;
        out += "],\"nextCursor\":";
        out += next < counts.size()
                   ? JsonString(kCursorPrefix + base::NumberToString(next))
                   : "null";
        c->Resolve(out + "}");
      },
      c, offset, limit));
}

void ListCookies(const std::shared_ptr<ServiceCall>& c, bool with_values) {
  ManagerOf(*c)->GetAllCookies(base::BindOnce(
      [](std::shared_ptr<ServiceCall> c, bool with_values, std::string site,
         const std::vector<net::CanonicalCookie>& cookies) {
        std::string out = "{\"items\":[";
        bool first = true;
        for (const net::CanonicalCookie& cookie : cookies) {
          if (SiteOfHost(HostOf(cookie.Domain())) != site) {
            continue;
          }
          out += (first ? "" : ",") + CookieJson(cookie, with_values);
          first = false;
        }
        c->Resolve(out + "]}");
      },
      c, with_values, SiteArg(*c)));
}

void List(const std::shared_ptr<ServiceCall>& c) {
  ListCookies(c, false);
}

void GetValues(const std::shared_ptr<ServiceCall>& c) {
  ListCookies(c, true);
}

void SetCookie(const std::shared_ptr<ServiceCall>& c) {
  const base::DictValue* spec = c->Dict("cookie");
  if (!spec) {
    c->InvalidArguments("a cookie is needed");
    return;
  }
  const auto text = [&](const char* key) {
    const std::string* value = spec->FindString(key);
    return value ? *value : std::string();
  };
  const bool secure = spec->FindBool("secure").value_or(false);
  const std::string domain = text("domain");
  const GURL url((secure ? "https://" : "http://") + HostOf(domain) +
                 (text("path").empty() ? "/" : text("path")));
  base::Time expires;
  if (std::optional<double> ms = spec->FindDouble("expires")) {
    expires =
        base::Time::FromMillisecondsSinceUnixEpoch(static_cast<int64_t>(*ms));
  }
  net::CookieInclusionStatus status;
  std::unique_ptr<net::CanonicalCookie> cookie =
      net::CanonicalCookie::CreateSanitizedCookie(
          url, text("name"), text("value"), domain, text("path"),
          base::Time::Now(), expires, base::Time::Now(), secure,
          spec->FindBool("httpOnly").value_or(false),
          ParseSameSite(text("sameSite")), net::COOKIE_PRIORITY_MEDIUM,
          std::nullopt, &status);
  if (!cookie) {
    c->InvalidArguments("the cookie is not valid");
    return;
  }
  ManagerOf(*c)->SetCanonicalCookie(
      *cookie, url, net::CookieOptions::MakeAllInclusive(),
      base::BindOnce(
          [](std::shared_ptr<ServiceCall> c, net::CookieAccessResult result) {
            if (result.status.IsInclude()) {
              c->ResolveNull();
            } else {
              c->InvalidArguments("Chromium refused the cookie: " +
                                  result.status.GetDebugString());
            }
          },
          c));
}

void Remove(const std::shared_ptr<ServiceCall>& c) {
  ManagerOf(*c)->GetAllCookies(base::BindOnce(
      [](std::shared_ptr<ServiceCall> c,
         const std::vector<net::CanonicalCookie>& cookies) {
        for (const net::CanonicalCookie& cookie : cookies) {
          if (cookie.Name() == c->StringOr("name", "") &&
              cookie.Domain() == c->StringOr("domain", "") &&
              cookie.Path() == c->StringOr("path", "")) {
            ManagerOf(*c)->DeleteCanonicalCookie(
                cookie, base::BindOnce([](std::shared_ptr<ServiceCall> c,
                                          bool) { c->ResolveNull(); },
                                       c));
            return;
          }
        }
        c->ResolveNull();  // nothing to remove
      },
      c));
}

void RemoveSiteData(const std::shared_ptr<ServiceCall>& c) {
  auto filter = content::BrowsingDataFilterBuilder::Create(
      content::BrowsingDataFilterBuilder::Mode::kDelete);
  filter->AddRegisterableDomain(SiteArg(*c));
  RemoveBrowsingData(
      c->profile(), base::Time(), base::Time::Max(),
      content::BrowsingDataRemover::DATA_TYPE_COOKIES |
          content::BrowsingDataRemover::DATA_TYPE_DOM_STORAGE,
      std::move(filter),
      base::BindOnce([](std::shared_ptr<ServiceCall> c) { c->ResolveNull(); },
                     c));
}

}  // namespace

void AddCookiesHandlers(HandlerTable& table) {
  table["cookies.sites"] = &Sites;
  table["cookies.list"] = &List;
  table["cookies.getValues"] = &GetValues;
  table["cookies.set"] = &SetCookie;
  table["cookies.remove"] = &Remove;
  table["cookies.removeSiteData"] = &RemoveSiteData;
}

}  // namespace bdk
