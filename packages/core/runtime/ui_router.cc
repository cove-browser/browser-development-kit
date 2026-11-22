/* The UI request router (T-UIR-01): ui-requests.md sections 1 to 3. It decides what happens to a
   request that Chromium (the backend) raised: the mode of its surface, the handler, the queue of
   its kind and page, the outcome the raiser learns, and the rules no configuration or handler can
   override. It knows nothing about presenters; "default presenter" is an outcome the backend acts
   on. Platform-neutral C++23. */
#include <algorithm>
#include <cstring>

#include "automation_internal.h"
#include "core_internal.h"

namespace bdkcore {
namespace {

/* ---- Surfaces ------------------------------------------------------------------------------ */

constexpr uint32_t kD = 1u, kC = 2u, kX = 4u;
constexpr uint32_t kDC = kD | kC, kDCX = kD | kC | kX;

struct SurfaceInfo {
    const char* name;
    uint32_t id;
    uint32_t modes; /* the modes ui-requests.md section 4 allows */
    /* The completions of this surface that grant (ui-requests.md section 5) and so need a gesture.
       "grant" grants on every surface. */
    const char* grants;
};

/* One row per line and in this layout: packages/cli checks the names and modes against its schema
   (units.test.ts reads these rows), so the two cannot drift. */
// clang-format off
constexpr SurfaceInfo kSurfaces[] = {
    {"permissions", BDK_SURFACE_PERMISSIONS, kDC, "grant"},
    {"javascript_dialogs", BDK_SURFACE_JAVASCRIPT_DIALOGS, kDC, ""},
    {"http_auth", BDK_SURFACE_HTTP_AUTH, kDC, ""},
    {"client_certificates", BDK_SURFACE_CLIENT_CERTIFICATES, kDC, ""},
    {"file_pickers", BDK_SURFACE_FILE_PICKERS, kDCX, ""},
    {"context_menus", BDK_SURFACE_CONTEXT_MENUS, kDCX, ""},
    {"new_windows", BDK_SURFACE_NEW_WINDOWS, kDC, ""},
    {"downloads", BDK_SURFACE_DOWNLOADS, kDCX, "keep"},
    {"passwords", BDK_SURFACE_PASSWORDS, kDCX, "save"},
    {"autofill", BDK_SURFACE_AUTOFILL, kDCX, "save"},
    {"autofill_suggestions", BDK_SURFACE_AUTOFILL_SUGGESTIONS, kDC, "accept"},
    {"printing", BDK_SURFACE_PRINTING, kDCX, ""},
    {"translation", BDK_SURFACE_TRANSLATION, kDCX, ""},
    {"find", BDK_SURFACE_FIND, kDC, ""},
    {"fullscreen", BDK_SURFACE_FULLSCREEN, kDCX, ""},
    {"exclusive_access", BDK_SURFACE_EXCLUSIVE_ACCESS, kDC, ""},
    {"notifications", BDK_SURFACE_NOTIFICATIONS, kDCX, ""},
    {"certificate_errors", BDK_SURFACE_CERTIFICATE_ERRORS, kDC, "proceed"},
    {"reputation_warnings", BDK_SURFACE_REPUTATION_WARNINGS, kDC, "proceed"},
    {"devtools", BDK_SURFACE_DEVTOOLS, kDCX, ""},
    {"device_choosers", BDK_SURFACE_DEVICE_CHOOSERS, kDCX, "select"},
    {"screen_capture", BDK_SURFACE_SCREEN_CAPTURE, kDCX, "share"},
    {"webauthn", BDK_SURFACE_WEBAUTHN, kDCX, ""},
    {"external_protocols", BDK_SURFACE_EXTERNAL_PROTOCOLS, kDCX, "open"},
};
// clang-format on

const SurfaceInfo* surface_by_id(uint32_t id) {
    for (const SurfaceInfo& s : kSurfaces) {
        if (s.id == id)
            return &s;
    }
    return nullptr;
}

const SurfaceInfo* surface_by_name(const std::string& name) {
    for (const SurfaceInfo& s : kSurfaces) {
        if (name == s.name)
            return &s;
    }
    return nullptr;
}

/* The two interstitial surfaces: never disabled, never rejected or dropped into "proceed". */
bool is_interstitial(uint32_t surface) {
    return surface == BDK_SURFACE_CERTIFICATE_ERRORS || surface == BDK_SURFACE_REPUTATION_WARNINGS;
}

bool word_in(const char* words, const std::string& word) {
    std::string list = std::string(" ") + words + " ";
    return list.find(" " + word + " ") != std::string::npos;
}

} // namespace

const char* ui_surface_name(uint32_t surface) {
    const SurfaceInfo* s = surface_by_id(surface);
    return s ? s->name : "unknown";
}

bool completion_grants(uint32_t surface, const std::string& completion) {
    if (completion == "grant")
        return true;
    const SurfaceInfo* s = surface_by_id(surface);
    return s && word_in(s->grants, completion);
}

namespace {

/* ---- A small JSON reader ------------------------------------------------------------------- */

/* Reads the configuration, the payload of a request and the arguments of a completion. Numbers and
   literals are kept as text; nothing here needs more. The text may come from the backend, so the
   depth is bounded. */
/* An object member. Not std::pair: a pair of an incomplete Json is ill-formed, and libstdc++
   rejects it. */
struct JsonMember;

struct Json {
    enum Kind { kOther, kString, kArray, kObject } kind = kOther;
    std::string text; /* a string's value; a number or literal's source text */
    std::vector<Json> items;
    std::vector<JsonMember> members;

