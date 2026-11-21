// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// search.* (services.md 5.13, T-SVC-10) on Chromium's TemplateURLService and
// omnibox: engines, the address bar's classification, and suggestions from
// history, bookmarks and keyword engines. A search engine's suggest service
// is asked only when the profile's search-suggest preference is on.
// search.enginesChanged comes from observing TemplateURLService, so a change
// made anywhere (these commands, Chromium's settings page, sync) is announced.

#include <memory>
#include <string>
#include <utility>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/engine/services/events.h"
#include "bdk/engine/services/services.h"
#include "chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "chrome/browser/autocomplete/chrome_autocomplete_provider_client.h"
#include "chrome/browser/autocomplete/chrome_autocomplete_scheme_classifier.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/common/pref_names.h"
#include "components/history/core/browser/history_service.h"
#include "components/omnibox/browser/autocomplete_classifier.h"
#include "components/omnibox/browser/autocomplete_controller.h"
#include "components/omnibox/browser/autocomplete_controller_config.h"
#include "components/omnibox/browser/autocomplete_enums.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/omnibox/browser/autocomplete_match_type.h"
#include "components/omnibox/browser/autocomplete_provider.h"
#include "components/prefs/pref_service.h"
#include "components/search_engines/template_url.h"
#include "components/search_engines/template_url_data.h"
#include "components/search_engines/template_url_service.h"
#include "components/search_engines/template_url_service_observer.h"
#include "third_party/metrics_proto/omnibox_event.pb.h"
#include "url/gurl.h"

namespace bdk {
namespace {

constexpr int kDefaultSuggestions = 8;
constexpr char kSearchTerms[] = "{searchTerms}";

TemplateURLService* EnginesOf(const ServiceCall& c) {
  return TemplateURLServiceFactory::GetForProfile(c.original_profile());
}

std::string EngineJson(const TemplateURL& engine, const TemplateURL* def) {
  return "{\"id\":" + JsonString(engine.sync_guid()) +
         ",\"name\":" + JsonString(base::UTF16ToUTF8(engine.short_name())) +
         ",\"keyword\":" + JsonString(base::UTF16ToUTF8(engine.keyword())) +
         ",\"searchUrl\":" + JsonString(engine.url()) +
         ",\"suggestUrl\":" + JsonStringOrNull(engine.suggestions_url()) +
         ",\"faviconUrl\":" +
         JsonStringOrNull(engine.favicon_url().is_valid()
                              ? engine.favicon_url().spec()
                              : std::string()) +
         ",\"isDefault\":" + (&engine == def ? "true" : "false") +
         ",\"source\":" +
         (engine.prepopulate_id() > 0 ||
                  engine.policy_origin() !=
                      TemplateURLData::PolicyOrigin::kNoPolicy
              ? "\"product\""
              : "\"user\"") +
         "}";
}

std::string EnginesJson(TemplateURLService* service) {
  const TemplateURL* def = service->GetDefaultSearchProvider();
  std::string out = "[";
  for (const TemplateURL* engine : service->GetTemplateURLs()) {
    if (engine->type() != TemplateURL::NORMAL) {
      continue;  // extension and omnibox-API keywords are the extensions'
    }
    out += (out.size() > 1 ? "," : "") + EngineJson(*engine, def);
  }
  return out + "]";
}

// Announces the engines whenever TemplateURLService reports a change that
// alters what search.engines answers.
class SearchEvents : public ServiceEvents, public TemplateURLServiceObserver {
 public:
  SearchEvents(TemplateURLService* service,
               std::weak_ptr<bdkcore::Profile> profile)
      : service_(service), profile_(std::move(profile)) {
    if (service_->loaded()) {
      last_ = EnginesJson(service_);
    }
    observation_.Observe(service_);
  }

  void OnTemplateURLServiceChanged() override {
    if (!service_->loaded()) {
      return;
    }
    std::string engines = EnginesJson(service_);
    if (engines == last_) {
      return;
    }
    const bool first = last_.empty();
    last_ = std::move(engines);
    if (first) {
      return;  // loading the profile's engines is not a change
    }
    EmitEvent(profile_, "search.enginesChanged",
              "{\"engines\":" + last_ + ",\"source\":\"application\"}");
  }

  void OnTemplateURLServiceShuttingDown() override { observation_.Reset(); }

