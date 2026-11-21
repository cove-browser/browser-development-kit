/* The stub's test hooks for the broker (bdk_stub_broker.h). */
#include "bdk_stub_broker.h"

#include "stub_services.h"

using namespace bdkcore;

namespace {

/* The capability names of a JSON array of strings; false if it is not one. */
bool read_capabilities(const std::string& json, std::vector<std::string>* out) {
    JsonValue v;
    if (!valid_json(json) || !json_parse(json, &v) || v.type != JsonValue::Type::kArray)
        return false;
    for (auto& item : v.items) {
        if (item.type != JsonValue::Type::kString)
            return false;
        out->push_back(item.text);
    }
    return true;
}

bdk_call_t invoke_as(bdk_runtime_t h, bdk_stub_caller_t id, const CommandRef& ref,
                     bdk_string_t args, const bdk_invoke_options_t* opts, uint8_t gesture,
                     bdk_result_cb cb, bdk_progress_cb prog, void* ctx, bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb || !string_ok(args))
        return 0;
    auto rt = runtime_of(h);
    auto it = bdkstub::callers().find(id);
    if (!rt || it == bdkstub::callers().end() || !invoke_options_ok(opts))
        return 0;
    return broker_submit(rt, it->second, ref, str(args), opts, gesture != 0, cb, prog, ctx, rel);
}

} // namespace

extern "C" {

BDK_EXPORT bdk_status_t bdk_stub_caller_create(bdk_runtime_t h, const bdk_stub_caller_spec_t* spec,
                                               bdk_stub_caller_t* out, bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    if (!spec || spec->struct_size < sizeof *spec || !out ||
        (spec->kind != BDK_STUB_CALLER_PAGE && spec->kind != BDK_STUB_CALLER_SESSION) ||
        !string_ok(spec->name) || !string_ok(spec->capabilities_json))
        return BDK_ERR_INVALID_ARGUMENT;
    auto caller = std::make_shared<Caller>();
    caller->kind = spec->kind == BDK_STUB_CALLER_PAGE ? CallerKind::kPage : CallerKind::kSession;
    caller->name = str(spec->name);
    if (!read_capabilities(str(spec->capabilities_json), &caller->capabilities))
        return BDK_ERR_INVALID_ARGUMENT;
    std::string problem = grant_error(caller->kind, caller->capabilities);
    if (!problem.empty())
        return fail(out_error, BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED, problem);
    caller->profile = spec->profile;
    if (caller->kind == CallerKind::kPage && !get<Profile>(spec->profile, T_PROFILE))
        return BDK_ERR_STALE_HANDLE;
    caller->scope.all_profiles = spec->all_profiles != 0;
    caller->scope.all_contents = spec->all_contents != 0;
    for (size_t i = 0; i < spec->scope_profile_count; ++i)
        caller->scope.profiles.insert(spec->scope_profiles[i]);
    for (size_t i = 0; i < spec->scope_contents_count; ++i)
        caller->scope.contents.insert(spec->scope_contents[i]);
    static bdk_stub_caller_t next = 1;
    bdkstub::callers()[next] = caller;
    *out = next++;
    return BDK_OK;
}

BDK_EXPORT bdk_call_t bdk_stub_invoke_as(bdk_runtime_t h, bdk_stub_caller_t caller,
                                         bdk_string_t name, bdk_string_t args,
                                         const bdk_invoke_options_t* opts, uint8_t gesture,
                                         bdk_result_cb cb, bdk_progress_cb prog, void* ctx,
                                         bdk_release_cb rel) {
    if (!string_ok(name))
        return 0;
    CommandRef ref;
    ref.name = str(name);
    return invoke_as(h, caller, ref, args, opts, gesture, cb, prog, ctx, rel);
}

BDK_EXPORT bdk_call_t bdk_stub_invoke_as_id(bdk_runtime_t h, bdk_stub_caller_t caller,
                                            uint32_t command_id, bdk_string_t args,
                                            const bdk_invoke_options_t* opts, uint8_t gesture,
                                            bdk_result_cb cb, bdk_progress_cb prog, void* ctx,
                                            bdk_release_cb rel) {
    CommandRef ref;
    ref.id = command_id;
    return invoke_as(h, caller, ref, args, opts, gesture, cb, prog, ctx, rel);
}

BDK_EXPORT bdk_status_t bdk_stub_caller_revoke(bdk_runtime_t h, bdk_stub_caller_t id) {
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    auto it = bdkstub::callers().find(id);
    if (!rt || it == bdkstub::callers().end())
        return BDK_ERR_STALE_HANDLE;
    broker_revoke_caller(it->second);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_set_authentication(bdk_runtime_t h, uint32_t mode) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    if (mode > 2)
        return BDK_ERR_INVALID_ARGUMENT;
    bdkstub::controls().authentication = static_cast<int>(mode);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_answer_authentication(bdk_runtime_t h, uint8_t granted) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    return bdkstub::services_answer_authentication(granted != 0) ? BDK_OK
                                                                 : BDK_ERR_INVALID_ARGUMENT;
}

BDK_EXPORT bdk_status_t bdk_stub_hold_services(bdk_runtime_t h, uint8_t hold) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    bdkstub::controls().hold = hold != 0;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_release_held_commands(bdk_runtime_t h) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    bdkstub::services_release_held();
    return BDK_OK;
}

BDK_EXPORT uint32_t bdk_stub_service_count(bdk_runtime_t h, uint32_t which) {
    /* The service counts outlive the runtime: a test reads them after shutdown. */
    if (check_thread() != BDK_OK)
        return 0;
    const bdkstub::Controls& c = bdkstub::controls();
    switch (which) {
    case BDK_STUB_COUNT_EXECUTED:
        return c.executed;
    case BDK_STUB_COUNT_HELD:
        return c.held;
    case BDK_STUB_COUNT_STOPPED:
        return c.stopped;
    case BDK_STUB_COUNT_AUTHENTICATIONS:
        return c.authentications;
    case BDK_STUB_COUNT_LIVE_CALLS: {
        auto rt = runtime_of(h);
        if (!rt)
            return 0;
        uint32_t live = 0;
        for (const auto& w : rt->calls)
            live += w.expired() ? 0 : 1;
        return live;
    }
    default:
        return 0;
    }
}

} // extern "C"
