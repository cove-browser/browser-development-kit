/* Providers: configuration, registration, calls and the contract (providers_internal.h). */
#include <algorithm>
#include <cmath>
#include <cstring>

#include "providers_internal.h"

namespace bdkcore {

/* What the runtime keeps of providers. */
struct ProviderCall;

struct ProviderTable {
    ProviderChoice choices[kServiceCount];
    std::string google_key;
    struct Registered {
        bool present = false;
        bdk_provider_vtable_t vtable{};
        void* ctx = nullptr;
        std::string descriptor; /* normalised JSON */
    } registered[kServiceCount];
    std::vector<std::weak_ptr<ProviderCall>> calls;
    std::map<bdk_contents_t, TranslationState> translation_states;
    std::unique_ptr<ReputationList> reputation_list;
};

namespace {

const char* const kServiceNames[kServiceCount] = {"translation", "reputation", "geolocation"};

ProviderTable& table_of(Runtime& rt) {
    if (!rt.provider_table)
        rt.provider_table = std::make_shared<ProviderTable>();
    return *rt.provider_table;
}

const ProviderTable& table_of(const Runtime& rt) {
    static const ProviderTable empty;
    return rt.provider_table ? *rt.provider_table : empty;
}

int index_of(Service s) {
    return static_cast<int>(s);
}

/* The provider names each service accepts in this runtime, the ones BDK names but has not built
   (T-PRV-08, after v1), and the default. */
struct ServiceRules {
    const char* defaults;
    std::vector<std::string> built;
    std::vector<std::string> later;
};

const ServiceRules& rules_of(Service s) {
    static const ServiceRules kRules[kServiceCount] = {
        {"default", {"default", "app"}, {"apple", "deepl", "microsoft", "google"}},
        {"default", {"default", "app", "google"}, {"web-risk"}},
        {"system", {"system", "app", "google"}, {}},
    };
    return kRules[index_of(s)];
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

/* An endpoint BDK may send a provider's requests to: https, or http on this machine. */
bool endpoint_ok(const std::string& url) {
    if (url.compare(0, 8, "https://") == 0)
        return url.size() > 8;
    for (const char* local : {"http://127.0.0.1", "http://localhost", "http://[::1]"}) {
        size_t n = strlen(local);
        if (url.compare(0, n, local) == 0 && (url.size() == n || url[n] == ':' || url[n] == '/'))
            return true;
    }
    return false;
}

std::string quoted(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

const JsonValue* member(const JsonValue& object, const char* key) {
    return object.type == JsonValue::Type::kObject ? object.find(key) : nullptr;
}

bool is_string(const JsonValue* v) {
    return v && v->type == JsonValue::Type::kString;
}

bool is_number(const JsonValue* v) {
    return v && v->type == JsonValue::Type::kNumber && std::isfinite(v->number);
}

/* Checks a descriptor and writes it in its normal form. */
std::string check_descriptor(const std::string& text, std::string* normalized) {
    JsonValue d;
    if (!valid_json(text) || !json_parse(text, &d) || d.type != JsonValue::Type::kObject)
        return "the descriptor is not a JSON object";
    for (const auto& m : d.members) {
        static const char* const kKeys[] = {"id",   "name",      "privacy",
                                            "host", "policyUrl", "protection"};
        bool known = false;
        for (const char* k : kKeys)
            known = known || m.first == k;
        if (!known)
            return "the descriptor has an unknown field \"" + m.first + "\"";
        if (m.second.type != JsonValue::Type::kString)
            return "the descriptor's \"" + m.first + "\" is not a string";
    }
    const JsonValue* id = d.find("id");
    const JsonValue* name = d.find("name");
    const JsonValue* privacy = d.find("privacy");
    if (!is_string(id) || id->text.empty() || !is_string(name) || name->text.empty())
        return "the descriptor needs an \"id\" and a \"name\"";
    if (!is_string(privacy) || (privacy->text != "onDevice" && privacy->text != "remote"))
        return "the descriptor's \"privacy\" is not \"onDevice\" or \"remote\"";
    const JsonValue* host = d.find("host");
    const JsonValue* policy = d.find("policyUrl");
    if (privacy->text == "remote") {
        if (!is_string(host) || host->text.empty())
            return "a remote provider's descriptor names the host page content is sent to";
        if (!is_string(policy) || policy->text.compare(0, 8, "https://") != 0 ||
            policy->text.size() <= 8)
            return "a remote provider's descriptor needs an https \"policyUrl\"";
    }
    *normalized = json_serialize(d);
    return "";
}

std::string builtin_descriptor(Service s, const std::string& provider) {
    switch (s) {
    case Service::kTranslation:
        return R"({"id":"default","name":"On-device translation","privacy":"onDevice"})";
    case Service::kReputation:
        if (provider == "google")
            return R"({"id":"google","name":"Google Safe Browsing","privacy":"remote",)"
                   R"("host":"safebrowsing.googleapis.com",)"
                   R"("policyUrl":"https://policies.google.com/privacy","protection":"standard"})";
        return R"({"id":"default","name":"Block lists","privacy":"onDevice",)"
               R"("protection":"reduced"})";
    case Service::kGeolocation:
        if (provider == "google")
            return R"({"id":"google","name":"Google Geolocation API","privacy":"remote",)"
                   R"("host":"www.googleapis.com",)"
                   R"("policyUrl":"https://policies.google.com/privacy"})";
        return R"({"id":"system","name":"Operating system location","privacy":"onDevice"})";
    }
    return "null";
}

/* The required operations of each service, and every operation it has. */
struct Operation {
    const char* name;
    bdk_provider_op_cb bdk_provider_vtable_t::* slot;
    Service service;
    bool required;
};

const Operation kOperations[] = {
    {"supported_pairs", &bdk_provider_vtable_t::supported_pairs, Service::kTranslation, true},
    {"prepare", &bdk_provider_vtable_t::prepare, Service::kTranslation, false},
    {"translate", &bdk_provider_vtable_t::translate, Service::kTranslation, true},
    {"detect", &bdk_provider_vtable_t::detect, Service::kTranslation, false},
    {"remove_model", &bdk_provider_vtable_t::remove_model, Service::kTranslation, false},
    {"check_url", &bdk_provider_vtable_t::check_url, Service::kReputation, true},
    {"check_download", &bdk_provider_vtable_t::check_download, Service::kReputation, false},
    {"update", &bdk_provider_vtable_t::update, Service::kReputation, false},
    {"start", &bdk_provider_vtable_t::start, Service::kGeolocation, true},
};

const Operation* find_operation(Service s, const std::string& name) {
    for (const Operation& op : kOperations) {
        if (op.service == s && name == op.name)
            return &op;
    }
    return nullptr;
}

ProviderReply error_reply(const char* domain, const char* code, const std::string& message) {
    ProviderReply r;
    r.domain = domain;
    r.code = code;
    r.message = message;
    return r;
}

} // namespace

std::map<bdk_contents_t, TranslationState>& translation_states(Runtime& rt) {
    return table_of(rt).translation_states;
}

bool reputation_set_list(Runtime& rt, ReputationList list, std::string* message) {
    ProviderTable& t = table_of(rt);
    if (t.reputation_list && list.sequence <= t.reputation_list->sequence) {
        *message = "the list's sequence " + std::to_string(list.sequence) +
                   " is not newer than the current " + std::to_string(t.reputation_list->sequence);
        return false;
    }
    t.reputation_list = std::make_unique<ReputationList>(std::move(list));
    return true;
}

const ReputationList* reputation_current_list(const Runtime& rt) {
    return table_of(rt).reputation_list.get();
}

const char* service_name(Service s) {
    return kServiceNames[index_of(s)];
}

bool service_from_name(const std::string& name, Service* out) {
    for (int i = 0; i < kServiceCount; ++i) {
        if (name == kServiceNames[i]) {
            *out = static_cast<Service>(i);
            return true;
        }
    }
    return false;
}

bdk_status_t providers_configure(Runtime& rt, const std::string& resolved,
                                 const bdk_start_config_t& config, bdk_error_t* out_error) {
    ProviderTable& t = table_of(rt);
    for (int i = 0; i < kServiceCount; ++i)
        t.choices[i].provider = rules_of(static_cast<Service>(i)).defaults;
    JsonValue root;
    if (!json_parse(resolved, &root) || root.type != JsonValue::Type::kObject)
        return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                    "the resolved configuration is not a JSON object");
    bool uses_google = false;
    if (const JsonValue* services = root.find("services")) {
        if (services->type != JsonValue::Type::kObject)
            return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                        "\"services\" in the resolved configuration is not an object");
        for (const auto& [name, table] : services->members) {
            if (table.type != JsonValue::Type::kObject)
                return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                            "services." + name + " in the resolved configuration is not an object");
            const JsonValue* provider = table.find("provider");
            if (provider && provider->type != JsonValue::Type::kString)
                return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                            "services." + name + ".provider is not a string");
            Service s;
            if (!service_from_name(name, &s)) {
                /* credentials (S14) and leak_check (S16) are proposals: only their defaults run. */
                static const std::map<std::string, std::string> kFixed = {
                    {"credentials", "chromium"}, {"leak_check", "none"}};
                auto fixed = kFixed.find(name);
                if (fixed == kFixed.end())
                    continue; /* a table this runtime does not read, such as components */
                if (provider && provider->text != fixed->second)
                    return fail(out_error, BDK_DOMAIN_CONFIG, "providerUnavailable",
                                "services." + name + ".provider \"" + provider->text +
                                    "\" is not in this runtime; only \"" + fixed->second + "\" is");
                continue;
            }
            ProviderChoice& choice = t.choices[index_of(s)];
            const ServiceRules& rules = rules_of(s);
            if (provider) {
                if (contains(rules.later, provider->text))
                    return fail(out_error, BDK_DOMAIN_CONFIG, "providerUnavailable",
                                "services." + name + ".provider \"" + provider->text +
                                    "\" is not in this runtime yet (T-PRV-08)");
                if (!contains(rules.built, provider->text))
                    return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                                "services." + name + ".provider \"" + provider->text +
                                    "\" is not a provider BDK knows");
                choice.provider = provider->text;
            }
            if (const JsonValue* endpoint = table.find("endpoint")) {
                if (endpoint->type != JsonValue::Type::kString || !endpoint_ok(endpoint->text))
                    return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                                "services." + name + ".endpoint is not an https URL");
                /* Only the default reputation provider's protocol (BDK's lists) can be sent
                   elsewhere without patching Chromium: Safe Browsing's and the network location
                   provider's hosts are constants (T-PRV-10). */
                if (!(s == Service::kReputation && choice.provider == "default"))
                    return fail(out_error, BDK_DOMAIN_CONFIG, "endpointUnsupported",
                                "services." + name + ".endpoint cannot be used with provider \"" +
                                    choice.provider + "\" in this runtime");
                choice.endpoint = endpoint->text;
            }
            if (const JsonValue* interval = table.find("update_interval")) {
                if (interval->type != JsonValue::Type::kString)
                    return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                                "services." + name + ".update_interval is not a string");
                choice.update_interval = interval->text;
            }
            uses_google = uses_google || choice.provider == "google";
        }
    }
    if (!string_ok(config.google_api_key))
        return BDK_ERR_INVALID_ARGUMENT;
    std::string key = str(config.google_api_key);
    if (uses_google && key.empty())
        return fail(out_error, BDK_DOMAIN_CONFIG, "keyMissing",
                    "a service uses provider \"google\" and no Google API key was given");
    if (!uses_google && !key.empty())
        return fail(out_error, BDK_DOMAIN_CONFIG, "keyUnused",
                    "a Google API key was given but no service uses provider \"google\"; BDK "
                    "passes the key to Chromium only for the services configured that way");
    t.google_key = key;
    return BDK_OK;
}