 private:
  raw_ptr<TemplateURLService> service_;
  std::weak_ptr<bdkcore::Profile> profile_;
  std::string last_;
  base::ScopedObservation<TemplateURLService, TemplateURLServiceObserver>
      observation_{this};
};

// Runs `then` once the engines are loaded from the profile.
void WhenLoaded(const std::shared_ptr<ServiceCall>& c,
                void (*then)(const std::shared_ptr<ServiceCall>&,
                             TemplateURLService*)) {
  TemplateURLService* service = EnginesOf(*c);
  if (!service) {
    c->Reject(BDK_DOMAIN_PLATFORM, "unavailable",
              "search engines are not available");
    return;
  }
  if (service->loaded()) {
    then(c, service);
    return;
  }
  auto subscription = std::make_shared<base::CallbackListSubscription>();
  *subscription = service->RegisterOnLoadedCallback(base::BindOnce(
      [](std::shared_ptr<ServiceCall> call,
         void (*next)(const std::shared_ptr<ServiceCall>&, TemplateURLService*),
         std::shared_ptr<base::CallbackListSubscription>) {
        if (!call->cancelled()) {
          next(call, EnginesOf(*call));
        }
      },
      c, then, subscription));
  service->Load();
}

// The checks of an engine's fields; empty when they hold.
std::string EngineProblem(TemplateURLService* service,
                          const std::u16string& name,
                          const std::u16string& keyword,
                          const std::string& search_url,
                          const std::string& suggest_url,
                          const TemplateURL* self) {
  if (name.empty()) {
    return "name: empty";
  }
  if (keyword.empty() || keyword.find(u' ') != std::u16string::npos) {
    return "keyword: must be one word";
  }
  const TemplateURL* taken = service->GetTemplateURLForKeyword(keyword);
  if (taken && taken != self) {
    return "keyword: another engine has it";
  }
  const GURL url(search_url);
  if (!url.SchemeIsHTTPOrHTTPS() ||
      search_url.find(kSearchTerms) == std::string::npos) {
    return "searchUrl: an http(s) URL with {searchTerms}";
  }
  if (!suggest_url.empty() && !GURL(suggest_url).SchemeIsHTTPOrHTTPS()) {
    return "suggestUrl: an http(s) URL";
  }
  return std::string();
}

TemplateURL* EngineFor(const ServiceCall& c, TemplateURLService* service) {
  TemplateURL* engine = service->GetTemplateURLForGUID(c.StringOr("id", ""));
  if (!engine) {
    c.InvalidArguments("no search engine '" + c.StringOr("id", "") + "'");
  }
  return engine;
}

void ListEngines(const std::shared_ptr<ServiceCall>& c, TemplateURLService* s) {
  c->Resolve("{\"engines\":" + EnginesJson(s) + "}");
}

void AddEngine(const std::shared_ptr<ServiceCall>& c, TemplateURLService* s) {
  const std::u16string name = base::UTF8ToUTF16(c->StringOr("name", ""));
  const std::u16string keyword = base::UTF8ToUTF16(c->StringOr("keyword", ""));
  const std::string url = c->StringOr("searchUrl", "");
  const std::string suggest = c->StringOr("suggestUrl", "");
  if (std::string problem =
          EngineProblem(s, name, keyword, url, suggest, nullptr);
      !problem.empty()) {
    c->InvalidArguments(problem);
    return;
  }
  TemplateURLData data;
  data.SetShortName(name);
  data.SetKeyword(keyword);
  data.SetURL(url);
  data.suggestions_url = suggest;
  data.favicon_url = GURL(c->StringOr("faviconUrl", ""));
  data.safe_for_autoreplace = false;
  TemplateURL* added = s->Add(std::make_unique<TemplateURL>(data));
  if (!added) {
    c->InvalidArguments("keyword: another engine has it");
    return;
  }
  c->Resolve(EngineJson(*added, s->GetDefaultSearchProvider()));
}

void UpdateEngine(const std::shared_ptr<ServiceCall>& c,
                  TemplateURLService* s) {
  TemplateURL* engine = EngineFor(*c, s);
  if (!engine) {
    return;
  }
  const std::u16string name = c->String("name")
                                  ? base::UTF8ToUTF16(*c->String("name"))
                                  : engine->short_name();
  const std::u16string keyword = c->String("keyword")
                                     ? base::UTF8ToUTF16(*c->String("keyword"))
                                     : engine->keyword();
  const std::string url = c->StringOr("searchUrl", engine->url());
  const std::string suggest =
      c->StringOr("suggestUrl", engine->suggestions_url());
  if (std::string problem =
          EngineProblem(s, name, keyword, url, suggest, engine);
      !problem.empty()) {
    c->InvalidArguments(problem);
    return;
  }
  if (c->String("suggestUrl") || c->String("faviconUrl")) {
    // TemplateURLService changes only name, keyword and URL in place
    // (ResetTemplateURL): the engine is replaced by one with the same id.
    if (engine == s->GetDefaultSearchProvider()) {
      c->Reject(BDK_DOMAIN_BROKER, BDK_CODE_NOT_IMPLEMENTED,
                "the default engine's suggest and favicon URLs cannot be "
                "changed; make another engine the default first");
      return;
    }
    TemplateURLData data(engine->data());
    data.SetShortName(name);
    data.SetKeyword(keyword);
    data.SetURL(url);
    data.suggestions_url = suggest;
    if (std::optional<std::string> favicon = c->String("faviconUrl")) {
      data.favicon_url = GURL(*favicon);
    }
    data.safe_for_autoreplace = false;
    data.id = kInvalidTemplateURLID;
    s->Remove(engine);
    engine = s->Add(std::make_unique<TemplateURL>(data));
    if (!engine) {
      c->Reject(BDK_DOMAIN_PLATFORM, "unavailable",
                "the engine could not be replaced");
      return;
    }
  } else {
    s->ResetTemplateURL(engine, name, keyword, url);
  }
  c->Resolve(EngineJson(*engine, s->GetDefaultSearchProvider()));
}

void RemoveEngine(const std::shared_ptr<ServiceCall>& c,
                  TemplateURLService* s) {
  TemplateURL* engine = EngineFor(*c, s);
  if (!engine) {
    return;
  }
  if (engine == s->GetDefaultSearchProvider()) {
    c->InvalidArguments("id: the default engine cannot be removed");
    return;
  }
  s->Remove(engine);
  c->ResolveNull();
}

void SetDefault(const std::shared_ptr<ServiceCall>& c, TemplateURLService* s) {
  TemplateURL* engine = EngineFor(*c, s);
  if (!engine) {
    return;
  }
  if (!s->CanMakeDefault(engine)) {
    c->Reject(BDK_DOMAIN_PLATFORM, "unavailable",
              "this engine cannot be the default (a policy or an extension "
              "controls the default)");
    return;
  }
  s->SetUserSelectedDefaultSearchProvider(engine);
  c->ResolveNull();
}

void Classify(const std::shared_ptr<ServiceCall>& c, TemplateURLService* s) {
  const std::u16string text = base::UTF8ToUTF16(c->StringOr("text", ""));
  if (text.find_first_not_of(u" \t\n") == std::u16string::npos) {
    c->InvalidArguments("text: empty");
    return;
  }
  AutocompleteMatch match;
  AutocompleteClassifierFactory::GetForProfile(c->profile())
      ->Classify(text, /*in_keyword_mode=*/false,
                 /*allow_exact_keyword_match=*/true,
                 metrics::OmniboxEventProto::OTHER, &match,
                 /*alternate_nav_url=*/nullptr);
  const bool search = AutocompleteMatch::IsSearchType(match.type);
  std::string engine_id;
  if (search) {
    if (const TemplateURL* engine = match.GetTemplateURL(s)) {
      engine_id = engine->sync_guid();
    }
  }
  const bool has_url = match.destination_url.is_valid();
  c->Resolve(std::string("{\"kind\":") + (search ? "\"search\"" : "\"url\"") +
             ",\"url\":" +
             JsonStringOrNull(has_url ? match.destination_url.spec() : "") +
             ",\"engineId\":" + JsonStringOrNull(engine_id) + "}");
}

int Bit(AutocompleteProvider::Type type) {
  return static_cast<int>(type);
}

const char* SuggestionType(const AutocompleteMatch& match) {
  if (match.type == omnibox::AutocompleteMatchType::kUrlWhatYouTyped) {
    return "url";
  }
  if (AutocompleteMatch::IsSearchType(match.type)) {
    return "search";
  }
  if (match.provider) {
    switch (match.provider->type()) {
      case AutocompleteProvider::Type::kBookmark:
        return "bookmark";
      case AutocompleteProvider::Type::kKeyword:
        return "keyword";
      default:
        break;
    }
  }
  return "history";
}

// One suggestions call: an AutocompleteController that answers when it is
// done, or when the call is cancelled.
class SuggestionsRun : public AutocompleteController::Observer {
 public:
  SuggestionsRun(std::shared_ptr<ServiceCall> call, size_t limit)
      : call_(std::move(call)), limit_(limit) {
    Profile* profile = call_->profile();
    AutocompleteControllerConfig config;
    config.provider_types = Bit(AutocompleteProvider::Type::kBookmark) |
                            Bit(AutocompleteProvider::Type::kHistoryQuick) |
                            Bit(AutocompleteProvider::Type::kHistoryUrl) |
                            Bit(AutocompleteProvider::Type::kKeyword);
    // The engine's suggest service sees typed text only with the user's
    // consent (services.md 5.13).
    if (profile->GetPrefs()->GetBoolean(prefs::kSearchSuggestEnabled)) {
      config.provider_types |= Bit(AutocompleteProvider::Type::kSearch);
    }
    controller_ = std::make_unique<AutocompleteController>(
        std::make_unique<ChromeAutocompleteProviderClient>(profile), config);
    controller_->AddObserver(this);
    AutocompleteInput input(base::UTF8ToUTF16(call_->StringOr("text", "")),
                            metrics::OmniboxEventProto::OTHER,
                            ChromeAutocompleteSchemeClassifier(profile));
    base::WeakPtr<SuggestionsRun> weak = weak_factory_.GetWeakPtr();
    call_->control().OnCancel([weak] {
      if (weak) {
        weak->Finish(false);
      }
    });
    controller_->Start(input);
  }