    const Json* find(const std::string& key) const;
    bool is_true() const {
        return kind == kOther && text == "true";
    }
};

struct JsonMember {
    std::string first;
    Json second;
};

const Json* Json::find(const std::string& key) const {
    for (const auto& m : members) {
        if (m.first == key)
            return &m.second;
    }
    return nullptr;
}

class JsonReader {
  public:
    explicit JsonReader(const std::string& text) : s_(text) {}
    bool Read(Json* out) {
        Ws();
        if (!Value(out, 0))
            return false;
        Ws();
        return i_ == s_.size();
    }

  private:
    void Ws() {
        while (i_ < s_.size() && strchr(" \t\r\n", s_[i_]))
            ++i_;
    }
    bool Str(std::string* out) {
        if (i_ >= s_.size() || s_[i_] != '"')
            return false;
        ++i_;
        out->clear();
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (i_ >= s_.size())
                return false;
            char e = s_[i_++];
            switch (e) {
            case 'b':
                out->push_back('\b');
                break;
            case 'f':
                out->push_back('\f');
                break;
            case 'n':
                out->push_back('\n');
                break;
            case 'r':
                out->push_back('\r');
                break;
            case 't':
                out->push_back('\t');
                break;
            case 'u': {
                /* Every name and value compared here is ASCII; anything else reads as '?', which
                   matches none of them. */
                if (i_ + 4 > s_.size())
                    return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s_[i_];
                    ++i_;
                    if (!isxdigit(static_cast<unsigned char>(h)))
                        return false;
                    const unsigned digit = isdigit(static_cast<unsigned char>(h))
                                               ? static_cast<unsigned>(h - '0')
                                               : static_cast<unsigned>(tolower(h) - 'a') + 10u;
                    cp = cp * 16u + digit;
                }
                out->push_back(cp < 0x80u ? static_cast<char>(cp) : '?');
                break;
            }
            default:
                out->push_back(e);
                break; /* " \ / */
            }
        }
        return false;
    }
    bool Value(Json* out, int depth) {
        if (depth > 32 || i_ >= s_.size())
            return false;
        char c = s_[i_];
        if (c == '"') {
            out->kind = Json::kString;
            return Str(&out->text);
        }
        if (c == '[' || c == '{') {
            const bool object = c == '{';
            out->kind = object ? Json::kObject : Json::kArray;
            ++i_;
            Ws();
            if (i_ < s_.size() && s_[i_] == (object ? '}' : ']')) {
                ++i_;
                return true;
            }
            for (;;) {
                Ws();
                std::string key;
                if (object) {
                    if (!Str(&key))
                        return false;
                    Ws();
                    if (i_ >= s_.size() || s_[i_] != ':')
                        return false;
                    ++i_;
                    Ws();
                }
                Json v;
                if (!Value(&v, depth + 1))
                    return false;
                if (object)
                    out->members.push_back(JsonMember{std::move(key), std::move(v)});
                else
                    out->items.push_back(std::move(v));
                Ws();
                if (i_ >= s_.size())
                    return false;
                char d = s_[i_++];
                if (d == (object ? '}' : ']'))
                    return true;
                if (d != ',')
                    return false;
            }
        }
        size_t start = i_;
        while (i_ < s_.size() && !strchr(",]} \t\r\n", s_[i_]))
            ++i_;
        out->kind = Json::kOther;
        out->text = s_.substr(start, i_ - start);
        return i_ > start;
    }