bdk_status_t providers_check_registrations(const Runtime& rt, bdk_error_t* out_error) {
    std::string missing;
    const ProviderTable& t = table_of(rt);
    for (int i = 0; i < kServiceCount; ++i) {
        if (t.choices[i].provider == "app" && !t.registered[i].present)
            missing += (missing.empty() ? "" : ", ") + std::string(kServiceNames[i]);
    }
    if (missing.empty())
        return BDK_OK;
    return fail(out_error, BDK_DOMAIN_CONFIG, "providerMissing",
                "services configured with provider = \"app\" need a provider before the freeze: " +
                    missing);
}

const ProviderChoice& provider_choice(const Runtime& rt, Service s) {
    static const ProviderChoice kDefaults[kServiceCount] = {
        {"default", "", "1h"}, {"default", "", "1h"}, {"system", "", "1h"}};
    if (!rt.provider_table)
        return kDefaults[index_of(s)];
    return rt.provider_table->choices[index_of(s)];
}

bool provider_is_app(const Runtime& rt, Service s) {
    return provider_choice(rt, s).provider == "app";
}

const std::string& provider_google_key(const Runtime& rt) {
    return table_of(rt).google_key;
}

std::string provider_descriptor(const Runtime& rt, Service s) {
    const ProviderTable& t = table_of(rt);
    if (provider_is_app(rt, s) && t.registered[index_of(s)].present)
        return t.registered[index_of(s)].descriptor;
    return builtin_descriptor(s, provider_choice(rt, s).provider);
}