  ~SuggestionsRun() override { controller_->RemoveObserver(this); }

  // AutocompleteController::Observer:
  void OnResultChanged(AutocompleteController* controller,
                       bool default_match_changed) override {
    if (controller->done()) {
      Finish(true);
    }
  }

 private:
  void Finish(bool answer) {
    if (done_) {
      return;
    }
    done_ = true;
    if (answer) {
      std::string out = "[";
      size_t n = 0;
      for (const AutocompleteMatch& match : controller_->result()) {
        if (n++ == limit_) {
          break;
        }
        out += (out.size() > 1 ? "," : "") + std::string("{\"type\":\"") +
               SuggestionType(match) +
               "\",\"text\":" + JsonString(base::UTF16ToUTF8(match.contents)) +
               ",\"url\":" + JsonString(match.destination_url.spec()) +
               ",\"relevance\":" + base::NumberToString(match.relevance) +
               ",\"inlineCompletion\":" +
               JsonStringOrNull(
                   match.allowed_to_be_default_match
                       ? base::UTF16ToUTF8(match.inline_autocompletion)
                       : std::string()) +
               "}";
      }
      call_->Resolve("{\"suggestions\":" + out + "]}");
    }
    controller_->Stop(AutocompleteStopReason::kClobbered);
    // Never from inside the controller's own call.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, this);
  }

