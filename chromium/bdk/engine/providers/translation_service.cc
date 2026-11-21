// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/providers/translation_service.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/providers_internal.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/services/service_call.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "components/prefs/pref_service.h"
#include "components/translate/content/browser/content_translate_driver.h"
#include "components/translate/core/browser/translate_download_manager.h"
#include "components/translate/core/browser/translate_pref_names.h"
#include "components/translate/core/browser/translate_prefs.h"
#include "components/translate/core/common/language_detection_details.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "url/origin.h"

namespace bdk {

namespace {

// Reads the main frame's text nodes, restoring any translation first, and
// keeps them in the isolated world so a later script can write them back. Text
// in script, style and code, and in elements marked translate="no" or
// "notranslate" (as Chrome honours them), is left alone.
constexpr char16_t kExtractScript[] =
    uR"JS((() => {
  const old = globalThis.__bdkTranslation;
  if (old) old.nodes.forEach((n, i) => { n.nodeValue = old.original[i]; });
  const skip = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'TEXTAREA', 'CODE',
                        'PRE', 'svg', 'math']);
  const state = globalThis.__bdkTranslation = {nodes: [], original: []};
  const root = document.body || document.documentElement;
  if (!root) return {texts: [], lang: ''};
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, {
    acceptNode(node) {
      for (let e = node.parentElement; e; e = e.parentElement) {
        if (skip.has(e.tagName) || e.getAttribute('translate') === 'no' ||
            e.classList.contains('notranslate'))
          return NodeFilter.FILTER_REJECT;
      }
      return node.nodeValue.trim() ? NodeFilter.FILTER_ACCEPT
                                   : NodeFilter.FILTER_REJECT;
    }});
  for (let n = walker.nextNode(); n && state.nodes.length < 5000;
       n = walker.nextNode()) {
    state.nodes.push(n);
    state.original.push(n.nodeValue);
  }
  return {texts: state.original, lang: document.documentElement.lang || ''};
})())JS";

// Writes translated texts into the nodes the extract script kept. Only text
// node values are set: a translation can never add markup.
constexpr char16_t kApplyPrefix[] =
    uR"JS(((texts) => {
  const s = globalThis.__bdkTranslation;
  if (!s) return false;
  s.nodes.forEach((n, i) => {
    if (typeof texts[i] === 'string') n.nodeValue = texts[i];
  });
  return true;
}))JS";

constexpr char16_t kRevertScript[] =
    uR"JS((() => {
  const s = globalThis.__bdkTranslation;
  if (!s) return false;
  s.nodes.forEach((n, i) => { n.nodeValue = s.original[i]; });
  return true;
})())JS";

std::string DefaultTarget() {
  std::string locale = g_browser_process
                           ? g_browser_process->GetApplicationLocale()
                           : std::string("en");
  return translate::TranslateDownloadManager::GetLanguageCode(locale);
}

bdkcore::TranslationPreferences ReadPrefs(Profile* profile) {
  bdkcore::TranslationPreferences prefs;
  std::unique_ptr<translate::TranslatePrefs> tp =
      ChromeTranslateClient::CreateTranslatePrefs(profile->GetPrefs());
  prefs.target = tp->GetRecentTargetLanguage();
  if (prefs.target.empty()) {
    prefs.target = DefaultTarget();
  }
  prefs.always = tp->GetAlwaysTranslateLanguages();
  prefs.never = tp->GetNeverTranslateLanguages();
  prefs.never_sites =
      tp->GetNeverPromptSitesBetween(base::Time(), base::Time::Max());
  prefs.offers =
      profile->GetPrefs()->GetBoolean(translate::prefs::kOfferTranslateEnabled);
  return prefs;
}