bool provider_has_operation(const Runtime& rt, Service s, const std::string& operation) {
    const Operation* op = find_operation(s, operation);
    const ProviderTable& t = table_of(rt);
    const auto& reg = t.registered[index_of(s)];
    return op && reg.present && reg.vtable.*(op->slot) != nullptr;
}

bdk_status_t provider_register(Runtime& rt, const std::string& service,
                               const bdk_provider_vtable_t& vtable, void* ctx, bdk_release_cb rel,
                               bdk_error_t* out_error) {
    Service s;
    if (!service_from_name(service, &s))
        return BDK_ERR_INVALID_ARGUMENT;
    /* Read only the bytes the caller declared. */
    bdk_provider_vtable_t vt;
    memset(&vt, 0, sizeof vt);
    memcpy(&vt, &vtable, vtable.struct_size < sizeof vt ? vtable.struct_size : sizeof vt);
    for (const Operation& op : kOperations) {
        if (op.service == s && op.required && !(vt.*(op.slot)))
            return BDK_ERR_INVALID_ARGUMENT;
    }
    if (!string_ok(vt.descriptor_json))
        return BDK_ERR_INVALID_ARGUMENT;
    ProviderTable& t = table_of(rt);
    auto& reg = t.registered[index_of(s)];
    if (reg.present)
        return fail(out_error, BDK_DOMAIN_CONFIG, "duplicateName",
                    "a " + service + " provider is already registered");
    if (t.choices[index_of(s)].provider != "app")
        return fail(out_error, BDK_DOMAIN_CONFIG, "providerNotApp",
                    "services." + service + " uses provider \"" + t.choices[index_of(s)].provider +
                        "\"; register a provider only for provider = \"app\"");
    std::string descriptor;
    std::string problem = check_descriptor(str(vt.descriptor_json), &descriptor);
    if (!problem.empty())
        return fail(out_error, BDK_DOMAIN_CONFIG, "descriptorInvalid", problem);
    vt.descriptor_json = bdk_string_t{nullptr, 0}; /* borrowed: not kept */
    reg.present = true;
    reg.vtable = vt;
    reg.ctx = ctx;
    reg.descriptor = descriptor;
    rt.providers[service] = Owned{ctx, rel};
    return BDK_OK;
}

