/* Runtime core: registered windows (bdk_window.h, T-SWF-08) and the extension calls that need the
   user's gesture (bdk_extensions.h, T-EXT-02, T-EXT-04). The checks and the bookkeeping are here;
   attachments, actions and menus are the backend's. */
#include <algorithm>
#include <cstring>

#include "broker_internal.h"

using namespace bdkcore;

namespace bdkcore {

std::shared_ptr<Window> window_of(bdk_window_t h) {
    return get<Window>(h, T_WINDOW);
}

void release_window_handlers(Window& w) {
    release_later(w.anchor_ctx, w.anchor_rel);
    release_later(w.request_ctx, w.request_rel);
    w.anchor_cb = nullptr;
    w.anchor_ctx = nullptr;
    w.anchor_rel = nullptr;
    w.request_cb = nullptr;
    w.request_ctx = nullptr;
    w.request_rel = nullptr;
}

bool window_anchor(const Window& w, uint32_t role, double out_rect[4]) {
    if (!w.anchor_cb || check_thread() != BDK_OK)
        return false;
    double rect[4] = {0, 0, 0, 0};
    if (!w.anchor_cb(w.anchor_ctx, w.handle, role, rect))
        return false;
    memcpy(out_rect, rect, sizeof rect);
    return true;
}

void window_request(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Window>& w,
                    const std::string& request_json, std::function<void(bdk_ui_response_t)> done) {
    post([rt, w, request_json, done = std::move(done)] {
        if (rt->shutting || table_type(w->handle) != T_WINDOW)
            return;
        bdk_ui_response_t answer = BDK_UI_USE_DEFAULT;
        if (w->request_cb) {
            answer = w->request_cb(w->request_ctx, w->handle, view(request_json));
            /* Anything but a known answer is a rejection, as for a UI request. */
            if (answer != BDK_UI_HANDLED && answer != BDK_UI_USE_DEFAULT)
                answer = BDK_UI_REJECT;
        }
        post([done, answer] { done(answer); });
    });
}

/* ---- Backend defaults ---- */

bdk_window_t Backend::WindowForContents(const Contents&) {
    return 0;
}

void Backend::WindowContents(const Window&, std::vector<bdk_contents_t>* contents,
                             bdk_contents_t* active) {
    contents->clear();
    *active = 0;
}

int32_t Backend::WindowExtensionId(const Window&) {
    return -1;
}

bdk_status_t Backend::ExtensionActivateAction(Runtime&, const std::shared_ptr<Contents>&,
                                              const std::string& id, bdk_native_handle_t,
                                              std::string*, bdk_error_t* err) {
    return fail(err, BDK_DOMAIN_EXTENSION, BDK_CODE_NOT_FOUND,
                "no extension \"" + id + "\" is installed");
}

bdk_status_t Backend::ExtensionPerformCommand(Runtime&, const std::shared_ptr<Contents>&,
                                              const std::string& id, const std::string&,
                                              bdk_native_handle_t, std::string*, bdk_error_t* err) {
    return fail(err, BDK_DOMAIN_EXTENSION, BDK_CODE_NOT_FOUND,
                "no extension \"" + id + "\" is installed");
}

bdk_status_t Backend::ExtensionPresentMenu(Runtime&, const Window&, bdk_native_handle_t,
                                           bdk_error_t*) {
    return BDK_OK;
}

} // namespace bdkcore