bool Has(const std::vector<std::string>& list, const std::string& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

// Writes the difference between two preference sets to Chromium's.
void WritePrefs(Profile* profile,
                const bdkcore::TranslationPreferences& before,
                const bdkcore::TranslationPreferences& after) {
  std::unique_ptr<translate::TranslatePrefs> tp =
      ChromeTranslateClient::CreateTranslatePrefs(profile->GetPrefs());
  if (after.target != before.target) {
    tp->SetRecentTargetLanguage(after.target);
  }
  // Block before unblocking: Chromium never removes the last language of its
  // never-translate list, so a replaced list is applied only in this order.
  for (const std::string& l : after.never) {
    if (!Has(before.never, l)) {
      tp->BlockLanguage(l);
    }
  }
  for (const std::string& l : before.never) {
    if (!Has(after.never, l)) {
      tp->UnblockLanguage(l);
    }
  }
  for (const std::string& l : before.always) {
    if (!Has(after.always, l)) {
      tp->RemoveLanguagePairFromAlwaysTranslateList(l);
    }
  }
  for (const std::string& l : after.always) {
    if (!Has(before.always, l)) {
      tp->AddLanguagePairToAlwaysTranslateList(l, after.target);
    }
  }
  for (const std::string& s : before.never_sites) {
    if (!Has(after.never_sites, s)) {
      tp->RemoveSiteFromNeverPromptList(s);
    }
  }
  for (const std::string& s : after.never_sites) {
    if (!Has(before.never_sites, s)) {
      tp->AddSiteToNeverPromptList(s);
    }
  }
  if (after.offers != before.offers) {
    profile->GetPrefs()->SetBoolean(translate::prefs::kOfferTranslateEnabled,
                                    after.offers);
  }
}

void EmitPreferences(Profile* profile,
                     const std::shared_ptr<bdkcore::Runtime>& rt,
                     bdk_profile_t handle) {
  bdkcore::emit_builtin_event(
      rt, "translation.preferencesChanged", handle,
      bdkcore::translation_preferences_json(ReadPrefs(profile)));
}

// The site never-for-this-site names: the page's origin, as Chromium's
// translate UI stores it (its host).
std::string SiteOf(content::WebContents* contents) {
  return contents->GetPrimaryMainFrame()->GetLastCommittedOrigin().host();
}

Profile* ProfileOf(content::WebContents* contents) {
  return Profile::FromBrowserContext(contents->GetBrowserContext());
}

// This runtime has no translation engine of its own yet (T-PRV-02): with
// provider = "default" every step reports provider.unavailable.
const bdkcore::BuiltinProviderCall& NoEngine() {
  static const base::NoDestructor<bdkcore::BuiltinProviderCall> no_engine;
  return *no_engine;
}

class PageTranslation
    : public content::WebContentsObserver,
      public content::WebContentsUserData<PageTranslation>,
      public translate::TranslateDriver::LanguageDetectionObserver {
 public:
  ~PageTranslation() override {
    if (driver_) {
      driver_->RemoveLanguageDetectionObserver(this);
    }
    if (job_) {
      job_->Cancel();
    }
  }

  // Translates the page; `call` is the command's, or null when the request
  // or the always list started it.
  void Translate(const std::string& from,
                 const std::string& to,
                 std::shared_ptr<ServiceCall> call) {
    std::shared_ptr<bdkcore::Contents> core = Core();
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    if (!core || !rt) {
      if (call) {
        call->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "the page is gone");
      }
      return;
    }
    bdkcore::TranslationState state = bdkcore::translation_state(*core);
    const std::string src = from.empty() ? state.detected : from;
    const std::string dst =
        to.empty() ? ReadPrefs(ProfileOf(web_contents())).target : to;
    if (src.empty()) {
      if (call) {
        call->Reject(BDK_DOMAIN_PROVIDER, "unsupportedPair",
                     "the page's language is not known: name the language "
                     "to translate from");
      }
      return;
    }
    if (src == dst) {
      if (call) {
        call->InvalidArguments("the page is already in " + dst);
      }
      return;
    }
    if (job_) {
      job_->Cancel();
      job_.reset();
    }
    state.phase = "translating";
    state.from = src;
    state.to = dst;
    state.progress = 0;
    state.error_domain = state.error_code = state.error_message = "";
    bdkcore::translation_set_state(core, state);
    web_contents()->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
        kExtractScript,
        base::BindOnce(&PageTranslation::OnExtracted,
                       weak_factory_.GetWeakPtr(), src, dst, call),
        ISOLATED_WORLD_ID_TRANSLATE);
  }

  void Revert() {
    if (job_) {
      job_->Cancel();
      job_.reset();
    }
    web_contents()->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
        kRevertScript, base::DoNothing(), ISOLATED_WORLD_ID_TRANSLATE);
    std::shared_ptr<bdkcore::Contents> core = Core();
    if (!core) {
      return;
    }
    bdkcore::TranslationState state = bdkcore::translation_state(*core);
    if (state.phase == "unavailable") {
      return;
    }
    state.phase = "available";
    state.from = state.to = "";
    state.progress = -1;
    state.error_domain = state.error_code = state.error_message = "";
    bdkcore::translation_set_state(core, state);
  }

  // Raises the translation request in the page's current phase.
  // `require_pairs`: an offer is made only when the provider can say which
  // languages it translates.
  void RaiseRequest(bool require_pairs) {
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    if (!rt) {
      return;
    }
    bdkcore::ProviderHandlers handlers;
    handlers.done = [self = weak_factory_.GetWeakPtr(),
                     require_pairs](const bdkcore::ProviderReply& reply) {
      if (self && (reply.ok() || !require_pairs)) {
        self->OnPairsForRequest(reply);
      }
    };
    bdkcore::translation_call(rt, NoEngine(), "supported_pairs", "{}",
                              std::move(handlers));
  }

 private:
  friend class content::WebContentsUserData<PageTranslation>;

  explicit PageTranslation(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<PageTranslation>(*contents) {
    if (ChromeTranslateClient* client =
            ChromeTranslateClient::FromWebContents(contents)) {
      driver_ = client->translate_driver();
      driver_->AddLanguageDetectionObserver(this);
    }
  }

  std::shared_ptr<bdkcore::Contents> Core() {
    EngineContents* page = EngineContents::FromWebContents(web_contents());
    return page ? page->Core() : nullptr;
  }

  // content::WebContentsObserver: a new page has no translation; one in
  // progress is cancelled (swift-api.md 11.2, "Cancellation").
  void PrimaryPageChanged(content::Page& page) override {
    if (job_) {
      job_->Cancel();
      job_.reset();
    }
    std::shared_ptr<bdkcore::Contents> core = Core();
    if (core && bdkcore::translation_state(*core).phase != "unavailable") {
      bdkcore::translation_set_state(core, bdkcore::TranslationState{});
    }
  }

  // translate::TranslateDriver::LanguageDetectionObserver:
  // The driver's observer list must be empty when it goes, so leave it here.
  void OnTranslateDriverDestroyed(translate::TranslateDriver* driver) override {
    driver->RemoveLanguageDetectionObserver(this);
    driver_ = nullptr;
  }

  void OnLanguageDetermined(
      const translate::LanguageDetectionDetails& details) override {
    std::shared_ptr<bdkcore::Contents> core = Core();
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    if (!core || !rt) {
      return;
    }
    // Chromium reports "und" when neither the page nor its detector knows;
    // its detection model is a Google download BDK does not fetch, so
    // detection rests on what the page declares (feasibility.md 6).
    std::string language = details.adopted_language;
    if (language == "und") {
      language.clear();
    }
    bdkcore::TranslationState state;
    state.detected = language;
    state.phase = language.empty() ? "unavailable" : "available";
    bdkcore::translation_set_state(core, state);
    if (language.empty() || !SurfaceAvailable(core, BDK_SURFACE_TRANSLATION)) {
      return;
    }
    bdkcore::TranslationDecision decision = bdkcore::translation_decide(
        ReadPrefs(ProfileOf(web_contents())), language, SiteOf(web_contents()));
    if (decision.automatic) {
      Translate(language, std::string(), nullptr);
    } else if (decision.offer) {
      RaiseRequest(/*require_pairs=*/true);
    }
  }

  void OnExtracted(const std::string& from,
                   const std::string& to,
                   std::shared_ptr<ServiceCall> call,
                   base::Value result) {
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    const base::DictValue* dict = result.GetIfDict();
    const base::ListValue* list = dict ? dict->FindList("texts") : nullptr;
    if (!rt || !list) {
      Fail(BDK_DOMAIN_PROVIDER, "unavailable",
           "the page's text could not be read", call);
      return;
    }
    bdkcore::TranslationJobSpec spec;
    spec.from = from;
    spec.to = to;
    spec.url = web_contents()->GetLastCommittedURL().spec();
    for (const base::Value& text : *list) {
      spec.texts.push_back(text.is_string() ? text.GetString() : "");
    }
    spec.builtin = NoEngine();
    spec.progress = [self = weak_factory_.GetWeakPtr(), call](double f) {
      if (!self) {
        return;
      }
      if (std::shared_ptr<bdkcore::Contents> core = self->Core()) {
        bdkcore::TranslationState now = bdkcore::translation_state(*core);
        now.progress = f;
        bdkcore::translation_set_state(core, now);
      }
      if (call) {
        call->Progress(f, "translating");
      }
    };
    spec.done = [self = weak_factory_.GetWeakPtr(), call](
                    const bdkcore::ProviderReply& reply,
                    const std::vector<std::string>& texts) {
      if (self) {
        self->OnTranslated(reply, texts, call);
      } else if (call) {
        call->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "the page is gone");
      }
    };
    job_ = bdkcore::translation_job_start(rt, std::move(spec));
    if (call) {
      std::weak_ptr<bdkcore::TranslationJob> job = job_;
      call->control().OnCancel([job] {
        if (std::shared_ptr<bdkcore::TranslationJob> j = job.lock()) {
          j->Cancel();
        }
      });
    }
  }

  void OnTranslated(const bdkcore::ProviderReply& reply,
                    const std::vector<std::string>& texts,
                    std::shared_ptr<ServiceCall> call) {
    job_.reset();
    std::shared_ptr<bdkcore::Contents> core = Core();
    if (!core) {
      return;
    }
    bdkcore::TranslationState state = bdkcore::translation_state(*core);
    state.progress = -1;
    if (reply.code == BDK_CODE_CANCELLED) {
      // Reverted or navigated away: whoever cancelled has set the state.
      if (state.phase == "translating") {
        state.phase = "available";
        state.from = state.to = "";
        bdkcore::translation_set_state(core, state);
      }
      if (call) {
        call->Reject(BDK_DOMAIN_BROKER, BDK_CODE_CANCELLED,
                     "the translation was cancelled");
      }
      return;
    }
    if (!reply.ok()) {
      Fail(reply.domain.c_str(), reply.code.c_str(), reply.message, call);
      return;
    }
    base::ListValue list;
    for (const std::string& text : texts) {
      list.Append(text);
    }
    std::string json;
    base::JSONWriter::Write(list, &json);
    web_contents()->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
        kApplyPrefix + (u"(" + base::UTF8ToUTF16(json) + u")"),
        base::DoNothing(), ISOLATED_WORLD_ID_TRANSLATE);
    state.phase = "translated";
    bdkcore::translation_set_state(core, state);
    if (call) {
      call->Resolve(bdkcore::translation_state_json(state));
    }
  }

  void Fail(const char* domain,
            const char* code,
            const std::string& message,
            const std::shared_ptr<ServiceCall>& call) {
    if (std::shared_ptr<bdkcore::Contents> core = Core()) {
      bdkcore::TranslationState state = bdkcore::translation_state(*core);
      state.phase = "failed";
      state.progress = -1;
      state.error_domain = domain;
      state.error_code = code;
      state.error_message = message;
      bdkcore::translation_set_state(core, state);
    }
    if (call) {
      call->Reject(domain, code, message);
    }
  }

  void OnPairsForRequest(const bdkcore::ProviderReply& reply) {
    std::shared_ptr<bdkcore::Contents> core = Core();
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    EngineContents* page = EngineContents::FromWebContents(web_contents());
    if (!core || !rt || !page) {
      return;
    }
    std::string pairs = "[]";
    if (reply.ok()) {
      std::optional<base::DictValue> answer = ParseJsonDict(reply.result_json);
      if (const base::ListValue* list =
              answer ? answer->FindList("pairs") : nullptr) {
        base::JSONWriter::Write(*list, &pairs);
      }
    }
    const bdkcore::TranslationState state = bdkcore::translation_state(*core);
    const std::string descriptor =
        bdkcore::provider_descriptor(*rt, bdkcore::Service::kTranslation);
    std::optional<base::DictValue> payload =
        ParseJsonDict(bdkcore::translation_request_payload(
            state, ReadPrefs(ProfileOf(web_contents())).target, pairs,
            descriptor));
    if (!payload) {
      return;
    }
    RaiseUiRequest(
        *page, BDK_SURFACE_TRANSLATION, *payload,
        base::BindOnce(&PageTranslation::OnRequestFinished,
                       weak_factory_.GetWeakPtr(), payload->Clone()));
  }

  void OnRequestFinished(base::DictValue payload, const UiResult& result) {
    if (result.outcome == bdkcore::UiOutcome::kCompleted) {
      Answer(result.completion, ToJson(result.args));
      return;
    }
    if (result.outcome == bdkcore::UiOutcome::kDefaulted) {
      ShowDefault(payload);
    }
  }

  void Answer(const std::string& completion, const std::string& arguments) {
    bdkcore::TranslationAnswer answer;
    std::string message;
    std::shared_ptr<bdkcore::Contents> core = Core();
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    if (!core || !rt ||
        !bdkcore::translation_parse_answer(completion, arguments, &answer,
                                           &message)) {
      return;
    }
    Profile* profile = ProfileOf(web_contents());
    const bdkcore::TranslationPreferences before = ReadPrefs(profile);
    bdkcore::TranslationPreferences after = before;
    const std::string language =
        answer.language.empty() ? bdkcore::translation_state(*core).detected
                                : answer.language;
    using Kind = bdkcore::TranslationAnswer::Kind;
    switch (answer.kind) {
      case Kind::kTranslate:
        Translate(answer.from, answer.to, nullptr);
        return;
      case Kind::kRevert:
        Revert();
        return;
      case Kind::kDismiss:
        return;
      case Kind::kAlways:
        if (!language.empty() && !Has(after.always, language)) {
          after.always.push_back(language);
          std::erase(after.never, language);
        }
        break;
      case Kind::kNever:
        if (!language.empty() && !Has(after.never, language)) {
          after.never.push_back(language);
          std::erase(after.always, language);
        }
        break;
      case Kind::kNeverForSite:
        if (!Has(after.never_sites, SiteOf(web_contents()))) {
          after.never_sites.push_back(SiteOf(web_contents()));
        }
        break;
    }
    WritePrefs(profile, before, after);
    EmitPreferences(profile, rt, core->profile ? core->profile->handle : 0);
    if (answer.kind == Kind::kAlways && !language.empty()) {
      Translate(language, std::string(), nullptr);
    }
  }

  // The default presenter: a sheet on the page's window, as the password
  // question is (ui-requests.md 5.8 and 5.10). When the provider is remote
  // the offer names it and says the page's text is sent to it.
  void ShowDefault(const base::DictValue& payload) {
    EngineContents* page = EngineContents::FromWebContents(web_contents());
    const std::string* phase = payload.FindString("phase");
    if (!page || !phase || *phase != "offer") {
      return;  // only the offer has a default presenter
    }
    const std::string* detected = payload.FindString("detected");
    const base::DictValue* provider = payload.FindDict("provider");
    const std::string* privacy =
        provider ? provider->FindString("privacy") : nullptr;
    const std::string* host = provider ? provider->FindString("host") : nullptr;
    Alert alert;
    alert.title = "Translate this page?";
    alert.message = "This page is in " +
                    (detected ? *detected
                              : "another "
                                "language") +
                    ".";
    if (privacy && *privacy == "remote") {
      alert.message += " Its text will be sent to " +
                       (host ? *host : std::string("the provider")) + ".";
    }
    alert.buttons.push_back({"translate", "Translate"});
    alert.buttons.push_back({"never", "Never translate this language"});
    alert.buttons.push_back({"neverForSite", "Never translate this site"});
    alert.buttons.push_back({"dismiss", "Not now"});
    UiPresenters::Get().ShowAlert(
        "translation", &page->host_view(), std::move(alert),
        base::BindOnce(
            [](base::WeakPtr<PageTranslation> self, int index) {
              static constexpr auto kAnswers = std::to_array<const char*>(
                  {"translate", "never", "neverForSite", "dismiss"});
              if (self && index >= 0 &&
                  static_cast<size_t>(index) < kAnswers.size()) {
                self->Answer(kAnswers[static_cast<size_t>(index)], "{}");
              }
            },
            weak_factory_.GetWeakPtr()));
  }

  raw_ptr<translate::TranslateDriver> driver_ = nullptr;
  std::shared_ptr<bdkcore::TranslationJob> job_;
  base::WeakPtrFactory<PageTranslation> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(PageTranslation);

