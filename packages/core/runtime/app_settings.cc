/* Application settings: the `app.` keys of settings.get, getMany, describe, set and reset. The core
   answers them, so the stub and the engine behave the same; the backend only keeps the values
   (Backend::LoadAppSettings and SaveAppSettings). See services.md section 6 and swift-api.md 12.3.
 */
#include "broker_internal.h"

namespace bdkcore {
namespace {

bool is_app_key(const std::string& key) {
    return key.compare(0, 4, "app.") == 0;
}

const char* json_type_name(const JsonValue& v) {
    switch (v.type) {
    case JsonValue::Type::kBool:
        return "boolean";
    case JsonValue::Type::kNumber:
        return "number";
    case JsonValue::Type::kString:
        return "string";
    case JsonValue::Type::kArray:
        return "array";
    case JsonValue::Type::kObject:
        return "object";
    case JsonValue::Type::kNull:
        break;
    }
    return "json";
}

/* The keys a settings.* call names. */
std::vector<std::string> keys_of(const JsonValue& arguments) {
    std::vector<std::string> keys;
    if (const JsonValue* key = arguments.find("key"))
        keys.push_back(key->text);
    if (const JsonValue* list = arguments.find("keys")) {
        for (const JsonValue& k : list->items)
            keys.push_back(k.text);
    }
    return keys;
}

const char* source_name(CallerKind kind) {
    switch (kind) {
    case CallerKind::kPage:
        return "page";
    case CallerKind::kSession:
        return "session";
    case CallerKind::kHost:
        break;
    }
    return "application";
}

/* The profile a setting's values belong to: none for an application setting. */
const Profile* owner_of(const AppSetting& setting, const Profile* profile) {
    return setting.application ? nullptr : profile;
}

/* The values of the scope the setting lives in, loaded from the backend on first use. Null when the
   setting is profile scoped and the call has no profile. */
AppSettingValues* values_for(Runtime& rt, const AppSetting& setting, const Profile* profile) {
    if (!setting.application && !profile)
        return nullptr;
    const Profile* owner = owner_of(setting, profile);
    AppSettingValues& store = rt.app_values[owner ? owner->id : std::string()];
    if (!store.loaded) {
        store.loaded = true;
        std::string text =
            owner && owner->ephemeral ? std::string() : backend().LoadAppSettings(rt, owner);
        JsonValue parsed;
        if (!text.empty() && valid_json(text) && json_parse(text, &parsed) &&
            parsed.type == JsonValue::Type::kObject) {
            for (auto& member : parsed.members)
                store.values[member.first] = json_serialize(member.second);
        }
    }
    return &store;
}

void save(Runtime& rt, const AppSetting& setting, const Profile* profile,
          const AppSettingValues& store) {
    const Profile* owner = owner_of(setting, profile);
    if (owner && owner->ephemeral)
        return;
    std::string out = "{";
    for (const auto& [key, value] : store.values)
        out += (out.size() > 1 ? ",\"" : "\"") + json_escape(key) + "\":" + value;
    backend().SaveAppSettings(rt, owner, out + "}");
}

std::string current(const AppSetting& setting, const AppSettingValues& store) {
    auto it = store.values.find(setting.key);
    return it == store.values.end() ? setting.default_json : it->second;
}

/* An empty message when the value suits the setting's type. */
std::string type_problem(const AppSetting& setting, const JsonValue& value) {
    if (setting.type == "json" || setting.type == json_type_name(value))
        return std::string();
    return "'" + setting.key + "' takes a " + setting.type + " value";
}

const AppSetting* find(const Runtime& rt, const std::string& key) {
    auto it = rt.app_settings.find(key);
    return it == rt.app_settings.end() ? nullptr : &it->second;
}

} // namespace

bdk_status_t app_setting_declare(Runtime& rt, const std::string& declaration,
                                 bdk_error_t* out_error) {
    JsonValue parsed;
    if (!json_parse(declaration, &parsed) || parsed.type != JsonValue::Type::kObject)
        return BDK_ERR_INVALID_ARGUMENT;
    const JsonValue* key = parsed.find("key");
    const JsonValue* scope = parsed.find("scope");
    const JsonValue* exposed = parsed.find("exposedToPages");
    const JsonValue* def = parsed.find("default");
    if (!key || key->type != JsonValue::Type::kString ||
        (scope && (scope->type != JsonValue::Type::kString ||
                   (scope->text != "profile" && scope->text != "application"))) ||
        (exposed && exposed->type != JsonValue::Type::kBool))
        return BDK_ERR_INVALID_ARGUMENT;
    if (key->text.size() <= 4 || !is_app_key(key->text))
        return fail(out_error, BDK_DOMAIN_CONFIG, "nameNotApp",
                    "application setting keys start with 'app.'");
    if (rt.app_settings.count(key->text))
        return fail(out_error, BDK_DOMAIN_CONFIG, "duplicateName",
                    "'" + key->text + "' is already registered");
    AppSetting setting;
    setting.key = key->text;
    setting.type = def ? json_type_name(*def) : "json";
    setting.default_json = def ? json_serialize(*def) : "null";
    setting.application = scope && scope->text == "application";
    setting.exposed = exposed && exposed->boolean;
    rt.app_settings[setting.key] = setting;
    return BDK_OK;
}

bool app_settings_exposed(const Runtime& rt, const JsonValue& arguments, std::string* message) {
    for (const std::string& key : keys_of(arguments)) {
        const AppSetting* setting = find(rt, key);
        if (setting && !setting->exposed) {
            *message = "'" + key + "' is not exposed to pages";
            return false;
        }
    }
    return true;
}

bool app_settings_execute(Runtime& rt, const std::shared_ptr<BuiltinCall>& call,
                          const JsonValue& arguments) {
    const std::vector<std::string> keys = keys_of(arguments);
    size_t app_keys = 0;
    for (const std::string& key : keys)
        app_keys += is_app_key(key) ? 1 : 0;
    if (app_keys == 0)
        return false;
    CommandControl& control = *call->control;
    if (app_keys != keys.size()) {
        control.Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                       "application settings and browser settings cannot be read in one call");
        return true;
    }
    const Profile* profile = call->profile.get();

