/* Translation without Chromium (providers_internal.h): preferences, a page's state and its events,
   when to offer, the job that sends a page's text to the provider in batches, and the request's
   payload and answers. */
#include <algorithm>
#include <cctype>

#include "providers_internal.h"

namespace bdkcore {

struct ProviderTable;
std::map<bdk_contents_t, TranslationState>& translation_states(Runtime& rt);

namespace {

std::string quoted(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

std::string string_array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i)
        out += (i ? "," : "") + quoted(items[i]);
    return out + "]";
}

/* The primary language subtag, lower case: "pt-BR" -> "pt". */
std::string primary(const std::string& code) {
    std::string p = code.substr(0, code.find('-'));
    std::transform(p.begin(), p.end(), p.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return p;
}

bool same_language(const std::string& a, const std::string& b) {
    return !a.empty() && !b.empty() && primary(a) == primary(b);
}

bool listed(const std::vector<std::string>& list, const std::string& language) {
    return std::any_of(list.begin(), list.end(),
                       [&](const std::string& l) { return same_language(l, language); });
}

bool blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char ch) { return std::isspace(ch) != 0; });
}

ProviderReply reply_of(const char* domain, const char* code, const std::string& message) {
    ProviderReply r;
    r.domain = domain;
    r.code = code;
    r.message = message;
    return r;
}

/* A list of languages or sites from setPreferences: checked one by one, duplicates dropped. */
bool read_list(const JsonValue& array, bool sites, std::vector<std::string>* out,
               std::string* message) {
    out->clear();
    for (const JsonValue& item : array.items) {
        bool ok = sites ? (item.text.compare(0, 7, "http://") == 0 ||
                           item.text.compare(0, 8, "https://") == 0 ||
                           item.text.find('.') != std::string::npos)
                        : translation_language_ok(item.text);
        if (!ok) {
            *message = "\"" + item.text + "\" is not a " + (sites ? "site" : "language code");
            return false;
        }
        if (std::find(out->begin(), out->end(), item.text) == out->end())
            out->push_back(item.text);
    }
    return true;
}

} // namespace