    const std::string& s_;
    size_t i_ = 0;
};

bool read_json(const std::string& text, Json* out) {
    return JsonReader(text).Read(out);
}

/* ---- Configuration ------------------------------------------------------------------------- */

bool parse_mode(const std::string& text, UiMode* mode, uint32_t* bit) {
    if (text == "default") {
        *mode = UiMode::kDefault;
        *bit = kD;
    } else if (text == "custom") {
        *mode = UiMode::kCustom;
        *bit = kC;
    } else if (text == "disabled") {
        *mode = UiMode::kDisabled;
        *bit = kX;
    } else {
        return false;
    }
    return true;
}

UiMode mode_of(const Runtime& rt, uint32_t surface) {
    auto it = rt.ui_modes.find(surface);
    return it == rt.ui_modes.end() ? UiMode::kDefault : it->second;
}

/* ---- Finishing ----------------------------------------------------------------------------- */

using QueueKey = std::pair<bdk_contents_t, uint32_t>;

void deliver(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Request>& r);

/* Gives the slot of the kind and page to the next request that is still open. */
void start_next(const std::shared_ptr<Runtime>& rt, QueueKey key) {
    auto it = rt->ui_queues.find(key);
    if (it == rt->ui_queues.end())
        return;
    UiQueue& q = it->second;
    q.busy = false;
    while (!q.waiting.empty()) {
        std::shared_ptr<Request> next = q.waiting.front();
        q.waiting.pop_front();
        if (next->finished) {
            table_free(next->handle); /* withdrawn before the application ever saw it */
            continue;
        }
        q.busy = true;
        next->holds_slot = true;
        deliver(rt, next);
        return;
    }
    rt->ui_queues.erase(it);
}

void finish_request(Request& r, UiOutcome outcome, std::string completion = "",
                    std::string args = "") {
    /* An interstitial is never rejected, dropped or skipped into "proceed": the safe outcome is the
       completion that goes back (ui-requests.md section 3). A withdrawal stays one: the page is
       gone, so there is nothing to go back from. */
    if (is_interstitial(r.surface) &&
        (outcome == UiOutcome::kRejected || outcome == UiOutcome::kCancelled)) {
        outcome = UiOutcome::kCompleted;
        completion = "go_back";
        args = "{}";
    }
    r.finished = true;
    release_withdrawn(r);
    if (r.holds_slot) {
        r.holds_slot = false;
        QueueKey key{r.contents, r.surface};
        if (auto rt = current_runtime()) {
            post([rt, key] {
                if (!rt->shutting)
                    start_next(rt, key);
            });
        }
    }
    if (r.on_finish) {
        auto on_finish = std::move(r.on_finish);
        r.on_finish = nullptr;
        on_finish(outcome, completion, args);
    }
}

/* ---- Delivery ------------------------------------------------------------------------------ */

void deliver(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Request>& r) {
    post([rt, r] {
        if (rt->shutting)
            return;
        auto it = rt->ui_handlers.find(r->surface);
        if (r->finished || it == rt->ui_handlers.end()) {
            /* Never handed to the application, so nothing else will release the handle. With no
               handler the default presenter shows it. */
            if (!r->finished)
                finish_request(*r, UiOutcome::kDefaulted);
            table_free(r->handle);
            return;
        }
        bdk_ui_request_info_t info;
        memset(&info, 0, sizeof info);
        info.struct_size = sizeof info;
        info.request = r->handle;
        info.surface = r->surface;
        info.contents = r->contents;
        info.window = r->window;
        info.profile = r->profile;
        info.payload_json = view(r->payload);
        memcpy(info.anchor, r->anchor, sizeof info.anchor);
        info.secure_view = r->secure_view;
        bdk_ui_response_t resp = it->second.cb(it->second.ctx, &info);
        r->delivered = true;
        if (r->finished || resp == BDK_UI_HANDLED)
            return;
        /* Anything but a known "use the default" is a rejection: the safe reading of a bad value.
         */
        finish_request(*r,
                       resp == BDK_UI_USE_DEFAULT ? UiOutcome::kDefaulted : UiOutcome::kRejected);
    });
}

} // namespace

