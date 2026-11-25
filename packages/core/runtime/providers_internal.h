/* Providers (T-PRV-01, T-PRV-09): which provider each service uses, the application's providers
   registered through bdk_provider.h, the contract their answers must meet, and the parts of
   translation and reputation that do not depend on Chromium. Platform-neutral C++23 like the rest
   of the core; the decisions are in packages/core/docs/conventions.md ("Providers").

   A backend never calls an application provider directly: it asks provider_invoke, which posts
   the call, checks the answer against the contract and reports it later, on the UI thread. */
#ifndef BDK_PROVIDERS_INTERNAL_H_
#define BDK_PROVIDERS_INTERNAL_H_

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "broker_internal.h"

namespace bdkcore {

/* The services that take a provider ([services.<name>] in bdk.toml). */
enum class Service : uint8_t { kTranslation = 0, kReputation = 1, kGeolocation = 2 };
constexpr int kServiceCount = 3;
const char* service_name(Service);
bool service_from_name(const std::string& name, Service* out);

/* What the resolved configuration chose for one service. */
struct ProviderChoice {
    std::string provider;               /* "default", "system", "app" or "google" */
    std::string endpoint;               /* empty unless remapped */
    std::string update_interval = "1h"; /* reputation lists */
};

/* Reads "services" from the resolved configuration and the Google key from the start configuration.
   Called by bdk_runtime_start before the backend starts. Fails with config.resolvedInvalid (a value
   of the wrong kind or an unknown provider name), config.providerUnavailable (a provider BDK names
   but this runtime does not have yet), config.endpointUnsupported (an endpoint for a provider whose
   protocol cannot be redirected), config.keyMissing (a provider = "google" with no key) and
   config.keyUnused (a key with no service using Google). */
bdk_status_t providers_configure(Runtime&, const std::string& resolved,
                                 const bdk_start_config_t& config, bdk_error_t* out_error);
/* Fails with config.providerMissing, naming every service configured "app" with no provider.
   Called at the freeze. */
bdk_status_t providers_check_registrations(const Runtime&, bdk_error_t* out_error);

const ProviderChoice& provider_choice(const Runtime&, Service);
/* The service is configured "app" (and so, after the freeze, has a registered provider). */
bool provider_is_app(const Runtime&, Service);
/* The Google key from the start configuration; empty unless a service uses provider = "google". */
const std::string& provider_google_key(const Runtime&);
/* [services.webauthn] phones (configuration.md 3.8, T-UIR-12): whether a WebAuthn ceremony may
   offer a passkey on a phone and so contact the phone's tunnel host. False unless the resolved
   configuration says true; a backend offers no phone and contacts no tunnel host while it is. */
bool webauthn_phones_allowed(const Runtime&);
/* The descriptor of the service's provider, as JSON: the registered one for "app", BDK's own for
   the others (services.md 5.12, ProviderDescriptor). The default reputation provider's says
   "reduced protection". */
std::string provider_descriptor(const Runtime&, Service);
/* Whether the registered application provider has the operation ("translate", "prepare", ...). */
bool provider_has_operation(const Runtime&, Service, const std::string& operation);

/* bdk_provider_register and the handle of one call, for core.cc. */
bdk_status_t provider_register(Runtime&, const std::string& service,
                               const bdk_provider_vtable_t& vtable, void* ctx, bdk_release_cb rel,
                               bdk_error_t* out_error);
/* Cancels every outstanding provider call (runtime shutdown). */
void providers_shutdown(Runtime&);

/* An answer of a provider. An empty code is success. */
struct ProviderReply {
    std::string domain, code, message;
    std::string result_json = "null";
    bool ok() const {
        return code.empty();
    }
};

struct ProviderHandlers {
    std::function<void(double)> progress;           /* prepare */
    std::function<void(const std::string&)> report; /* start: one checked position */
    std::function<void(const ProviderReply&)> done; /* exactly once, later, on the UI thread */
};

/* One outstanding call to an application provider. */
class ProviderRequest {
  public:
    virtual ~ProviderRequest() = default;
    /* The provider is told (its cancel operation) and done gets broker.cancelled, later. Does
       nothing once done has run. */
    virtual void Cancel() = 0;
};

/* Calls an operation of the service's application provider. The input is checked by the caller;
   the answer is checked here against the operation's contract, and an answer that breaks it is
   provider.contractViolation. A rejection keeps its code in the provider domain. done is always
   called later. Returns null only when the service has no application provider or no such
   operation: done is then called later with provider.unavailable. */
std::shared_ptr<ProviderRequest> provider_invoke(const std::shared_ptr<Runtime>&, Service,
                                                 const std::string& operation,
                                                 const std::string& input_json,
                                                 ProviderHandlers handlers);

/* Checks an answer against the contract of the operation. Empty when it meets it; otherwise the
   problem. normalized receives the answer as the core passes it on. Exposed for the stub's built-in
   providers, which answer through the same checks. */
std::string provider_check_answer(Service, const std::string& operation,
                                  const std::string& input_json, const std::string& answer_json,
                                  std::string* normalized);

/* ---- Translation (translation.json; T-PRV-02, T-PRV-03) ---- */

struct TranslationPreferences {
    std::string target = "en";
    std::vector<std::string> always, never, never_sites;
    bool offers = true;
};
std::string translation_preferences_json(const TranslationPreferences&);
/* Applies translation.setPreferences arguments (already checked against the definition). Returns
   false, with a message, for a language code or a site that is not acceptable. */
bool translation_apply_preferences(const JsonValue& arguments, TranslationPreferences* prefs,
                                   std::string* message);

/* What a page's translation is, as translation.state answers and translation.stateChanged and
   BDK_CONTENTS_EVENT_TRANSLATION report it. */
struct TranslationState {
    std::string phase = "unavailable"; /* unavailable, available, translating, translated, failed */
    std::string detected, from, to;
    double progress = -1; /* < 0: not reported */
    std::string error_domain, error_code, error_message;
};
std::string translation_state_json(const TranslationState&);
/* Records the page's state and, if it changed, reports it: a contents event to the page's
   subscribers and translation.stateChanged to the profile's. */
void translation_set_state(const std::shared_ptr<Contents>&, const TranslationState&);
const TranslationState& translation_state(const Contents&);

/* Whether a page in `language` on `site` (its origin) should be offered translation, or translated
   without asking (the language is on the always list). und, empty and the target language are not
   offered. */
struct TranslationDecision {
    bool offer = false, automatic = false;
};
TranslationDecision translation_decide(const TranslationPreferences&, const std::string& language,
                                       const std::string& site);

/* The provider's detect operation, for a page that declares no language: its input, a sample of
   the page's text segments (about the first 2 KiB), and the language its answer names (empty for
   null, a failure or no language). */
std::string translation_detect_input(const std::vector<std::string>& texts);
std::string translation_detected(const ProviderReply& reply);

/* A language code BDK accepts: two or three letters, optionally followed by -subtags of letters and
   digits (BCP 47 shape, not checked against a registry). */
bool translation_language_ok(const std::string& code);

/* How a page's text is translated, whatever the backend: a list of text segments in, the same list
   translated out. Segments that are empty or only whitespace are not sent. The provider is asked
   for its pairs first (provider.unsupportedPair when the pair is not offered), then to prepare a
   pair that is not installed, then for the segments in batches (at most kTranslationBatchTexts
   segments and kTranslationBatchBytes bytes each), one at a time. A segment longer than
   kTranslationBatchBytes is sent as pieces that fit (split at a space where one is near, else at a
   character boundary) and its translation is the pieces' answers joined. Progress is the share of
   pieces done. Cancel stops at the next answer and reports broker.cancelled. */
constexpr size_t kTranslationBatchTexts = 64;
constexpr size_t kTranslationBatchBytes = size_t{16} * 1024;

class TranslationJob {
  public:
    virtual ~TranslationJob() = default;
    virtual void Cancel() = 0;
};

/* What a backend's own provider does for one step of a job, for a service not configured "app"
   (the default engine). It answers through the same contract checks as an application provider,
   and always calls done, later, exactly once (provider.unavailable when it cannot run); it returns
   the request, or null when there is nothing to cancel. */
using BuiltinProviderCall = std::function<std::shared_ptr<ProviderRequest>(
    const std::string& operation, const std::string& input_json, ProviderHandlers)>;

struct TranslationJobSpec {
    std::string from, to, url;
    std::vector<std::string> texts;
    BuiltinProviderCall builtin; /* used unless translation is configured "app" */
    std::function<void(double)> progress;
    /* Once, later: the reply and, when it succeeded, the translated segments, in order. */
    std::function<void(const ProviderReply&, const std::vector<std::string>&)> done;
};
std::shared_ptr<TranslationJob> translation_job_start(const std::shared_ptr<Runtime>&,
                                                      TranslationJobSpec spec);

/* Calls one translation operation on whichever provider is configured: the application's, or the
   backend's own through builtin. */
std::shared_ptr<ProviderRequest> translation_call(const std::shared_ptr<Runtime>&,
                                                  const BuiltinProviderCall& builtin,
                                                  const std::string& operation,
                                                  const std::string& input_json,
                                                  ProviderHandlers handlers);

/* The completions of a translation request (ui-requests.md 5.10), parsed. */
struct TranslationAnswer {
    enum class Kind : uint8_t { kTranslate, kRevert, kAlways, kNever, kNeverForSite, kDismiss };
    Kind kind = Kind::kDismiss;
    std::string from, to, language;
};
/* False, with a message, for a completion the surface does not have or arguments that do not fit.
 */
bool translation_parse_answer(const std::string& completion, const std::string& arguments,
                              TranslationAnswer* out, std::string* message);
/* The payload of a translation request: phase, detected, target, languages, provider. */
std::string translation_request_payload(const TranslationState&, const std::string& target,
                                        const std::string& pairs_json,
                                        const std::string& descriptor_json);

/* ---- Reputation lists (decision D2; T-PRV-05) ---- */

/* A block list in BDK's format (conventions.md, "Reputation lists"). The signature is checked by
   the backend that downloaded it, before it is parsed. */
struct ReputationList {
    int64_t sequence = 0, generated = 0, expires = 0;
    std::string source;
    std::map<std::string, std::string> entries; /* expression -> verdict */
};
/* Parses a list. Fails, with a message, on anything that is not exactly the format. */
bool reputation_list_parse(const std::string& json, ReputationList* out, std::string* message);
/* The expressions a URL is looked up by, as Safe Browsing forms them: up to five host suffixes
   times up to six path prefixes, most specific first. Empty for a URL that is not http or https. */
std::vector<std::string> reputation_expressions(const std::string& url);
/* The verdict for a URL ("safe" when no expression matches, also for a list that has expired at
   now_ms, which reports "unknown"). */
std::string reputation_list_lookup(const ReputationList&, const std::string& url, int64_t now_ms);

/* The runtime's current list for the default provider. A list replaces the current one only when
   its sequence is higher. Returns false, with a message, when it does not. */
bool reputation_set_list(Runtime&, ReputationList list, std::string* message);
const ReputationList* reputation_current_list(const Runtime&);

/* The payload of a reputation warning for a verdict: url, verdict, provider, isOverridable, text.
 */
std::string reputation_warning_payload(const Runtime&, const std::string& url,
                                       const std::string& verdict);

} // namespace bdkcore

#endif /* BDK_PROVIDERS_INTERNAL_H_ */