/* ---- Calls ---- */

struct ProviderCall final : ProviderRequest, std::enable_shared_from_this<ProviderCall> {
    std::weak_ptr<Runtime> rt;
    Service service = Service::kTranslation;
    std::string operation, input;
    ProviderHandlers handlers;
    bdk_context_t handle = 0;
    bool finished = false;
    /* Reports sent before the answer are still delivered; none after a cancel. */
    bool cancelled = false;

    void Finish(const ProviderReply& reply) {
        if (finished)
            return;
        finished = true;
        table_free(handle);
        auto done = std::move(handlers.done);
        handlers = ProviderHandlers{};
        if (done)
            post([done, reply] { done(reply); });
    }

    void Cancel() override {
        if (finished)
            return;
        auto runtime = rt.lock();
        bdk_provider_op_cb cancel = nullptr;
        void* ctx = nullptr;
        if (runtime) {
            const auto& reg = table_of(*runtime).registered[index_of(service)];
            cancel = reg.vtable.cancel;
            ctx = reg.ctx;
        }
        finished = true;
        cancelled = true;
        auto done = std::move(handlers.done);
        handlers = ProviderHandlers{};
        auto self = shared_from_this();
        /* The provider learns of it from a task of its own, then the handle goes stale. */
        post([self, cancel, ctx] {
            if (cancel)
                cancel(ctx, self->handle, view(self->input));
            table_free(self->handle);
        });
        if (done)
            post([done] {
                done(error_reply(BDK_DOMAIN_BROKER, BDK_CODE_CANCELLED, "the call was cancelled"));
            });
    }
};