/* ---- Configuration ------------------------------------------------------------------------- */

bdk_status_t ui_configure(Runtime& rt, const std::string& resolved, bdk_error_t* out_error) {
    Json root;
    if (!read_json(resolved, &root) || root.kind != Json::kObject)
        return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                    "the resolved configuration is not a JSON object");
    const Json* surfaces = root.find("surfaces");
    if (!surfaces)
        return BDK_OK; /* every surface is default */
    if (surfaces->kind != Json::kObject)
        return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                    "\"surfaces\" in the resolved configuration is not an object");
    for (const auto& [name, value] : surfaces->members) {
        const SurfaceInfo* info = surface_by_name(name);
        if (!info)
            return fail(out_error, BDK_DOMAIN_UI, "surfaceUnknown",
                        "unknown UI surface \"" + name + "\" in the resolved configuration");
        UiMode mode;
        uint32_t bit;
        if (value.kind != Json::kString || !parse_mode(value.text, &mode, &bit))
            return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                        "the mode of the UI surface \"" + name +
                            "\" is not default, custom or disabled");
        if (!(info->modes & bit))
            return fail(out_error, BDK_DOMAIN_UI, "surfaceMode",
                        "the UI surface \"" + name + "\" does not allow the mode \"" + value.text +
                            "\"");
        rt.ui_modes[info->id] = mode;
    }
    return BDK_OK;
}

bdk_status_t ui_check_handlers(const Runtime& rt, bdk_error_t* out_error) {
    std::string missing;
    for (const SurfaceInfo& s : kSurfaces) {
        if (mode_of(rt, s.id) == UiMode::kCustom && rt.ui_handlers.count(s.id) == 0)
            missing += (missing.empty() ? "" : ", ") + std::string(s.name);
    }
    if (missing.empty())
        return BDK_OK;
    return fail(out_error, BDK_DOMAIN_CONFIG, "handlerMissing",
                "custom UI surfaces need a handler before the freeze: " + missing);
}

bool ui_surface_available(const Runtime& rt, uint32_t surface) {
    return mode_of(rt, surface) != UiMode::kDisabled;
}

/* ---- Raising ------------------------------------------------------------------------------- */

std::shared_ptr<Request> raise_ui_request(const std::shared_ptr<Runtime>& rt,
                                          std::shared_ptr<Request> r) {
    if (!r->profile && r->contents) {
        auto c = get<Contents>(r->contents, T_CONTENTS);
        r->profile = c && c->profile ? c->profile->handle : 0;
    }
    r->handle = table_make(T_REQUEST, r);
    rt->requests.erase(std::remove_if(rt->requests.begin(), rt->requests.end(),
                                      [](const std::shared_ptr<Request>& q) {
                                          return q->finished && table_type(q->handle) == T_NONE;
                                      }),
                       rt->requests.end());
    rt->requests.push_back(r);
    if (r->on_finish && r->contents)
        automation_request_raised(rt, *r); /* a page a script drives is waiting on its user */

    /* A surface that is disabled, or that no table row describes, denies without a handler. */
    const bool known = surface_by_id(r->surface) != nullptr;
    if (!known || mode_of(*rt, r->surface) == UiMode::kDisabled) {
        const UiOutcome outcome = known ? UiOutcome::kDisabled : UiOutcome::kRejected;
        post([rt, r, outcome] {
            if (rt->shutting)
                return;
            if (!r->finished)
                finish_request(*r, outcome);
            table_free(r->handle);
        });
        return r;
    }
    if (!r->on_finish) { /* a notice: nobody waits for it, so it holds no place in the queue */
        deliver(rt, r);
        return r;
    }
    UiQueue& q = rt->ui_queues[QueueKey{r->contents, r->surface}];
    if (q.busy) {
        q.waiting.push_back(r);
        return r;
    }
    q.busy = true;
    r->holds_slot = true;
    deliver(rt, r);
    return r;
}