PageTranslation* PageOf(const std::shared_ptr<ServiceCall>& call) {
  const std::shared_ptr<bdkcore::Contents>& core = call->builtin().contents;
  EngineContents* page = core ? EngineContents::From(*core) : nullptr;
  if (!page || !page->web_contents()) {
    call->InvalidArguments("'" + call->name() +
                           "' needs a page: name one in the invoke options");
    return nullptr;
  }
  PageTranslation::CreateForWebContents(page->web_contents());
  return PageTranslation::FromWebContents(page->web_contents());
}

// A disabled translation surface turns the service off (ui-requests.md 5.10).
bool Available(const std::shared_ptr<ServiceCall>& call) {
  if (bdkcore::ui_surface_available(call->runtime(), BDK_SURFACE_TRANSLATION)) {
    return true;
  }
  call->Reject(BDK_DOMAIN_PROVIDER, "unavailable", "translation is disabled");
  return false;
}

// Answers a call with what one provider operation answered.
void Forward(const std::shared_ptr<ServiceCall>& call,
             const std::string& operation,
             const std::string& input,
             bool models_changed) {
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (!rt) {
    return;
  }
  bdkcore::ProviderHandlers handlers;
  handlers.progress = [call](double f) { call->Progress(f, "downloading"); };
  handlers.done = [call, rt,
                   models_changed](const bdkcore::ProviderReply& reply) {
    if (!reply.ok()) {
      call->Reject(reply.domain.c_str(), reply.code.c_str(), reply.message);
      return;
    }
    call->Resolve(models_changed ? "null" : reply.result_json);
    if (!models_changed) {
      return;
    }
    bdk_profile_t profile =
        call->builtin().profile ? call->builtin().profile->handle : 0;
    bdkcore::ProviderHandlers pairs;
    pairs.done = [rt, profile](const bdkcore::ProviderReply& r) {
      if (r.ok()) {
        bdkcore::emit_builtin_event(rt, "translation.modelsChanged", profile,
                                    r.result_json);
      }
    };
    bdkcore::translation_call(rt, NoEngine(), "supported_pairs", "{}",
                              std::move(pairs));
  };
  std::shared_ptr<bdkcore::ProviderRequest> request = bdkcore::translation_call(
      rt, NoEngine(), operation, input, std::move(handlers));
  if (request) {
    call->control().OnCancel([request] { request->Cancel(); });
  }
}