std::shared_ptr<ProviderRequest> provider_invoke(const std::shared_ptr<Runtime>& rt, Service s,
                                                 const std::string& operation,
                                                 const std::string& input_json,
                                                 ProviderHandlers handlers) {
    const Operation* op = find_operation(s, operation);
    ProviderTable& t = table_of(*rt);
    const auto& reg = t.registered[index_of(s)];
    if (!op || !reg.present || !(reg.vtable.*(op->slot))) {
        auto done = std::move(handlers.done);
        if (done)
            post([done, s, operation] {
                done(error_reply(BDK_DOMAIN_PROVIDER, "unavailable",
                                 std::string("the ") + service_name(s) +
                                     " provider has no operation " + operation));
            });
        return nullptr;
    }
    auto call = std::make_shared<ProviderCall>();
    call->rt = rt;
    call->service = s;
    call->operation = operation;
    call->input = input_json;
    call->handlers = std::move(handlers);
    call->handle = table_make(T_PROVIDER_CALL, call);
    t.calls.push_back(call);
    std::erase_if(t.calls, [](const std::weak_ptr<ProviderCall>& w) { return w.expired(); });
    bdk_provider_op_cb fn = reg.vtable.*(op->slot);
    void* ctx = reg.ctx;
    post([call, fn, ctx] {
        if (!call->finished)
            fn(ctx, call->handle, view(call->input));
    });
    return call;
}

void providers_shutdown(Runtime& rt) {
    if (!rt.provider_table)
        return;
    for (auto& w : rt.provider_table->calls) {
        if (auto c = w.lock()) {
            /* The runtime is going: nobody waits for done, but the provider is told. */
            c->handlers.done = nullptr;
            c->Cancel();
        }
    }
    rt.provider_table->calls.clear();
}

/* ---- The contract ---- */