void withdraw_ui_request(Request& r) {
    if (r.finished)
        return;
    if (r.withdrawn) {
        void (*cb)(void*) = r.withdrawn;
        void* ctx = r.wctx;
        post([cb, ctx] { cb(ctx); });
    }
    finish_request(r, UiOutcome::kWithdrawn);
}

void withdraw_contents_requests(const std::shared_ptr<Runtime>& rt, bdk_contents_t contents) {
    for (auto& r : std::vector<std::shared_ptr<Request>>(rt->requests)) {
        if (contents && r->contents != contents)
            continue;
        withdraw_ui_request(*r);
    }
}

/* ---- Completing ---------------------------------------------------------------------------- */

namespace {

/* A user gesture is the operating system's event that confirmed the grant. The ABI has one such
   kind. A runtime with a window system can check more (that it is recent, that it went to a window
   of this application); that is the backend's to add when it presents UI. */
bool is_gesture(const bdk_native_handle_t& g) {
    return g.kind == BDK_NATIVE_NSEVENT;
}

/* A string array under key, or false when it is there but is not one. */
bool string_set(const Json& object, const char* key, std::vector<std::string>* out, bool* present) {
    const Json* v = object.find(key);
    *present = v != nullptr;
    if (!v)
        return true;
    if (v->kind != Json::kArray)
        return false;
    for (const Json& item : v->items) {
        if (item.kind != Json::kString)
            return false;
        out->push_back(item.text);
    }
    return true;
}

} // namespace

bdk_status_t ui_complete(Request& r, const std::string& completion, const std::string& arguments,
                         bdk_native_handle_t gesture, bdk_error_t* err) {
    if (r.finished)
        return fail(err, BDK_DOMAIN_UI, BDK_CODE_REQUEST_COMPLETED,
                    "the request is already completed");
    const bool interstitial = is_interstitial(r.surface);
    if (interstitial && completion != "go_back" && completion != "proceed")
        return fail(err, BDK_DOMAIN_UI, "completionNotAllowed",
                    "an interstitial is completed by go_back or proceed only");
    if (completion_grants(r.surface, completion) && !is_gesture(gesture))
        return fail(err, BDK_DOMAIN_UI, BDK_CODE_GESTURE_REQUIRED,
                    "a granting completion needs the gesture event");

    /* Proceeding past an interstitial only where the page offers it: the request says so, and a
       request that does not say is treated as not overridable. */
    if (interstitial && completion == "proceed") {
        Json payload;
        const Json* overridable = nullptr;
        if (read_json(r.payload, &payload))
            overridable = payload.find("isOverridable");
        if (!overridable || !overridable->is_true())
            return fail(err, BDK_DOMAIN_UI, "notOverridable",
                        "this interstitial cannot be proceeded past");
    }

    /* A completion accepts a subset of what the request asked for, never more. */
    if (r.surface == BDK_SURFACE_PERMISSIONS && completion == "grant" && !arguments.empty()) {
        Json args;
        if (!read_json(arguments, &args) || args.kind != Json::kObject)
            return BDK_ERR_INVALID_ARGUMENT;
        std::vector<std::string> granted;
        bool present = false;
        if (!string_set(args, "kinds", &granted, &present))
            return BDK_ERR_INVALID_ARGUMENT;
        if (present) {
            std::vector<std::string> asked;
            Json payload;
            bool asked_present = false;
            if (read_json(r.payload, &payload) && payload.kind == Json::kObject &&
                !string_set(payload, "kinds", &asked, &asked_present))
                asked.clear();
            for (const std::string& kind : granted) {
                if (std::find(asked.begin(), asked.end(), kind) == asked.end())
                    return fail(err, BDK_DOMAIN_UI, "widensRequest",
                                "the grant covers \"" + kind + "\", which was not asked for");
            }
        }
    }
    r.gesture = gesture;
    finish_request(r, UiOutcome::kCompleted, completion, arguments);
    r.gesture = bdk_native_handle_t{};
    return BDK_OK;
}

bdk_status_t ui_use_default(Request& r, bdk_error_t* err) {
    if (r.finished)
        return fail(err, BDK_DOMAIN_UI, BDK_CODE_REQUEST_COMPLETED,
                    "the request is already completed");
    finish_request(r, UiOutcome::kDefaulted);
    return BDK_OK;
}

void ui_release(Request& r) {
    if (!r.finished)
        finish_request(r, UiOutcome::kCancelled);
}

} // namespace bdkcore