bool translation_language_ok(const std::string& code) {
    size_t dash = code.find('-');
    std::string first = code.substr(0, dash);
    if (first.size() < 2 || first.size() > 3 ||
        !std::all_of(first.begin(), first.end(),
                     [](unsigned char ch) { return std::isalpha(ch) != 0; }))
        return false;
    if (dash == std::string::npos)
        return true;
    std::string rest = code.substr(dash + 1);
    size_t start = 0;
    while (start <= rest.size()) {
        size_t end = rest.find('-', start);
        std::string tag = rest.substr(start, end == std::string::npos ? end : end - start);
        if (tag.empty() || tag.size() > 8 ||
            !std::all_of(tag.begin(), tag.end(),
                         [](unsigned char ch) { return std::isalnum(ch) != 0; }))
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

std::string translation_preferences_json(const TranslationPreferences& p) {
    return "{\"target\":" + quoted(p.target) + ",\"alwaysTranslate\":" + string_array(p.always) +
           ",\"neverTranslate\":" + string_array(p.never) +
           ",\"neverSites\":" + string_array(p.never_sites) +
           ",\"offers\":" + (p.offers ? "true" : "false") + "}";
}

bool translation_apply_preferences(const JsonValue& args, TranslationPreferences* prefs,
                                   std::string* message) {
    TranslationPreferences next = *prefs;
    if (const JsonValue* target = args.find("target");
        target && target->type == JsonValue::Type::kString) {
        if (!translation_language_ok(target->text)) {
            *message = "\"" + target->text + "\" is not a language code";
            return false;
        }
        next.target = target->text;
    }
    struct {
        const char* key;
        bool sites;
        std::vector<std::string>* into;
    } lists[] = {{"alwaysTranslate", false, &next.always},
                 {"neverTranslate", false, &next.never},
                 {"neverSites", true, &next.never_sites}};
    for (auto& l : lists) {
        const JsonValue* v = args.find(l.key);
        if (v && v->type == JsonValue::Type::kArray && !read_list(*v, l.sites, l.into, message))
            return false;
    }
    if (const JsonValue* offers = args.find("offers");
        offers && offers->type == JsonValue::Type::kBool)
        next.offers = offers->boolean;
    /* A language is either always or never translated: the list written last in this call wins. */
    if (args.find("alwaysTranslate") &&
        args.find("alwaysTranslate")->type == JsonValue::Type::kArray) {
        std::erase_if(next.never, [&](const std::string& l) { return listed(next.always, l); });
    } else {
        std::erase_if(next.always, [&](const std::string& l) { return listed(next.never, l); });
    }
    *prefs = std::move(next);
    return true;
}

std::string translation_state_json(const TranslationState& s) {
    std::string out = "{\"phase\":" + quoted(s.phase);
    if (!s.detected.empty())
        out += ",\"detected\":" + quoted(s.detected);
    if (!s.from.empty())
        out += ",\"from\":" + quoted(s.from);
    if (!s.to.empty())
        out += ",\"to\":" + quoted(s.to);
    if (s.progress >= 0)
        out += ",\"progress\":" + json_serialize([&] {
                   JsonValue v;
                   v.type = JsonValue::Type::kNumber;
                   v.number = s.progress;
                   return v;
               }());
    if (!s.error_code.empty())
        out += ",\"error\":{\"domain\":" + quoted(s.error_domain) +
               ",\"code\":" + quoted(s.error_code) + ",\"message\":" + quoted(s.error_message) +
               "}";
    return out + "}";
}

const TranslationState& translation_state(const Contents& c) {
    static const TranslationState kNone;
    auto rt = c.rt.lock();
    if (!rt)
        return kNone;
    auto& states = translation_states(*rt);
    auto it = states.find(c.handle);
    return it == states.end() ? kNone : it->second;
}

void translation_set_state(const std::shared_ptr<Contents>& c, const TranslationState& state) {
    auto rt = c->rt.lock();
    if (!rt || c->dead)
        return;
    auto& states = translation_states(*rt);
    /* Forget pages that are gone. */
    std::erase_if(states,
                  [&](const auto& kv) { return kv.first != c->handle && !contents_of(kv.first); });
    std::string before = translation_state_json(translation_state(*c));
    std::string after = translation_state_json(state);
    states[c->handle] = state;
    if (before == after)
        return;
    std::string url;
    {
        ContentsState cs;
        backend().State(*c, &cs);
        url = cs.url;
    }
    emit_contents_event(c, BDK_CONTENTS_EVENT_TRANSLATION, url, after, 0, 0);
    emit_builtin_event(rt, "translation.stateChanged", c->profile ? c->profile->handle : 0,
                       "{\"page\":" + quoted(std::to_string(c->handle)) + ",\"state\":" + after +
                           "}");
}

TranslationDecision translation_decide(const TranslationPreferences& p, const std::string& language,
                                       const std::string& site) {
    TranslationDecision d;
    if (language.empty() || language == "und" || !translation_language_ok(language) ||
        same_language(language, p.target))
        return d;
    if (listed(p.always, language)) {
        d.automatic = true;
        return d;
    }
    if (!p.offers || listed(p.never, language))
        return d;
    if (!site.empty() &&
        std::find(p.never_sites.begin(), p.never_sites.end(), site) != p.never_sites.end())
        return d;
    d.offer = true;
    return d;
}

std::shared_ptr<ProviderRequest> translation_call(const std::shared_ptr<Runtime>& rt,
                                                  const BuiltinProviderCall& builtin,
                                                  const std::string& operation,
                                                  const std::string& input_json,
                                                  ProviderHandlers handlers) {
    if (provider_is_app(*rt, Service::kTranslation))
        return provider_invoke(rt, Service::kTranslation, operation, input_json,
                               std::move(handlers));
    if (!builtin) /* no engine of its own: answered as a service with no provider is */
        return provider_invoke(rt, Service::kTranslation, operation, input_json,
                               std::move(handlers));
    /* The backend's own provider answers through the same contract. */
    ProviderHandlers checked = std::move(handlers);
    checked.done = [done = std::move(checked.done), operation,
                    input_json](const ProviderReply& reply) {
        if (!done)
            return;
        if (!reply.ok()) {
            done(reply);
            return;
        }
        ProviderReply answer = reply;
        std::string problem = provider_check_answer(Service::kTranslation, operation, input_json,
                                                    reply.result_json, &answer.result_json);
        if (!problem.empty())
            answer = reply_of(BDK_DOMAIN_PROVIDER, "contractViolation",
                              "the default translation provider's " + operation + ": " + problem);
        done(answer);
    };
    return builtin(operation, input_json, std::move(checked));
}

namespace {

class Job final : public TranslationJob, public std::enable_shared_from_this<Job> {
  public:
    Job(std::shared_ptr<Runtime> rt, TranslationJobSpec spec)
        : rt_(std::move(rt)), spec_(std::move(spec)), out_(spec_.texts) {}

    void Begin() {
        for (size_t i = 0; i < spec_.texts.size(); ++i) {
            if (!blank(spec_.texts[i]))
                pending_.push_back(i);
        }
        total_ = pending_.size();
        auto self = shared_from_this();
        Step("supported_pairs", "{}", [self](const ProviderReply& r) { self->OnPairs(r); });
    }

    void Cancel() override {
        if (finished_)
            return;
        cancelled_ = true;
        if (current_)
            current_->Cancel();
        else
            Finish(
                reply_of(BDK_DOMAIN_BROKER, BDK_CODE_CANCELLED, "the translation was cancelled"));
    }

  private:
    void Step(const std::string& operation, const std::string& input,
              const std::function<void(const ProviderReply&)>& next) {
        ProviderHandlers h;
        auto self = shared_from_this();
        if (operation == "prepare")
            h.progress = [self](double f) {
                if (self->spec_.progress)
                    self->spec_.progress(f * 0.1);
            };
        h.done = [self, next](const ProviderReply& r) {
            self->current_.reset();
            if (self->cancelled_ && r.ok()) {
                self->Finish(reply_of(BDK_DOMAIN_BROKER, BDK_CODE_CANCELLED,
                                      "the translation was cancelled"));
                return;
            }
            if (!r.ok()) {
                self->Finish(r);
                return;
            }
            next(r);
        };
        current_ = translation_call(rt_, spec_.builtin, operation, input, std::move(h));
    }

    void OnPairs(const ProviderReply& r) {
        JsonValue v;
        json_parse(r.result_json, &v);
        const JsonValue* pairs = v.find("pairs");
        bool found = false, installed = false;
        for (const JsonValue& p : pairs ? pairs->items : std::vector<JsonValue>{}) {
            if (same_language(p.find("from")->text, spec_.from) &&
                same_language(p.find("to")->text, spec_.to)) {
                found = true;
                installed = p.find("installed")->boolean;
                break;
            }
        }
        if (!found) {
            Finish(reply_of(BDK_DOMAIN_PROVIDER, "unsupportedPair",
                            "the provider does not translate " + spec_.from + " to " + spec_.to));
            return;
        }
        auto self = shared_from_this();
        if (!installed && provider_supports_prepare()) {
            Step("prepare", Pair(), [self](const ProviderReply&) { self->NextBatch(); });
            return;
        }
        NextBatch();
    }

    bool provider_supports_prepare() const {
        return !provider_is_app(*rt_, Service::kTranslation) ||
               provider_has_operation(*rt_, Service::kTranslation, "prepare");
    }

    std::string Pair() const {
        return "{\"from\":" + quoted(spec_.from) + ",\"to\":" + quoted(spec_.to) + "}";
    }

    void NextBatch() {
        if (pending_.empty()) {
            ProviderReply ok;
            Finish(ok);
            return;
        }
        batch_.clear();
        size_t bytes = 0;
        while (!pending_.empty() && batch_.size() < kTranslationBatchTexts) {
            const std::string& text = spec_.texts[pending_.front()];
            if (!batch_.empty() && bytes + text.size() > kTranslationBatchBytes)
                break;
            bytes += text.size();
            batch_.push_back(pending_.front());
            pending_.pop_front();
        }
        std::string texts = "[";
        for (size_t i = 0; i < batch_.size(); ++i)
            texts += (i ? "," : "") + quoted(spec_.texts[batch_[i]]);
        texts += ']';
        std::string input = "{\"from\":" + quoted(spec_.from) + ",\"to\":" + quoted(spec_.to) +
                            ",\"texts\":" + texts + ",\"url\":" + quoted(spec_.url) + "}";
        auto self = shared_from_this();
        Step("translate", input, [self](const ProviderReply& r) { self->OnBatch(r); });
    }

    void OnBatch(const ProviderReply& r) {
        JsonValue v;
        json_parse(r.result_json, &v);
        const JsonValue* texts = v.find("texts");
        for (size_t i = 0; i < batch_.size() && texts && i < texts->items.size(); ++i)
            out_[batch_[i]] = texts->items[i].text;
        done_ += batch_.size();
        if (spec_.progress && total_)
            spec_.progress(0.1 + 0.9 * static_cast<double>(done_) / static_cast<double>(total_));
        NextBatch();
    }

    void Finish(const ProviderReply& r) {
        if (finished_)
            return;
        finished_ = true;
        auto done = std::move(spec_.done);
        spec_.done = nullptr;
        if (done)
            done(r, r.ok() ? out_ : std::vector<std::string>{});
    }

    std::shared_ptr<Runtime> rt_;
    TranslationJobSpec spec_;
    std::vector<std::string> out_;
    std::deque<size_t> pending_;
    std::vector<size_t> batch_;
    size_t total_ = 0, done_ = 0;
    std::shared_ptr<ProviderRequest> current_;
    bool cancelled_ = false, finished_ = false;
};

} // namespace

std::shared_ptr<TranslationJob> translation_job_start(const std::shared_ptr<Runtime>& rt,
                                                      TranslationJobSpec spec) {
    auto job = std::make_shared<Job>(rt, std::move(spec));
    job->Begin();
    return job;
}

bool translation_parse_answer(const std::string& completion, const std::string& arguments,
                              TranslationAnswer* out, std::string* message) {
    JsonValue args;
    if (!arguments.empty() && (!valid_json(arguments) || !json_parse(arguments, &args))) {
        *message = "the arguments are not JSON";
        return false;
    }
    auto text = [&](const char* key) {
        const JsonValue* v = args.type == JsonValue::Type::kObject ? args.find(key) : nullptr;
        return v && v->type == JsonValue::Type::kString ? v->text : std::string();
    };
    using K = TranslationAnswer::Kind;
    static const std::pair<const char*, K> kKinds[] = {{"translate", K::kTranslate},
                                                       {"revert", K::kRevert},
                                                       {"always", K::kAlways},
                                                       {"never", K::kNever},
                                                       {"neverForSite", K::kNeverForSite},
                                                       {"dismiss", K::kDismiss}};
    bool known = false;
    for (const auto& [name, kind] : kKinds) {
        if (completion == name) {
            out->kind = kind;
            known = true;
        }
    }
    if (!known) {
        *message = "\"" + completion + "\" is not a translation completion";
        return false;
    }
    out->from = text("from");
    out->to = text("to");
    out->language = text("language");
    for (const std::string* code : {&out->from, &out->to, &out->language}) {
        if (!code->empty() && !translation_language_ok(*code)) {
            *message = "\"" + *code + "\" is not a language code";
            return false;
        }
    }
    return true;
}

std::string translation_request_payload(const TranslationState& s, const std::string& target,
                                        const std::string& pairs_json,
                                        const std::string& descriptor_json) {
    std::string phase = s.phase == "available" ? "offer" : s.phase;
    std::string out = "{\"phase\":" + quoted(phase) + ",\"target\":" + quoted(target);
    if (!s.detected.empty())
        out += ",\"detected\":" + quoted(s.detected);
    if (s.progress >= 0 && s.phase == "translating")
        out += ",\"progress\":" + std::to_string(s.progress);
    if (!s.error_code.empty())
        out += ",\"error\":{\"domain\":" + quoted(s.error_domain) +
               ",\"code\":" + quoted(s.error_code) + ",\"message\":" + quoted(s.error_message) +
               "}";
    return out + ",\"languages\":" + (pairs_json.empty() ? "[]" : pairs_json) +
           ",\"provider\":" + descriptor_json + "}";
}

} // namespace bdkcore