std::string PairInput(const std::shared_ptr<ServiceCall>& call) {
  return "{\"from\":" + JsonString(call->StringOr("from", "")) +
         ",\"to\":" + JsonString(call->StringOr("to", "")) + "}";
}

bool PairOk(const std::shared_ptr<ServiceCall>& call) {
  if (bdkcore::translation_language_ok(call->StringOr("from", "")) &&
      bdkcore::translation_language_ok(call->StringOr("to", ""))) {
    return true;
  }
  call->InvalidArguments("from and to are language codes");
  return false;
}

void HandleProvider(const std::shared_ptr<ServiceCall>& call) {
  if (Available(call)) {
    call->Resolve(bdkcore::provider_descriptor(call->runtime(),
                                               bdkcore::Service::kTranslation));
  }
}

void HandleLanguages(const std::shared_ptr<ServiceCall>& call) {
  if (Available(call)) {
    Forward(call, "supported_pairs", "{}", false);
  }
}

void HandlePreferences(const std::shared_ptr<ServiceCall>& call) {
  if (Available(call)) {
    call->Resolve(
        bdkcore::translation_preferences_json(ReadPrefs(call->profile())));
  }
}

void HandleSetPreferences(const std::shared_ptr<ServiceCall>& call) {
  if (!Available(call)) {
    return;
  }
  bdkcore::JsonValue args;
  bdkcore::json_parse(call->builtin().arguments, &args);
  const bdkcore::TranslationPreferences before = ReadPrefs(call->profile());
  bdkcore::TranslationPreferences after = before;
  std::string message;
  if (!bdkcore::translation_apply_preferences(args, &after, &message)) {
    call->InvalidArguments(message);
    return;
  }
  WritePrefs(call->profile(), before, after);
  std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
  if (rt) {
    EmitPreferences(
        call->profile(), rt,
        call->builtin().profile ? call->builtin().profile->handle : 0);
  }
  call->Resolve(
      bdkcore::translation_preferences_json(ReadPrefs(call->profile())));
}