namespace {

/* Common prologue of a window call. */
std::shared_ptr<Window> window_for(bdk_window_t h, bdk_error_t* err, bdk_status_t* status) {
    clear_out(err);
    *status = check_thread();
    if (*status != BDK_OK)
        return nullptr;
    auto w = window_of(h);
    if (!w)
        *status = BDK_ERR_STALE_HANDLE;
    return w;
}

/* Common prologue of an extension call on a page: a live, open page of a running runtime and a
   gesture of the one kind the ABI has. */
std::shared_ptr<Contents> page_for(bdk_contents_t h, bdk_string_t id, bdk_native_handle_t gesture,
                                   bdk_buffer_t* out, bdk_error_t* err, bdk_status_t* status,
                                   std::shared_ptr<Runtime>* rt) {
    clear_out(err);
    if (out)
        *out = 0;
    *status = check_thread();
    if (*status != BDK_OK)
        return nullptr;
    auto c = contents_of(h);
    *rt = c ? c->rt.lock() : nullptr;
    if (!c || !*rt || (*rt)->shutting) {
        *status = BDK_ERR_STALE_HANDLE;
        return nullptr;
    }
    if (!out || !string_ok(id) || id.length == 0) {
        *status = BDK_ERR_INVALID_ARGUMENT;
        return nullptr;
    }
    if (c->closed) {
        *status = fail(err, BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "the contents is closed");
        return nullptr;
    }
    if (gesture.kind != BDK_NATIVE_NSEVENT) {
        *status = fail(err, BDK_DOMAIN_UI, BDK_CODE_GESTURE_REQUIRED,
                       "activating an extension needs the event that confirmed it");
        return nullptr;
    }
    if (!ui_surface_available(**rt, BDK_SURFACE_EXTENSION_ACTIONS)) {
        *status = fail(err, BDK_DOMAIN_EXTENSION, BDK_CODE_SURFACE_DISABLED,
                       "extension actions are disabled ([extensions.ui] actions)");
        return nullptr;
    }
    return c;
}

} // namespace

