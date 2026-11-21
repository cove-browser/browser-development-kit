/* The stub's provider services (stub_providers.h) and their test hooks (bdk_stub_providers.h). */
#include "stub_providers.h"

#include <chrono>

#include "../runtime/providers_internal.h"
#include "bdk_stub_providers.h"

using namespace bdkcore;

namespace bdkstub {

namespace {

/* What the stub knows of a page that a real engine reads from its DOM. */
struct Page {
    std::vector<std::string> original, shown;
    std::string language; /* as the page declared it */
    std::shared_ptr<TranslationJob> job;
    std::shared_ptr<ProviderRequest> watch;
    std::string last_position;
};

struct State {
    std::map<bdk_contents_t, Page> pages;
    std::map<std::string, TranslationPreferences> prefs; /* by profile id */
};

State& st() {
    static State* s = new State;
    return *s;
}

Page& page_of(const Contents& c) {
    return st().pages[c.handle];
}

TranslationPreferences& prefs_of(const Profile* p) {
    return st().prefs[p ? p->id : std::string()];
}

std::string quoted(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/* scheme://host[:port] of a URL, the "site" of never-for-this-site. */
std::string site_of(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return "";
    size_t end = url.find_first_of("/?#", scheme + 3);
    return url.substr(0, end);
}

std::string url_of(const Contents& c) {
    ContentsState s;
    backend().State(c, &s);
    return s.url;
}

/* The stub has no translation engine: its default provider is unavailable, like the engine's until
   the engine of T-PRV-02 is in it. */
std::shared_ptr<ProviderRequest> no_engine(const std::string&, const std::string&,
                                           ProviderHandlers h) {
    auto done = std::move(h.done);
    post([done] {
        ProviderReply r;
        r.domain = BDK_DOMAIN_PROVIDER;
        r.code = "unavailable";
        r.message = "this runtime has no default translation engine";
        if (done)
            done(r);
    });
    return nullptr;
}

void emit_preferences(const std::shared_ptr<Runtime>& rt, const Profile* p) {
    emit_builtin_event(rt, "translation.preferencesChanged", p ? p->handle : 0,
                       translation_preferences_json(prefs_of(p)));
}

std::string strings_json(const std::vector<std::string>& texts) {
    std::string out = "[";
    for (size_t i = 0; i < texts.size(); ++i)
        out += (i ? "," : "") + quoted(texts[i]);
    return out + "]";
}

void add_unique(std::vector<std::string>* list, const std::string& value) {
    if (std::find(list->begin(), list->end(), value) == list->end())
        list->push_back(value);
}

/* Translates the page from its original text. control is the command's, or null when the request or
   the always list started it. */
void start_translation(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c,
                       const std::string& from, const std::string& to,
                       const std::shared_ptr<CommandControl>& control) {
    Page& pg = page_of(*c);
    TranslationState s = translation_state(*c);
    const std::string src = from.empty() ? s.detected : from;
    const std::string dst = to.empty() ? prefs_of(c->profile.get()).target : to;
    if (src.empty()) {
        if (control)
            control->Reject(
                BDK_DOMAIN_PROVIDER, "unsupportedPair",
                "the page's language is not known: name the language to translate from");
        return;
    }
    if (src == dst) {
        if (control)
            control->Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                            "the page is already in " + dst);
        return;
    }
    if (pg.job)
        pg.job->Cancel();
    pg.shown = pg.original;
    s.phase = "translating";
    s.from = src;
    s.to = dst;
    s.progress = 0;
    s.error_domain = s.error_code = s.error_message = "";
    translation_set_state(c, s);

    std::weak_ptr<Contents> weak = c;
    TranslationJobSpec spec;
    spec.from = src;
    spec.to = dst;
    spec.url = url_of(*c);
    spec.texts = pg.original;
    spec.builtin = no_engine;
    spec.progress = [weak, control](double f) {
        auto page = weak.lock();
        if (!page)
            return;
        TranslationState now = translation_state(*page);
        now.progress = f;
        translation_set_state(page, now);
        if (control)
            control->Progress(f, "translating");
    };
    spec.done = [weak, control](const ProviderReply& reply, const std::vector<std::string>& texts) {
        auto page = weak.lock();
        if (!page) {
            if (control)
                control->Reject(BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "the page is gone");
            return;
        }
        Page& p = page_of(*page);
        p.job.reset();
        TranslationState now = translation_state(*page);
        now.progress = -1;
        if (reply.ok()) {
            p.shown = texts;
            now.phase = "translated";
        } else if (reply.code == BDK_CODE_CANCELLED) {
            /* Reverted or navigated away: whoever cancelled has set the state already. */
            if (now.phase == "translating") {
                now.phase = "available";
                now.from = now.to = "";
            }
        } else {
            now.phase = "failed";
            now.error_domain = reply.domain;
            now.error_code = reply.code;
            now.error_message = reply.message;
        }
        translation_set_state(page, now);
        if (!control)
            return;
        if (reply.ok())
            control->Resolve(translation_state_json(now));
        else
            control->Reject(reply.domain.c_str(), reply.code.c_str(), reply.message);
    };
    pg.job = translation_job_start(rt, std::move(spec));
    if (control) {
        std::weak_ptr<TranslationJob> job = pg.job;
        control->OnCancel([job] {
            if (auto j = job.lock())
                j->Cancel();
        });
    }
}

void revert(const std::shared_ptr<Contents>& c) {
    Page& pg = page_of(*c);
    if (pg.job)
        pg.job->Cancel();
    pg.job.reset();
    pg.shown = pg.original;
    TranslationState s = translation_state(*c);
    if (s.phase == "unavailable")
        return;
    s.phase = "available";
    s.from = s.to = "";
    s.progress = -1;
    s.error_domain = s.error_code = s.error_message = "";
    translation_set_state(c, s);
}

void answer(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c,
            const std::string& completion, const std::string& arguments) {
    TranslationAnswer a;
    std::string message;
    if (!translation_parse_answer(completion, arguments, &a, &message)) {
        log_line(rt, 2, "translation request: " + message);
        return;
    }
    TranslationPreferences& prefs = prefs_of(c->profile.get());
    const std::string language = a.language.empty() ? translation_state(*c).detected : a.language;
    using K = TranslationAnswer::Kind;
    switch (a.kind) {
    case K::kTranslate:
        start_translation(rt, c, a.from, a.to, nullptr);
        break;
    case K::kRevert:
        revert(c);
        break;
    case K::kAlways:
        if (language.empty())
            return;
        add_unique(&prefs.always, language);
        std::erase(prefs.never, language);
        emit_preferences(rt, c->profile.get());
        start_translation(rt, c, language, "", nullptr);
        break;
    case K::kNever:
        if (language.empty())
            return;
        add_unique(&prefs.never, language);
        std::erase(prefs.always, language);
        emit_preferences(rt, c->profile.get());
        break;
    case K::kNeverForSite:
        add_unique(&prefs.never_sites, site_of(url_of(*c)));
        emit_preferences(rt, c->profile.get());
        break;
    case K::kDismiss:
        break;
    }
}

/* Raises the translation request for the page in its current state, once the provider has said
   which languages it offers. A provider that cannot answer offers nothing. */
void raise_request(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c,
                   bool require_pairs) {
    std::weak_ptr<Contents> weak = c;
    ProviderHandlers h;
    h.done = [rt, weak, require_pairs](const ProviderReply& reply) {
        auto page = weak.lock();
        if (!page || page->dead || (require_pairs && !reply.ok()))
            return;
        std::string pairs = "[]";
        if (reply.ok()) {
            JsonValue v;
            json_parse(reply.result_json, &v);
            if (const JsonValue* list = v.find("pairs"))
                pairs = json_serialize(*list);
        }
        auto r = std::make_shared<Request>();
        r->surface = BDK_SURFACE_TRANSLATION;
        r->contents = page->handle;
        r->profile = page->profile ? page->profile->handle : 0;
        r->payload = translation_request_payload(translation_state(*page),
                                                 prefs_of(page->profile.get()).target, pairs,
                                                 provider_descriptor(*rt, Service::kTranslation));
        r->on_finish = [rt, weak](UiOutcome outcome, const std::string& completion,
                                  const std::string& args) {
            auto p = weak.lock();
            if (p && !p->dead && outcome == UiOutcome::kCompleted)
                answer(rt, p, completion, args);
        };
        raise_ui_request(rt, r);
    };
    translation_call(rt, no_engine, "supported_pairs", "{}", std::move(h));
}

/* After a load: the page's language, then an offer or an automatic translation. */
void page_loaded(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c,
                 const std::string& language) {
    std::weak_ptr<Contents> weak = c;
    auto decide = [rt, weak](const std::string& detected) {
        auto page = weak.lock();
        if (!page || page->dead)
            return;
        TranslationState s;
        s.detected = detected;
        s.phase = detected.empty() ? "unavailable" : "available";
        translation_set_state(page, s);
        if (detected.empty() || !ui_surface_available(*rt, BDK_SURFACE_TRANSLATION))
            return;
        TranslationDecision d =
            translation_decide(prefs_of(page->profile.get()), detected, site_of(url_of(*page)));
        if (d.automatic)
            start_translation(rt, page, detected, "", nullptr);
        else if (d.offer)
            raise_request(rt, page, true);
    };
    if (!language.empty() || !provider_has_operation(*rt, Service::kTranslation, "detect")) {
        decide(language);
        return;
    }
    std::string sample;
    for (const std::string& t : page_of(*c).original) {
        if (sample.size() > 2048)
            break;
        sample += t + "\n";
    }
    ProviderHandlers h;
    h.done = [decide](const ProviderReply& reply) {
        std::string detected;
        if (reply.ok() && reply.result_json != "null") {
            JsonValue v;
            json_parse(reply.result_json, &v);
            if (const JsonValue* l = v.find("language"))
                detected = l->text;
        }
        decide(detected);
    };
    provider_invoke(rt, Service::kTranslation, "detect", "{\"sample\":" + quoted(sample) + "}",
                    std::move(h));
}

/* The page navigates: its text, state and translation go. */
void reset_page(const std::shared_ptr<Contents>& c) {
    auto it = st().pages.find(c->handle);
    if (it != st().pages.end()) {
        if (it->second.job)
            it->second.job->Cancel();
        it->second.job.reset();
        it->second.original.clear();
        it->second.shown.clear();
        it->second.language.clear();
    }
    if (translation_state(*c).phase != "unavailable")
        translation_set_state(c, TranslationState{});
}

void warn(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c,
          const std::string& url, const std::string& verdict, const std::function<void(bool)>& go) {
    auto r = std::make_shared<Request>();
    r->surface = BDK_SURFACE_REPUTATION_WARNINGS;
    r->contents = c->handle;
    r->profile = c->profile ? c->profile->handle : 0;
    r->payload = reputation_warning_payload(*rt, url, verdict);
    /* No handler: the default presenter is the interstitial, which keeps the page blocked. */
    r->on_finish = [go](UiOutcome outcome, const std::string& completion, const std::string&) {
        go(outcome == UiOutcome::kCompleted && completion == "proceed");
    };
    raise_ui_request(rt, r);
}

bool unsafe(const std::string& verdict) {
    return verdict == "malware" || verdict == "phishing" || verdict == "unwantedSoftware";
}

} // namespace

void providers_reset() {
    State& s = st();
    for (auto& [handle, page] : s.pages) {
        if (page.job)
            page.job->Cancel();
        if (page.watch)
            page.watch->Cancel();
    }
    s.pages.clear();
    s.prefs.clear();
}

bool providers_execute(Runtime& rt, const std::shared_ptr<BuiltinCall>& call) {
    if (call->name.compare(0, 12, "translation.") != 0)
        return false;
    auto control = call->control;
    auto runtime = current_runtime();
    if (!runtime || !ui_surface_available(rt, BDK_SURFACE_TRANSLATION)) {
        control->Reject(BDK_DOMAIN_PROVIDER, "unavailable", "translation is disabled");
        return true;
    }
    JsonValue args;
    json_parse(call->arguments, &args);
    const std::string& n = call->name;
    const Profile* profile = call->profile.get();
    auto reply_to = [control](const ProviderReply& r) {
        if (r.ok())
            control->Resolve(r.result_json);
        else
            control->Reject(r.domain.c_str(), r.code.c_str(), r.message);
    };
    auto models_changed = [runtime, profile_handle = profile ? profile->handle : 0] {
        ProviderHandlers h;
        h.done = [runtime, profile_handle](const ProviderReply& r) {
            if (r.ok())
                emit_builtin_event(runtime, "translation.modelsChanged", profile_handle,
                                   r.result_json);
        };
        translation_call(runtime, no_engine, "supported_pairs", "{}", std::move(h));
    };
    auto pair_input = [&] {
        return "{\"from\":" + quoted(args.find("from")->text) +
               ",\"to\":" + quoted(args.find("to")->text) + "}";
    };
    auto keep = [control](const std::shared_ptr<ProviderRequest>& request) {
        if (request)
            control->OnCancel([request] { request->Cancel(); });
    };

    if (n == "translation.provider") {
        control->Resolve(provider_descriptor(rt, Service::kTranslation));
    } else if (n == "translation.languages") {
        ProviderHandlers h;
        h.done = reply_to;
        keep(translation_call(runtime, no_engine, "supported_pairs", "{}", std::move(h)));
    } else if (n == "translation.preferences") {
        control->Resolve(translation_preferences_json(prefs_of(profile)));
    } else if (n == "translation.setPreferences") {
        std::string message;
        if (!translation_apply_preferences(args, &prefs_of(profile), &message)) {
            control->Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS, message);
            return true;
        }
        emit_preferences(runtime, profile);
        control->Resolve(translation_preferences_json(prefs_of(profile)));
    } else if (n == "translation.downloadModel" || n == "translation.removeModel") {
        const bool download = n == "translation.downloadModel";
        if (!translation_language_ok(args.find("from")->text) ||
            !translation_language_ok(args.find("to")->text)) {
            control->Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                            "from and to are language codes");
            return true;
        }
        ProviderHandlers h;
        h.progress = [control](double f) { control->Progress(f, "downloading"); };
        h.done = [control, models_changed](const ProviderReply& r) {
            if (!r.ok()) {
                control->Reject(r.domain.c_str(), r.code.c_str(), r.message);
                return;
            }
            control->Resolve("null");
            models_changed();
        };
        keep(translation_call(runtime, no_engine, download ? "prepare" : "remove_model",
                              pair_input(), std::move(h)));
    } else {
        auto c = call->contents;
        if (!c) {
            control->Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                            "'" + n + "' needs a page: name one in the invoke options");
            return true;
        }
        if (n == "translation.state") {
            control->Resolve(translation_state_json(translation_state(*c)));
        } else if (n == "translation.translate") {
            const JsonValue* from = args.find("from");
            const JsonValue* to = args.find("to");
            std::string f = from && from->type == JsonValue::Type::kString ? from->text : "";
            std::string t = to && to->type == JsonValue::Type::kString ? to->text : "";
            if ((!f.empty() && !translation_language_ok(f)) ||
                (!t.empty() && !translation_language_ok(t))) {
                control->Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                                "from and to are language codes");
                return true;
            }
            start_translation(runtime, c, f, t, control);
        } else if (n == "translation.revert") {
            revert(c);
            control->Resolve("null");
        } else if (n == "translation.present") {
            raise_request(runtime, c, false);
            control->Resolve("null");
        } else {
            return false;
        }
    }
    return true;
}

