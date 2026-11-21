# chromium/bdk/engine/providers

Providers and privacy in the engine (M6). The choice of provider, the contract and the platform-neutral
parts (translation jobs, reputation lists) are the core's: `packages/core/runtime/providers_internal.h`
and `packages/core/docs/conventions.md`, "Providers". Everything here was written against the Chromium
source at the pin (158.0.8127.37) and **has not been built or run yet**: the engine build and the engine
tests (`tests_engine_providers.c`, `tests_engine_privacy.c`) are pending.

| File | Role |
|---|---|
| `provider_settings.{h,cc}` | A copy of the provider choice that Chromium's threads may read; the start-up switches and preferences of the audit list below; the application's Google key, given to Chromium before it starts (`google_apis::InitializeAndOverrideAPIKey`, compiled in by `support_external_google_api_key` in `chromium/gn`). Chromium accepts a key once per process. |
| `geolocation_provider.{h,cc}` | `provider = "app"`: a `device::LocationProvider` that `BdkContentBrowserClient::OverrideSystemLocationProvider` returns. It starts the application's watch on the UI thread once a page's permission is granted and hands each position back to the geolocation thread. `system` forces Chromium's `LocationProviderManager` to `PlatformOnly` (macOS defaults to a hybrid mode that can fall back to Google's network provider, `device_features.cc`); `google` keeps Chrome's mode and `GetGeolocationApiKey` gives the key only then. |
| `reputation_service.{h,cc}` | `default` and `app`: BDK's Safe Browsing service (`SafeBrowsingServiceInterface::RegisterFactory`), Chrome's own with a database manager that answers browse-URL checks from the core's lists or the application's `check_url`. A verdict shows Chromium's blocking page, which `services/interstitial_prompts.cc` routes to `reputation_warnings` with the provider's descriptor ("reduced protection" for the lists). The service is given its manager through `ServicesDelegate::CreateForTest(this, this)`, the only way without a patch. `google`: Chromium's Safe Browsing as in Chrome. |
| `translation_service.{h,cc}` | The `translation.*` commands and a page's translation: Chromium's language detection (`TranslateDriver::LanguageDetectionObserver`) and translate preferences are kept, and a page it reports as `und` is sampled and given to the provider's `detect` when it has one; Google's script injection is replaced by two scripts in the translate isolated world of the main frame that read and write text nodes; the text goes to the provider through the core's job. The offer is a `translation` request; with no handler the default presenter is a sheet (`UiPresenters::ShowAlert`, surface `translation`). |

## Audit list (T-PRV-07)

Read at the pin for a non-branded mac build with no Google key. "Off" means the build or BDK's start-up
settings turn it off; the network audit (`tests_engine_privacy.c`) is the check.