void HandleDownloadModel(const std::shared_ptr<ServiceCall>& call) {
  if (Available(call) && PairOk(call)) {
    Forward(call, "prepare", PairInput(call), true);
  }
}

void HandleRemoveModel(const std::shared_ptr<ServiceCall>& call) {
  if (Available(call) && PairOk(call)) {
    Forward(call, "remove_model", PairInput(call), true);
  }
}

void HandleState(const std::shared_ptr<ServiceCall>& call) {
  if (!Available(call) || !PageOf(call)) {
    return;
  }
  call->Resolve(bdkcore::translation_state_json(
      bdkcore::translation_state(*call->builtin().contents)));
}

void HandleTranslate(const std::shared_ptr<ServiceCall>& call) {
  if (!Available(call)) {
    return;
  }
  PageTranslation* page = PageOf(call);
  if (!page) {
    return;
  }
  const std::string from = call->StringOr("from", "");
  const std::string to = call->StringOr("to", "");
  if ((!from.empty() && !bdkcore::translation_language_ok(from)) ||
      (!to.empty() && !bdkcore::translation_language_ok(to))) {
    call->InvalidArguments("from and to are language codes");
    return;
  }
  page->Translate(from, to, call);
}

void HandleRevert(const std::shared_ptr<ServiceCall>& call) {
  if (!Available(call)) {
    return;
  }
  if (PageTranslation* page = PageOf(call)) {
    page->Revert();
    call->ResolveNull();
  }
}

void HandlePresent(const std::shared_ptr<ServiceCall>& call) {
  if (!Available(call)) {
    return;
  }
  if (PageTranslation* page = PageOf(call)) {
    page->RaiseRequest(/*require_pairs=*/false);
    call->ResolveNull();
  }
}

}  // namespace

void AttachTranslation(content::WebContents* contents) {
  PageTranslation::CreateForWebContents(contents);
}

void AddTranslationHandlers(HandlerTable& table) {
  table["translation.provider"] = &HandleProvider;
  table["translation.languages"] = &HandleLanguages;
  table["translation.preferences"] = &HandlePreferences;
  table["translation.setPreferences"] = &HandleSetPreferences;
  table["translation.downloadModel"] = &HandleDownloadModel;
  table["translation.removeModel"] = &HandleRemoveModel;
  table["translation.state"] = &HandleState;
  table["translation.translate"] = &HandleTranslate;
  table["translation.revert"] = &HandleRevert;
  table["translation.present"] = &HandlePresent;
}

}  // namespace bdk