void providers_check_navigation(const std::shared_ptr<Contents>& c, const std::string& url,
                                const std::function<void(bool)>& go) {
    reset_page(c);
    auto rt = c->rt.lock();
    if (!rt || (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)) {
        go(true);
        return;
    }
    const ProviderChoice& choice = provider_choice(*rt, Service::kReputation);
    if (choice.provider == "default") {
        const ReputationList* list = reputation_current_list(*rt);
        std::string verdict = list ? reputation_list_lookup(*list, url, now_ms()) : "safe";
        if (!unsafe(verdict)) {
            go(true);
            return;
        }
        warn(rt, c, url, verdict, go);
        return;
    }
    if (choice.provider != "app") {
        go(true); /* the stub has no Safe Browsing */
        return;
    }
    std::weak_ptr<Contents> weak = c;
    ProviderHandlers h;
    h.done = [rt, weak, url, go](const ProviderReply& reply) {
        auto page = weak.lock();
        if (!page || page->dead)
            return;
        std::string verdict = "unknown";
        if (reply.ok()) {
            JsonValue v;
            json_parse(reply.result_json, &v);
            verdict = v.find("verdict")->text;
        } else {
            log_line(rt, 2,
                     "the reputation provider could not check " + url + ": " + reply.message);
        }
        if (unsafe(verdict))
            warn(rt, page, url, verdict, go);
        else
            go(true); /* Like Safe Browsing, a check that fails lets the page load */
    };
    provider_invoke(rt, Service::kReputation, "check_url",
                    "{\"url\":" + quoted(url) + ",\"kind\":\"navigation\"}", std::move(h));
}

} // namespace bdkstub