| Feature | Contacts | In this build | BDK's setting |
|---|---|---|---|
| Metrics (UMA, UKM) | Google | Off without consent in non-branded builds (`metrics_service_accessor.cc:34-44`) | `user_experience_metrics.reporting_enabled` false (local state). BDK never passes `--force-enable-metrics-reporting` |
| Crash upload | `clients2.google.com/cr/report` | Off: no consent when not official (`chrome_crash_reporter_client.cc:218-221`) | BDK never passes `--enable-user-metrics` (`chrome_main_delegate.cc:1553-1558`) |
| Variations seed | `clientservices.googleapis.com` | Off unless `--variations-server-url` (`variations_service.cc:284-297`) | Never passed |
| Component updater | `update.googleapis.com` | **On** (`browser_process_impl.cc:1519-1529`) | `--disable-component-update`; `component_updates.component_updates_enabled` false (T-PRV-06) |
| Network time | `clients2.google.com/time` | **On**, queried at start-up (`network_time_tracker.cc:75-77, 392`) | Feature `NetworkTimeServiceQuerying` off; `network_time.network_time_queries_enabled` false |
| Safe Browsing V4 updates | `safebrowsing.googleapis.com` | **On** (a dummy key is sent) | Replaced by BDK's database manager for `default` and `app` |
| Safe Browsing real-time lookups | Google | Hash-prefix real-time needs Chrome branding (`hash_realtime_utils.cc:107-110`); URL real-time needs enhanced protection | `safebrowsing.enhanced`, `scout_reporting_enabled`, `extended_reporting_opt_in_allowed` false; the high-confidence allowlist answers "allowlisted" |
| Safe Browsing download protection, client-side detection, password protection, incident reports | Google | Created by Chrome's service | Client-side detection: the CSD allowlist answers "match". Download protection pings for executable downloads are **not settled** (T-PRV-12) |
| Sign-in, account consistency | `accounts.google.com` | Off: DICE needs an OAuth client (`account_consistency_mode_manager.cc:64-81`) | `--allow-browser-signin=false`; `signin.allowed_on_next_startup` false. BDK never supplies an OAuth client |
| Sync | Google | Unreachable without an account | `--disable-sync` |
| Optimization guide hints and models (incl. the language detection model) | Google | Need a key and consent | Features `OptimizationHints`, `OptimizationTargetPrediction` off; `url_keyed_anonymized_data_collection.enabled` false |
| Translate (element.js) | `translate.googleapis.com` | Inert without a key (`translate_manager.cc:971-980`); with a key the always list would auto-translate | Feature `TranslateTrigger` off; BDK's own translation replaces it |
| Spelling service | Google | macOS uses its own checker (`spellcheck_build_features.gni:13`) | `spellcheck.use_spelling_service` false |
| Password leak detection | `passwordsleakcheck-pa.googleapis.com` | Needs sign-in | `profile.password_manager_leak_detection` false |
| Domain reliability | Google | Off without metrics | `--disable-domain-reliability` |
| Captive portal and error-page suggestions | `www.gstatic.com/generate_204` | Only with `alternate_error_pages.enabled` | `alternate_error_pages.enabled` false |
| DNS over HTTPS automatic upgrade | The resolver's provider | Not verified | `dns_over_https.mode` "off" |
| Privacy Sandbox (attestations, topics, protected audiences) | Google | Data comes from the component updater | `privacy_sandbox.m1.*` false |
| Media router | Local discovery | | `media_router.enable_media_router` false |
| GCM (push messaging) | `mtalk.google.com` | Lazy: starts on a push subscription (`gcm_driver_desktop.cc:527, 589`) | **Not settled**: no switch or preference; T-PRV-12 |
| Omnibox AI Mode eligibility | `www.google.com/async/folae` | **On**, asked at start-up (`aim_eligibility_service.cc`); found by the network audit | Features `AimEnabled`, `AimServerEligibilityEnabled` off |
| Search-engine preconnect | The default search engine (Google until T-PRV-10) | **On**, at start-up (`search_engine_preconnector.cc`); found by the network audit | Feature `PreconnectToSearch` off |
| Translate ranker model | `www.gstatic.com/chrome/intelligence/assist/ranker/` | **On**, downloaded when translate starts (`translate_ranker_impl.cc`); found by the network audit | Features `TranslateRankerQuery`, `TranslateRankerEnforcement` off |
| Extension updates from the Chrome Web Store | `clients2.google.com` | Only with a store extension installed | M5's sources (T-EXT-09) |

Not settled from the source, so left to the network audit and task T-PRV-12: what registers with GCM at
start-up, whether the translate language list is fetched at start-up, the default search provider and
search prefetch (S1), and the Safe Browsing pings above.

## Security data (T-PRV-06, decision D5)

The default is the data that ships with each runtime release, with the component updater off. A mirror
of Google's update server cannot work without a patch: update-check responses are signed with Google's
key and bound to each request's nonce (CUP, `request_sender.cc:276-290`), and `EnabledCupSigning` is
fixed to true (`configurator_impl.cc:139-142`). What ships: the Certificate Transparency log list and the
Chrome Root Store are compiled in; Chromium's built-in CRLSet is empty, so CRLSet revocation is absent
between releases. Certificate Transparency is off in non-branded builds
(`kCertificateTransparencyAskBeforeEnabling`), so the 70-day staleness cutoff does not apply unless BDK
turns it on. The findings are in `docs/planning/feasibility.md` section 6.