  std::shared_ptr<ServiceCall> call_;
  const size_t limit_;
  std::unique_ptr<AutocompleteController> controller_;
  bool done_ = false;
  base::WeakPtrFactory<SuggestionsRun> weak_factory_{this};
};

void Suggestions(const std::shared_ptr<ServiceCall>& c, TemplateURLService*) {
  const std::string text = c->StringOr("text", "");
  const double limit = c->Number("limit").value_or(kDefaultSuggestions);
  if (limit < 1) {
    c->InvalidArguments("limit: at least 1");
    return;
  }
  if (text.empty()) {
    c->Resolve("{\"suggestions\":[]}");
    return;
  }
  new SuggestionsRun(c, static_cast<size_t>(limit));  // deletes itself
}

void DeleteSuggestion(const std::shared_ptr<ServiceCall>& c) {
  history::HistoryService* history = HistoryServiceFactory::GetForProfile(
      c->original_profile(), ServiceAccessType::EXPLICIT_ACCESS);
  if (!history) {
    c->Reject(BDK_DOMAIN_PLATFORM, "unavailable", "history is not available");
    return;
  }
  history->DeleteURLs({GURL(c->StringOr("url", ""))});
  c->ResolveNull();
}

}  // namespace

std::unique_ptr<ServiceEvents> CreateSearchEvents(
    Profile* profile,
    std::weak_ptr<bdkcore::Profile> bdk_profile) {
  TemplateURLService* service =
      TemplateURLServiceFactory::GetForProfile(profile);
  if (!service) {
    return nullptr;
  }
  return std::make_unique<SearchEvents>(service, std::move(bdk_profile));
}

void AddSearchHandlers(HandlerTable& table) {
  table["search.engines"] = [](const std::shared_ptr<ServiceCall>& c) {
    WhenLoaded(c, &ListEngines);
  };
  table["search.addEngine"] = [](const std::shared_ptr<ServiceCall>& c) {
    WhenLoaded(c, &AddEngine);
  };
  table["search.updateEngine"] = [](const std::shared_ptr<ServiceCall>& c) {
    WhenLoaded(c, &UpdateEngine);
  };
  table["search.removeEngine"] = [](const std::shared_ptr<ServiceCall>& c) {
    WhenLoaded(c, &RemoveEngine);
  };
  table["search.setDefault"] = [](const std::shared_ptr<ServiceCall>& c) {
    WhenLoaded(c, &SetDefault);
  };
  table["search.classify"] = [](const std::shared_ptr<ServiceCall>& c) {
    WhenLoaded(c, &Classify);
  };
  table["search.suggestions"] = [](const std::shared_ptr<ServiceCall>& c) {
    WhenLoaded(c, &Suggestions);
  };
  table["search.deleteSuggestion"] = &DeleteSuggestion;
}

}  // namespace bdk