    if (call->name == "settings.getMany") {
        std::string out = "{\"values\":{";
        bool first = true;
        for (const std::string& key : keys) {
            std::string value = "null";
            if (const AppSetting* setting = find(rt, key)) {
                AppSettingValues* store = values_for(rt, *setting, profile);
                if (!store) {
                    control.Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                                   "'" + key + "' needs a profile");
                    return true;
                }
                value = current(*setting, *store);
            }
            out += (first ? "\"" : ",\"") + json_escape(key) + "\":" + value;
            first = false;
        }
        control.Resolve(out + "}}");
        return true;
    }

    const std::string& key = keys[0];
    const AppSetting* setting = find(rt, key);
    if (!setting) {
        if (call->name == "settings.get")
            control.Resolve("{\"value\":null}"); /* like a key the catalogue does not list */
        else
            control.Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                           "unknown setting '" + key + "'");
        return true;
    }
    AppSettingValues* store = values_for(rt, *setting, profile);
    if (!store) {
        control.Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                       "'" + key + "' needs a profile");
        return true;
    }
    const std::string before = current(*setting, *store);

    if (call->name == "settings.get") {
        control.Resolve("{\"value\":" + before + "}");
    } else if (call->name == "settings.describe") {
        const bool stored = store->values.count(key) != 0;
        control.Resolve("{\"key\":\"" + json_escape(key) + "\",\"type\":\"" + setting->type +
                        "\",\"default\":" + setting->default_json + ",\"value\":" + before +
                        ",\"scope\":\"" + (setting->application ? "application" : "profile") +
                        "\",\"source\":\"" + (stored ? "user" : "product") +
                        "\",\"locked\":false}");
    } else {
        if (call->name == "settings.set") {
            const JsonValue* value = arguments.find("value");
            std::string problem = value ? type_problem(*setting, *value) : "a value is needed";
            if (!problem.empty()) {
                control.Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS, problem);
                return true;
            }
            store->values[key] = json_serialize(*value);
        } else { /* settings.reset */
            store->values.erase(key);
        }
        save(rt, *setting, profile, *store);
        control.Resolve("null");
        const std::string after = current(*setting, *store);
        if (after != before) {
            const Profile* owner = owner_of(*setting, profile);
            emit_builtin_event(current_runtime(), "settings.changed", owner ? owner->handle : 0,
                               "{\"key\":\"" + json_escape(key) + "\",\"oldValue\":" + before +
                                   ",\"newValue\":" + after + ",\"source\":\"" +
                                   source_name(call->caller->kind) + "\"}");
        }
    }
    return true;
}

void app_settings_forget(Runtime& rt, const std::string& profile_id) {
    rt.app_values.erase(profile_id);
}

} // namespace bdkcore
