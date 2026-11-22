/* The broker part of the harness for the stub runtime (harness_broker.h). */
#include <cstring>

#include "bdk_stub.h"
#include "bdk_stub_broker.h"
#include "harness_broker.h"
#include "harness_extensions.h"

namespace {
bdk_string_t text(const char* s) {
    return bdk_string_t{s, s ? strlen(s) : 0};
}
} // namespace

extern "C" {

int bdk_harness_broker_hooks(void) {
    return 1;
}

bdk_status_t bdk_harness_caller_create(bdk_runtime_t rt, const bdk_harness_caller_spec_t* spec,
                                       bdk_harness_caller_t* out, bdk_error_t* out_error) {
    bdk_stub_caller_spec_t s;
    memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    s.kind = spec->kind == BDK_HARNESS_CALLER_PAGE ? BDK_STUB_CALLER_PAGE : BDK_STUB_CALLER_SESSION;
    s.name = text(spec->name);
    s.capabilities_json = text(spec->capabilities_json);
    s.profile = spec->profile;
    s.all_profiles = static_cast<uint8_t>(spec->all_profiles != 0);
    s.scope_profiles = spec->scope_profiles;
    s.scope_profile_count = spec->scope_profile_count;
    s.all_contents = static_cast<uint8_t>(spec->all_contents != 0);
    s.scope_contents = spec->scope_contents;
    s.scope_contents_count = spec->scope_contents_count;
    return bdk_stub_caller_create(rt, &s, out, out_error);
}

bdk_call_t bdk_harness_invoke_as(bdk_runtime_t rt, bdk_harness_caller_t caller, const char* command,
                                 const char* args, const bdk_invoke_options_t* opts, int gesture,
                                 bdk_result_cb cb, bdk_progress_cb prog, void* ctx,
                                 bdk_release_cb rel) {
    return bdk_stub_invoke_as(rt, caller, text(command), text(args), opts,
                              static_cast<uint8_t>(gesture != 0), cb, prog, ctx, rel);
}

bdk_call_t bdk_harness_invoke_as_id(bdk_runtime_t rt, bdk_harness_caller_t caller, uint32_t id,
                                    const char* args, const bdk_invoke_options_t* opts, int gesture,
                                    bdk_result_cb cb, bdk_progress_cb prog, void* ctx,
                                    bdk_release_cb rel) {
    return bdk_stub_invoke_as_id(rt, caller, id, text(args), opts,
                                 static_cast<uint8_t>(gesture != 0), cb, prog, ctx, rel);
}

bdk_status_t bdk_harness_caller_revoke(bdk_runtime_t rt, bdk_harness_caller_t caller) {
    return bdk_stub_caller_revoke(rt, caller);
}

bdk_status_t bdk_harness_set_authentication(bdk_runtime_t rt, int mode) {
    return bdk_stub_set_authentication(rt, static_cast<uint32_t>(mode));
}

bdk_status_t bdk_harness_answer_authentication(bdk_runtime_t rt, int granted) {
    return bdk_stub_answer_authentication(rt, static_cast<uint8_t>(granted != 0));
}

bdk_status_t bdk_harness_hold_services(bdk_runtime_t rt, int hold) {
    return bdk_stub_hold_services(rt, static_cast<uint8_t>(hold != 0));
}

bdk_status_t bdk_harness_release_held_commands(bdk_runtime_t rt) {
    return bdk_stub_release_held_commands(rt);
}

int bdk_harness_page_hooks(void) {
    return 1;
}

bdk_status_t bdk_harness_page_bind(bdk_runtime_t rt, bdk_contents_t contents, int subframe,
                                   bdk_harness_caller_t* out, bdk_error_t* out_error) {
    return bdk_stub_page_bind(rt, contents, static_cast<uint8_t>(subframe != 0), out, out_error);
}

bdk_status_t bdk_harness_page_navigate(bdk_contents_t contents, const char* url) {
    return bdk_stub_page_navigate(contents, text(url));
}

bdk_subscription_t bdk_harness_subscribe_as(bdk_runtime_t rt, bdk_harness_caller_t caller,
                                            const char* event, bdk_event_cb cb, void* ctx,
                                            bdk_release_cb rel, bdk_error_t* out_error) {
    return bdk_stub_subscribe_as(rt, caller, text(event), cb, ctx, rel, out_error);
}

unsigned bdk_harness_service_count(bdk_runtime_t rt, unsigned which) {
    return bdk_stub_service_count(rt, which);
}

/* ---- harness_extensions.h ---- */

int bdk_harness_extension_hooks(void) {
    return 1;
}

bdk_status_t bdk_harness_extension_package(bdk_runtime_t rt, const char* kind, const char* key,
                                           const char* manifest) {
    return bdk_stub_extension_package(rt, text(kind), text(key), text(manifest));
}

bdk_status_t bdk_harness_attach_to_window(bdk_contents_t c, bdk_window_t w) {
    return bdk_stub_attach_contents(c, w);
}

bdk_status_t bdk_harness_window_request(bdk_window_t w, const char* request) {
    return bdk_stub_window_request(w, text(request));
}

bdk_status_t bdk_harness_last_window_answer(uint32_t* out) {
    return bdk_stub_last_window_answer(out);
}

bdk_status_t bdk_harness_window_anchor(bdk_window_t w, uint32_t role, double out_rect[4],
                                       uint8_t* out_found) {
    return bdk_stub_window_anchor(w, role, out_rect, out_found);
}

bdk_status_t bdk_harness_send_ui_update(bdk_runtime_t rt, const char* update) {
    return bdk_stub_send_ui_update(rt, text(update));
}

bdk_status_t bdk_harness_last_ui_update(bdk_runtime_t rt, char* out, size_t cap) {
    return bdk_stub_last_ui_update(rt, out, cap);
}

bdk_status_t bdk_harness_menus_presented(uint32_t* out) {
    return bdk_stub_menus_presented(out);
}

} /* extern "C" */