namespace {

std::string check_verdict(const JsonValue& answer, bool download, std::string* normalized) {
    const JsonValue* verdict = member(answer, "verdict");
    if (!is_string(verdict) || answer.members.size() != 1)
        return "the answer is not {\"verdict\": ...}";
    static const char* const kUrl[] = {"safe", "unknown", "malware", "phishing",
                                       "unwantedSoftware"};
    static const char* const kDownload[] = {"safe", "unknown", "malware", "unwantedSoftware",
                                            "dangerousDownload"};
    for (const char* v : download ? kDownload : kUrl) {
        if (verdict->text == v) {
            *normalized = "{\"verdict\":" + quoted(v) + "}";
            return "";
        }
    }
    return "\"" + verdict->text + "\" is not a verdict for " + (download ? "a download" : "a URL");
}

std::string check_position(const JsonValue& p, std::string* normalized) {
    if (p.type != JsonValue::Type::kObject)
        return "a report is not an object";
    if (const JsonValue* error = p.find("error")) {
        if (!is_string(error) || (error->text != "permissionDenied" &&
                                  error->text != "positionUnavailable" && error->text != "timeout"))
            return "a position error is not permissionDenied, positionUnavailable or timeout";
        const JsonValue* message = p.find("message");
        *normalized = "{\"error\":" + quoted(error->text) +
                      ",\"message\":" + quoted(is_string(message) ? message->text : "") + "}";
        return "";
    }
    const JsonValue* lat = p.find("latitude");
    const JsonValue* lng = p.find("longitude");
    const JsonValue* acc = p.find("accuracy");
    if (!is_number(lat) || !is_number(lng) || !is_number(acc))
        return "a position needs numbers latitude, longitude and accuracy";
    if (lat->number < -90 || lat->number > 90 || lng->number < -180 || lng->number > 180 ||
        acc->number < 0)
        return "a position is out of range";
    for (const auto& m : p.members) {
        static const char* const kKnown[] = {"latitude", "longitude",        "accuracy",
                                             "altitude", "altitudeAccuracy", "heading",
                                             "speed",    "timestamp"};
        bool known = false;
        for (const char* k : kKnown)
            known = known || m.first == k;
        if (!known)
            return "a position has an unknown field \"" + m.first + "\"";
        if (!is_number(&m.second))
            return "a position's \"" + m.first + "\" is not a number";
    }
    auto nonneg = [&](const char* k) {
        const JsonValue* v = p.find(k);
        return !v || v->number >= 0;
    };
    const JsonValue* heading = p.find("heading");
    if (!nonneg("altitudeAccuracy") || !nonneg("speed") ||
        (heading && (heading->number < 0 || heading->number >= 360)))
        return "a position is out of range";
    *normalized = json_serialize(p);
    return "";
}

} // namespace

std::string provider_check_answer(Service s, const std::string& operation,
                                  const std::string& input_json, const std::string& answer_json,
                                  std::string* normalized) {
    JsonValue answer;
    if (!valid_json(answer_json) || !json_parse(answer_json, &answer))
        return "the answer is not JSON";
    *normalized = "null";
    if (s == Service::kTranslation) {
        if (operation == "supported_pairs") {
            const JsonValue* pairs = member(answer, "pairs");
            if (!pairs || pairs->type != JsonValue::Type::kArray)
                return "the answer is not {\"pairs\": [...]}";
            std::string out = "{\"pairs\":[";
            for (size_t i = 0; i < pairs->items.size(); ++i) {
                const JsonValue& pair = pairs->items[i];
                const JsonValue* from = member(pair, "from");
                const JsonValue* to = member(pair, "to");
                const JsonValue* installed = member(pair, "installed");
                if (!is_string(from) || !is_string(to) || !translation_language_ok(from->text) ||
                    !translation_language_ok(to->text) || from->text == to->text)
                    return "pair " + std::to_string(i) + " does not name two languages";
                if (installed && installed->type != JsonValue::Type::kBool)
                    return "pair " + std::to_string(i) + ": \"installed\" is not a boolean";
                out += std::string(i ? "," : "") + "{\"from\":" + quoted(from->text) +
                       ",\"to\":" + quoted(to->text) +
                       ",\"installed\":" + ((!installed || installed->boolean) ? "true" : "false") +
                       "}";
            }
            *normalized = out + "]}";
            return "";
        }
        if (operation == "translate") {
            JsonValue input;
            json_parse(input_json, &input);
            const JsonValue* in = member(input, "texts");
            const JsonValue* texts = member(answer, "texts");
            if (!texts || texts->type != JsonValue::Type::kArray)
                return "the answer is not {\"texts\": [...]}";
            if (in && texts->items.size() != in->items.size())
                return "the answer has " + std::to_string(texts->items.size()) + " texts for " +
                       std::to_string(in->items.size());
            std::string out = "{\"texts\":[";
            for (size_t i = 0; i < texts->items.size(); ++i) {
                if (texts->items[i].type != JsonValue::Type::kString)
                    return "text " + std::to_string(i) + " is not a string";
                out += std::string(i ? "," : "") + quoted(texts->items[i].text);
            }
            *normalized = out + "]}";
            return "";
        }
        if (operation == "detect") {
            if (answer.type == JsonValue::Type::kNull)
                return "";
            const JsonValue* language = member(answer, "language");
            const JsonValue* reliable = member(answer, "reliable");
            if (!is_string(language) || !translation_language_ok(language->text) || !reliable ||
                reliable->type != JsonValue::Type::kBool)
                return "the answer is not null or {\"language\", \"reliable\"}";
            *normalized = "{\"language\":" + quoted(language->text) +
                          ",\"reliable\":" + (reliable->boolean ? "true" : "false") + "}";
            return "";
        }
        return ""; /* prepare, remove_model: the answer carries nothing */
    }
    if (s == Service::kReputation) {
        if (operation == "check_url" || operation == "check_download")
            return check_verdict(answer, operation == "check_download", normalized);
        return ""; /* update */
    }
    return ""; /* start: its answer ends the watch; the positions are reports */
}

} // namespace bdkcore