extern "C" {

/* ---- bdk_window.h ---- */

BDK_EXPORT bdk_status_t bdk_window_register(bdk_runtime_t h, bdk_native_handle_t native,
                                            uint32_t kind, bdk_window_t* out, bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!out || native.kind == 0 ||
        (kind != BDK_WINDOW_KIND_NORMAL && kind != BDK_WINDOW_KIND_POPUP))
        return BDK_ERR_INVALID_ARGUMENT;
    auto w = std::make_shared<Window>();
    w->kind = kind;
    w->native = native;
    w->handle = table_make(T_WINDOW, w);
    if (bdk_status_t s = backend().RegisterWindow(*rt, *w, err)) {
        table_free(w->handle);
        return s;
    }
    rt->windows.push_back(w);
    *out = w->handle;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_window_unregister(bdk_window_t h, bdk_error_t* err) {
    bdk_status_t st;
    auto w = window_for(h, err, &st);
    if (!w)
        return st;
    table_free(h);
    release_window_handlers(*w);
    if (auto rt = current_runtime())
        rt->windows.erase(std::remove(rt->windows.begin(), rt->windows.end(), w),
                          rt->windows.end());
    backend().UnregisterWindow(*w);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_window_set_anchor_provider(bdk_window_t h, bdk_anchor_cb cb, void* ctx,
                                                       bdk_release_cb rel, bdk_error_t* err) {
    bdk_status_t st;
    auto w = window_for(h, err, &st);
    if (!w)
        return st;
    release_later(w->anchor_ctx, w->anchor_rel);
    w->anchor_cb = cb;
    w->anchor_ctx = cb ? ctx : nullptr;
    w->anchor_rel = cb ? rel : nullptr;
    if (!cb)
        release_later(ctx, rel);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_window_set_request_handler(bdk_window_t h, bdk_window_request_cb cb,
                                                       void* ctx, bdk_release_cb rel,
                                                       bdk_error_t* err) {
    bdk_status_t st;
    auto w = window_for(h, err, &st);
    if (!w)
        return st;
    release_later(w->request_ctx, w->request_rel);
    w->request_cb = cb;
    w->request_ctx = cb ? ctx : nullptr;
    w->request_rel = cb ? rel : nullptr;
    if (!cb)
        release_later(ctx, rel);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_window_for_contents(bdk_contents_t h, bdk_window_t* out,
                                                bdk_error_t* err) {
    clear_out(err);
    if (bdk_status_t s = check_thread())
        return s;
    auto c = contents_of(h);
    if (!c)
        return BDK_ERR_STALE_HANDLE;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    bdk_window_t w = backend().WindowForContents(*c);
    *out = table_type(w) == T_WINDOW ? w : 0;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_window_contents(bdk_window_t h, bdk_buffer_t* out, bdk_error_t* err) {
    bdk_status_t st;
    auto w = window_for(h, err, &st);
    if (!w)
        return st;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    std::vector<bdk_contents_t> pages;
    bdk_contents_t active = 0;
    backend().WindowContents(*w, &pages, &active);
    std::string json = "[";
    bool first = true;
    for (bdk_contents_t c : pages) {
        if (table_type(c) != T_CONTENTS)
            continue;
        json += (first ? "\"" : ",\"") + std::to_string(c) + "\"";
        first = false;
    }
    *out = make_buffer(json + "]");
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_window_active_contents(bdk_window_t h, bdk_contents_t* out,
                                                   bdk_error_t* err) {
    bdk_status_t st;
    auto w = window_for(h, err, &st);
    if (!w)
        return st;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    std::vector<bdk_contents_t> pages;
    bdk_contents_t active = 0;
    backend().WindowContents(*w, &pages, &active);
    *out = table_type(active) == T_CONTENTS ? active : 0;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_window_extension_id(bdk_window_t h, int32_t* out, bdk_error_t* err) {
    bdk_status_t st;
    auto w = window_for(h, err, &st);
    if (!w)
        return st;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    *out = backend().WindowExtensionId(*w);
    return BDK_OK;
}

/* ---- bdk_extensions.h ---- */

BDK_EXPORT bdk_status_t bdk_extension_action_activate(bdk_contents_t h, bdk_string_t id,
                                                      bdk_native_handle_t gesture,
                                                      bdk_buffer_t* out, bdk_error_t* err) {
    bdk_status_t st;
    std::shared_ptr<Runtime> rt;
    auto c = page_for(h, id, gesture, out, err, &st, &rt);
    if (!c)
        return st;
    std::string result;
    if (bdk_status_t s = backend().ExtensionActivateAction(*rt, c, str(id), gesture, &result, err))
        return s;
    *out = make_buffer(result);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_extension_command_perform(bdk_contents_t h, bdk_string_t id,
                                                      bdk_string_t command,
                                                      bdk_native_handle_t gesture,
                                                      bdk_buffer_t* out, bdk_error_t* err) {
    bdk_status_t st;
    std::shared_ptr<Runtime> rt;
    auto c = page_for(h, id, gesture, out, err, &st, &rt);
    if (!c)
        return st;
    if (!string_ok(command) || command.length == 0)
        return BDK_ERR_INVALID_ARGUMENT;
    std::string result;
    if (bdk_status_t s =
            backend().ExtensionPerformCommand(*rt, c, str(id), str(command), gesture, &result, err))
        return s;
    *out = make_buffer(result);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_extension_menu_present(bdk_window_t h, bdk_native_handle_t gesture,
                                                   bdk_error_t* err) {
    bdk_status_t st;
    auto w = window_for(h, err, &st);
    if (!w)
        return st;
    auto rt = current_runtime();
    if (!rt || rt->shutting)
        return BDK_ERR_STALE_HANDLE;
    if (gesture.kind != BDK_NATIVE_NSEVENT)
        return fail(err, BDK_DOMAIN_UI, BDK_CODE_GESTURE_REQUIRED,
                    "the extensions menu opens for the event that asked for it");
    if (ui_surface_mode(*rt, BDK_SURFACE_EXTENSION_ACTIONS) != UiMode::kDefault)
        return fail(err, BDK_DOMAIN_EXTENSION, BDK_CODE_SURFACE_DISABLED,
                    "the default extensions menu needs [extensions.ui] actions = \"default\"");
    return backend().ExtensionPresentMenu(*rt, *w, gesture, err);
}

} /* extern "C" */
