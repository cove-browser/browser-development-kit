/* Installable web apps (web-apps.md, T-PWA-02, T-PWA-03): what the core decides for every runtime.
   It reads [web_apps] and [services.push] at start, reports them in runtime.info, refuses every
   webApps.* command while web apps are off and the ones a private profile cannot run, and offers
   the host its confirmed install and uninstall (bdk_web_apps.h). The registry itself,
   installability and launching are the backend's. Platform-neutral C++23. */
#include "broker_internal.h"
#include "core_internal.h"

namespace bdkcore {
namespace {

bdk_status_t resolved_invalid(bdk_error_t* err, const std::string& what) {
    return fail(err, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                what + " in the resolved configuration");
}

} // namespace

bdk_status_t web_apps_configure(Runtime& rt, const std::string& resolved, bdk_error_t* err) {
    rt.web_apps = WebAppConfig{};
    JsonValue root;
    if (!json_parse(resolved, &root) || root.type != JsonValue::Type::kObject)
        return resolved_invalid(err, "the configuration is not a JSON object; it is invalid");
    if (const JsonValue* web_apps = root.find("web_apps")) {
        if (web_apps->type != JsonValue::Type::kObject)
            return resolved_invalid(err, "\"web_apps\" is not an object");
        if (const JsonValue* enabled = web_apps->find("enabled")) {
            if (enabled->type != JsonValue::Type::kBool)
                return resolved_invalid(err, "\"web_apps.enabled\" is not a boolean");
            rt.web_apps.enabled = enabled->boolean;
        }
    }
    const JsonValue* services = root.find("services");
    const JsonValue* push =
        services && services->type == JsonValue::Type::kObject ? services->find("push") : nullptr;
    if (!push)
        return BDK_OK;
    if (push->type != JsonValue::Type::kObject)
        return resolved_invalid(err, "\"services.push\" is not an object");
    if (const JsonValue* provider = push->find("provider")) {
        if (provider->type != JsonValue::Type::kString ||
            (provider->text != "none" && provider->text != "google"))
            return resolved_invalid(err, "\"services.push.provider\" is not none or google");
        rt.web_apps.push_provider = provider->text;
    }
    return BDK_OK;
}

std::string web_apps_runtime_info(const Runtime& rt) {
    return std::string("\"webApps\":{\"enabled\":") + (rt.web_apps.enabled ? "true" : "false") +
           "},\"push\":{\"provider\":\"" + rt.web_apps.push_provider + "\"}";
}

bool web_apps_refuse(const Runtime& rt, const std::shared_ptr<BuiltinCall>& call) {
    if (call->name.rfind("webApps.", 0) != 0)
        return false;
    CommandControl& control = *call->control;
    if (!rt.web_apps.enabled) {
        control.Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_DISABLED,
                       "web apps are off ([web_apps] enabled = false in bdk.toml)");
        return true;
    }
    if (!call->profile || !call->profile->ephemeral)
        return false;
    /* A private profile has no apps of its own and never sees its parent's (web-apps.md 5). */
    const std::string& name = call->name;
    if (name == "webApps.list") {
        control.Resolve("[]");
    } else if (name == "webApps.installability") {
        control.Resolve("{\"isInstallable\":false,\"installedId\":null,\"manifest\":null,"
                        "\"errors\":[\"a private profile cannot install web apps\"]}");
    } else if (name == "webApps.get") {
        control.Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_NOT_FOUND,
                       "a private profile has no web apps");
    } else {
        control.Reject(BDK_DOMAIN_WEB_APPS, BDK_CODE_PRIVATE_PROFILE,
                       "'" + name + "' is not available in a private profile");
    }
    return true;
}

} // namespace bdkcore

using namespace bdkcore;

/* ---- bdk_web_apps.h ---- */

BDK_EXPORT bdk_call_t bdk_web_app_install(bdk_runtime_t h, bdk_string_t args,
                                          const bdk_invoke_options_t* options,
                                          bdk_native_handle_t gesture, bdk_result_cb cb, void* ctx,
                                          bdk_release_cb rel) {
    return broker_invoke_confirmed(h, "webApps.install", args, options, gesture, cb, ctx, rel);
}

BDK_EXPORT bdk_call_t bdk_web_app_uninstall(bdk_runtime_t h, bdk_string_t args,
                                            const bdk_invoke_options_t* options,
                                            bdk_native_handle_t gesture, bdk_result_cb cb,
                                            void* ctx, bdk_release_cb rel) {
    return broker_invoke_confirmed(h, "webApps.uninstall", args, options, gesture, cb, ctx, rel);
}