/* ---- bdk_provider.h: answers ---- */

using namespace bdkcore;

namespace {

std::shared_ptr<ProviderCall> live_call(bdk_context_t h) {
    if (check_thread() != BDK_OK)
        return nullptr;
    auto c = get<ProviderCall>(h, T_PROVIDER_CALL);
    return c && !c->finished ? c : nullptr;
}

} // namespace

extern "C" {

BDK_EXPORT void bdk_provider_resolve(bdk_context_t h, bdk_string_t output) {
    auto c = live_call(h);
    if (!c)
        return;
    std::string normalized;
    std::string problem =
        string_ok(output)
            ? provider_check_answer(c->service, c->operation, c->input, str(output), &normalized)
            : "the answer is not a string";
    ProviderReply reply;
    if (problem.empty()) {
        reply.result_json = normalized;
    } else {
        reply = error_reply(BDK_DOMAIN_PROVIDER, "contractViolation",
                            std::string("the ") + service_name(c->service) + " provider's " +
                                c->operation + " broke its contract: " + problem);
    }
    c->Finish(reply);
}

BDK_EXPORT void bdk_provider_reject(bdk_context_t h, bdk_string_t domain, bdk_string_t code,
                                    bdk_string_t message) {
    auto c = live_call(h);
    if (!c)
        return;
    std::string d = string_ok(domain) ? str(domain) : "";
    std::string k = string_ok(code) ? str(code) : "";
    std::string m = string_ok(message) ? str(message) : "";
    /* A provider speaks in its own domain: anything else is reported as a provider failure. */
    if (d != BDK_DOMAIN_PROVIDER || k.empty()) {
        m = (d.empty() ? "" : d + "." + k + ": ") + m;
        k = "remoteError";
    }
    ProviderReply reply = error_reply(BDK_DOMAIN_PROVIDER, k.c_str(), m);
    c->Finish(reply);
}

BDK_EXPORT void bdk_provider_progress(bdk_context_t h, double fraction) {
    auto c = live_call(h);
    if (!c || c->operation != "prepare" || !c->handlers.progress || !std::isfinite(fraction))
        return;
    double f = fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);
    auto progress = c->handlers.progress;
    post([c, progress, f] {
        if (!c->cancelled)
            progress(f);
    });
}

BDK_EXPORT void bdk_provider_report(bdk_context_t h, bdk_string_t json) {
    auto c = live_call(h);
    if (!c || c->operation != "start" || !c->handlers.report || !string_ok(json))
        return;
    JsonValue p;
    std::string text = str(json);
    std::string normalized;
    if (!valid_json(text) || !json_parse(text, &p) || !check_position(p, &normalized).empty()) {
        /* A malformed position ends the watch: the page must not be given a wrong place. */
        c->Finish(error_reply(BDK_DOMAIN_PROVIDER, "contractViolation",
                              "the geolocation provider reported a malformed position"));
        return;
    }
    auto report = c->handlers.report;
    post([c, report, normalized] {
        if (!c->cancelled)
            report(normalized);
    });
}

BDK_EXPORT uint8_t bdk_provider_is_cancelled(bdk_context_t h) {
    if (check_thread() != BDK_OK)
        return 1;
    auto c = get<ProviderCall>(h, T_PROVIDER_CALL);
    return !c || c->finished ? 1 : 0;
}

} /* extern "C" */