/* ---- Test hooks ---- */

using namespace bdkstub;

extern "C" {

BDK_EXPORT bdk_status_t bdk_stub_page_text_set(bdk_contents_t h, bdk_string_t language,
                                               bdk_string_t texts_json) {
    if (bdk_status_t s = check_thread())
        return s;
    auto c = contents_of(h);
    auto rt = c ? c->rt.lock() : nullptr;
    if (!c || !rt)
        return BDK_ERR_STALE_HANDLE;
    JsonValue texts;
    std::string lang = string_ok(language) ? str(language) : "";
    if (!string_ok(texts_json) || !valid_json(str(texts_json)) ||
        !json_parse(str(texts_json), &texts) || texts.type != JsonValue::Type::kArray ||
        (!lang.empty() && !translation_language_ok(lang)))
        return BDK_ERR_INVALID_ARGUMENT;
    std::vector<std::string> list;
    for (const JsonValue& t : texts.items) {
        if (t.type != JsonValue::Type::kString)
            return BDK_ERR_INVALID_ARGUMENT;
        list.push_back(t.text);
    }
    reset_page(c);
    Page& pg = page_of(*c);
    pg.original = pg.shown = list;
    pg.language = lang;
    post([rt, c, lang] {
        if (!c->dead)
            page_loaded(rt, c, lang);
    });
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_page_text(bdk_contents_t h, bdk_buffer_t* out) {
    if (bdk_status_t s = check_thread())
        return s;
    auto c = contents_of(h);
    if (!c)
        return BDK_ERR_STALE_HANDLE;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    *out = make_buffer(strings_json(page_of(*c).shown));
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_reputation_list_load(bdk_runtime_t h, bdk_string_t list_json,
                                                      bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(list_json))
        return BDK_ERR_INVALID_ARGUMENT;
    ReputationList list;
    std::string message;
    if (!reputation_list_parse(str(list_json), &list, &message) ||
        !reputation_set_list(*rt, std::move(list), &message))
        return fail(out_error, BDK_DOMAIN_PROVIDER, "invalidList", message);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_geolocation_watch(bdk_contents_t h, uint8_t high_accuracy) {
    if (bdk_status_t s = check_thread())
        return s;
    auto c = contents_of(h);
    auto rt = c ? c->rt.lock() : nullptr;
    if (!c || !rt)
        return BDK_ERR_STALE_HANDLE;
    Page& pg = page_of(*c);
    if (pg.watch)
        pg.watch->Cancel();
    pg.watch.reset();
    pg.last_position.clear();
    const bdk_contents_t handle = c->handle;
    if (!provider_is_app(*rt, Service::kGeolocation)) {
        post([handle] {
            st().pages[handle].last_position =
                R"({"error":"positionUnavailable","message":"the stub runtime has no location"})";
        });
        return BDK_OK;
    }
    ProviderHandlers handlers;
    handlers.report = [handle](const std::string& position) {
        st().pages[handle].last_position = position;
    };
    handlers.done = [handle](const ProviderReply& reply) {
        Page& p = st().pages[handle];
        p.watch.reset();
        if (!reply.ok() && reply.code != BDK_CODE_CANCELLED)
            p.last_position =
                "{\"error\":\"positionUnavailable\",\"message\":" + quoted(reply.message) + "}";
    };
    pg.watch =
        provider_invoke(rt, Service::kGeolocation, "start",
                        high_accuracy ? R"({"highAccuracy":true})" : R"({"highAccuracy":false})",
                        std::move(handlers));
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_geolocation_last(bdk_contents_t h, bdk_buffer_t* out) {
    if (bdk_status_t s = check_thread())
        return s;
    auto c = contents_of(h);
    if (!c)
        return BDK_ERR_STALE_HANDLE;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    *out = make_buffer(page_of(*c).last_position);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_geolocation_stop(bdk_contents_t h) {
    if (bdk_status_t s = check_thread())
        return s;
    auto c = contents_of(h);
    if (!c)
        return BDK_ERR_STALE_HANDLE;
    Page& pg = page_of(*c);
    if (pg.watch)
        pg.watch->Cancel();
    pg.watch.reset();
    return BDK_OK;
}

} /* extern "C" */
