/* Stub runtime: the in-memory backend of the runtime core, its task queue and its test hooks.
   Nothing renders; navigation is a scripted state machine. */
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "../runtime/core_internal.h"
#include "bdk_abi_hash.h"
#include "bdk_stub.h"

using namespace bdkcore;

namespace {

constexpr int32_t kErrAborted = -3;
constexpr uint32_t kMainFrame = BDK_EVENT_FLAG_MAIN_FRAME;

struct Nav {
    bool cancelled = false;
};

struct StubContents : BackendData {
    std::vector<Entry> entries;
    int current = -1;
    std::string user_agent;
    bool loading = false, muted = false, focused = false;
    bool auto_discardable = true, discarded = false;
    double progress = 0.0, zoom = 1.0;
    std::shared_ptr<Nav> nav;
};

StubContents& data_of(const Contents& c) {
    return *static_cast<StubContents*>(c.data.get());
}

/* The state of the stub that the tests script and inspect. */
struct StubState {
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    std::map<std::string, std::pair<int32_t, std::string>> scripts;
    bdk_stub_ui_outcome_t outcome{};
};

StubState& S() {
    static StubState* s = new StubState;
    return *s;
}

void set_outcome(uint32_t kind, uint32_t surface, const std::string& completion,
                 const std::string& args) {
    bdk_stub_ui_outcome_t& o = S().outcome;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.kind = kind;
    o.surface = surface;
    snprintf(o.completion, sizeof o.completion, "%s", completion.c_str());
    snprintf(o.arguments_json, sizeof o.arguments_json, "%s", args.c_str());
}

void cancel_nav(const std::shared_ptr<Contents>& c, const char* why) {
    StubContents& d = data_of(*c);
    if (!d.nav)
        return;
    d.nav->cancelled = true;
    d.nav.reset();
    d.loading = false;
    std::string url =
        d.current >= 0 ? d.entries[static_cast<size_t>(d.current)].url : "about:blank";
    emit_contents_event(c, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, url,
                        std::string("{\"code\":\"") + why + "\"}", kErrAborted, kMainFrame);
}

/* target_index -1: a new entry. Otherwise history navigation (or reload) to that entry. */
void begin_nav(const std::shared_ptr<Contents>& c, const std::string& url, int target_index) {
    cancel_nav(c, "superseded");
    StubContents& d = data_of(*c);
    auto nav = std::make_shared<Nav>();
    d.nav = nav;
    d.loading = true;
    d.progress = 0.1;
    uint32_t flags = kMainFrame | BDK_EVENT_FLAG_USER_INITIATED;
    emit_contents_event(c, BDK_CONTENTS_EVENT_NAVIGATION_STARTED, url, "{}", 0, flags);
    post([c, nav, url, target_index, flags] {
        if (nav->cancelled || c->dead)
            return;
        StubContents& d2 = data_of(*c);
        int32_t net_error = 0;
        std::string title = url;
        {
            auto it = S().scripts.find(url);
            if (it != S().scripts.end()) {
                net_error = it->second.first;
                if (!it->second.second.empty())
                    title = it->second.second;
            }
        }
        if (net_error != 0) {
            d2.nav.reset();
            d2.loading = false;
            d2.progress = 1.0;
            emit_contents_event_now(c, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, url,
                                    "{\"code\":\"netError\"}", net_error,
                                    flags | BDK_EVENT_FLAG_ERROR_PAGE);
            return;
        }
        if (target_index < 0) {
            d2.entries.resize(static_cast<size_t>(d2.current) + 1);
            d2.entries.push_back(Entry{url, title});
            d2.current = static_cast<int>(d2.entries.size()) - 1;
        } else {
            d2.current = target_index;
            d2.entries[static_cast<size_t>(target_index)].title = title;
        }
        d2.progress = 0.8;
        emit_contents_event_now(c, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, url, "{}", 0, flags);
        emit_contents_event_now(c, BDK_CONTENTS_EVENT_TITLE_CHANGED, url,
                                "{\"title\":\"" + json_escape(title) + "\"}", 0, kMainFrame);
        post([c, nav, url, flags] {
            if (nav->cancelled || c->dead)
                return;
            StubContents& d3 = data_of(*c);
            d3.nav.reset();
            d3.loading = false;
            d3.progress = 1.0;
            emit_contents_event_now(c, BDK_CONTENTS_EVENT_LOAD_FINISHED, url, "{}", 0, flags);
        });
    });
}

enum class ParseState : uint8_t { kOk, kTooNew, kMalformed };

/* A bounds-checked cursor over the serialised session state. Nothing here relies on a terminator or
   on arithmetic that can overflow: every length is compared against the bytes that remain. */
struct StateCursor {
    const char* p;
    size_t n;
    size_t pos = 0;
    size_t remaining() const {
        return n - pos;
    }
    bool expect(char ch) {
        if (pos >= n || p[pos] != ch)
            return false;
        ++pos;
        return true;
    }
    /* Decimal digits only (no sign, no space); false on no digits or a value above UINT64_MAX. */
    bool uint(uint64_t* out) {
        size_t start = pos;
        uint64_t v = 0;
        while (pos < n && p[pos] >= '0' && p[pos] <= '9') {
            uint64_t d = static_cast<uint64_t>(p[pos] - '0');
            if (v > (UINT64_MAX - d) / 10)
                return false;
            v = v * 10 + d;
            ++pos;
        }
        *out = v;
        return pos != start;
    }
};

ParseState parse_session_state(const char* data, size_t length, long* current_out,
                               std::vector<Entry>* entries_out) {
    static const char kMagic[] = "BDKSTUB";
    const size_t magic = sizeof kMagic - 1;
    StateCursor cur{data, length};
    if (length < magic || memcmp(data, kMagic, magic) != 0)
        return ParseState::kMalformed;
    cur.pos = magic;
    size_t version_start = cur.pos;
    while (cur.pos < cur.n && data[cur.pos] >= '0' && data[cur.pos] <= '9')
        ++cur.pos;
    if (cur.pos == version_start)
        return ParseState::kMalformed;
    if (!(cur.pos - version_start == 1 && data[version_start] == '1'))
        return ParseState::kTooNew;
    if (!cur.expect(' '))
        return ParseState::kMalformed;

    /* current is -1 or an index; count is the number of entries. */
    bool no_current = cur.expect('-');
    uint64_t current = 0, count = 0;
    if (no_current) {
        if (!cur.expect('1'))
            return ParseState::kMalformed;
    } else if (!cur.uint(&current)) {
        return ParseState::kMalformed;
    }
    if (!cur.expect(' ') || !cur.uint(&count) || !cur.expect('\n'))
        return ParseState::kMalformed;
    /* Each entry takes at least "0 0\n\n" (5 bytes), which bounds the count by the input. */
    if (count > cur.remaining() / 5 || count > static_cast<uint64_t>(INT_MAX))
        return ParseState::kMalformed;
    if (!no_current && current >= count)
        return ParseState::kMalformed;

    std::vector<Entry> entries;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t ul = 0, tl = 0;
        if (!cur.uint(&ul) || !cur.expect(' ') || !cur.uint(&tl) || !cur.expect('\n'))
            return ParseState::kMalformed;
        size_t left = cur.remaining();
        if (ul > left || tl > left - static_cast<size_t>(ul))
            return ParseState::kMalformed;
        size_t u = static_cast<size_t>(ul), t = static_cast<size_t>(tl);
        if (left - u - t < 1 || data[cur.pos + u + t] != '\n')
            return ParseState::kMalformed;
        entries.push_back(
            Entry{std::string(data + cur.pos, u), std::string(data + cur.pos + u, t)});
        cur.pos += u + t + 1;
    }
    if (cur.pos != cur.n) /* trailing bytes */
        return ParseState::kMalformed;
    *current_out = no_current ? -1 : static_cast<long>(current);
    *entries_out = std::move(entries);
    return ParseState::kOk;
}

class StubBackend final : public Backend {
  public:
    void Post(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(S().mutex);
        S().queue.push_back(std::move(task));
    }

    bdk_status_t Start(Runtime&, const bdk_start_config_t&, const std::string&,
                       bdk_error_t*) override {
        S().scripts.clear();
        set_outcome(BDK_STUB_UI_NONE, 0, "", "");
        return BDK_OK;
    }

    void Shutdown(Runtime& rt, std::function<void()> done) override {
        rt.contents.clear();
        done();
    }

    std::string ManifestJson() override {
        return std::string("{\"bdk\":\"0.0.0-stub\",\"chromium\":null,\"digest\":null,"
                           "\"abiVersion\":") +
               std::to_string(BDK_ABI_VERSION) + ",\"abiHash\":\"" BDK_ABI_HASH "\"}";
    }

    void ProfilesChanged(Runtime&) override {}

    void OpenProfile(Runtime&, const std::shared_ptr<Profile>&, Done done) override {
        done("", "");
    }

    bdk_status_t OpenPrivateProfile(Runtime&, Profile&, bdk_error_t*) override {
        return BDK_OK;
    }

    void CloseProfile(Runtime&, Profile&) override {}

    void DeleteProfile(Runtime&, const Profile&, Done done) override {
        done("", "");
    }

    bdk_status_t CreateContents(Runtime&, const std::shared_ptr<Contents>& c,
                                const std::string& url, bdk_error_t*) override {
        c->data = std::make_unique<StubContents>();
        if (!url.empty())
            begin_nav(c, url, -1);
        return BDK_OK;
    }

    void CloseContents(const std::shared_ptr<Contents>& c) override {
        cancel_nav(c, "aborted");
        emit_contents_event(c, BDK_CONTENTS_EVENT_CLOSED, "", "{}", 0, 0);
    }

    void DestroyContents(Runtime&, Contents& c) override {
        StubContents& d = data_of(c);
        if (d.nav)
            d.nav->cancelled = true;
    }

    bdk_status_t Load(const std::shared_ptr<Contents>& c, const std::string& url,
                      bdk_error_t*) override {
        begin_nav(c, url, -1);
        return BDK_OK;
    }

    bdk_status_t Reload(const std::shared_ptr<Contents>& c, bool, bdk_error_t*) override {
        StubContents& d = data_of(*c);
        begin_nav(c, d.entries[static_cast<size_t>(d.current)].url, d.current);
        return BDK_OK;
    }

    void Stop(const std::shared_ptr<Contents>& c) override {
        cancel_nav(c, "aborted");
    }

    bdk_status_t GoToIndex(const std::shared_ptr<Contents>& c, int index, bdk_error_t*) override {
        begin_nav(c, data_of(*c).entries[static_cast<size_t>(index)].url, index);
        return BDK_OK;
    }

    void Entries(const Contents& c, int* current, std::vector<Entry>* entries) override {
        *current = data_of(c).current;
        *entries = data_of(c).entries;
    }

    void State(const Contents& c, ContentsState* st) override {
        const StubContents& d = data_of(c);
        bool has = d.current >= 0;
        st->url = has ? d.entries[static_cast<size_t>(d.current)].url : "about:blank";
        st->title = has ? d.entries[static_cast<size_t>(d.current)].title : "";
        st->loading = d.loading;
        st->progress = d.progress;
        if (st->url.compare(0, 8, "https://") == 0) {
            st->security = BDK_SECURITY_SECURE;
        } else if (st->url.compare(0, 7, "http://") == 0) {
            st->security = BDK_SECURITY_NONE;
        } else {
            st->security = BDK_SECURITY_UNKNOWN;
        }
        st->audio = d.muted ? BDK_AUDIO_MUTED : 0;
        st->visibility = BDK_VISIBILITY_VISIBLE;
        st->focused = d.focused;
        st->zoom = d.zoom;
    }

    std::string SessionState(const Contents& c) override {
        const StubContents& d = data_of(c);
        std::string s =
            "BDKSTUB1 " + std::to_string(d.current) + " " + std::to_string(d.entries.size()) + "\n";
        for (auto& e : d.entries) {
            s += std::to_string(e.url.size()) + " " + std::to_string(e.title.size()) + "\n" +
                 e.url + e.title + "\n";
        }
        return s;
    }

    bdk_status_t Restore(const std::shared_ptr<Contents>& c, bdk_bytes_t state,
                         bdk_error_t* err) override {
        std::vector<Entry> entries;
        long current = -1;
        switch (parse_session_state(reinterpret_cast<const char*>(state.data), state.length,
                                    &current, &entries)) {
        case ParseState::kOk:
            break;
        case ParseState::kTooNew:
            return fail(err, BDK_DOMAIN_CONTENTS, "stateTooNew",
                        "the session state is from a newer format");
        case ParseState::kMalformed:
            return BDK_ERR_INVALID_ARGUMENT;
        }
        cancel_nav(c, "aborted");
        StubContents& d = data_of(*c);
        d.entries = std::move(entries);
        d.current = static_cast<int>(current);
        return BDK_OK;
    }

    bdk_status_t SetMuted(Contents& c, bool muted, bdk_error_t*) override {
        data_of(c).muted = muted;
        return BDK_OK;
    }

    bdk_status_t SetZoom(Contents& c, double zoom, bdk_error_t*) override {
        data_of(c).zoom = zoom;
        return BDK_OK;
    }

    bdk_status_t SetUserAgent(Contents& c, const std::string& ua, bdk_error_t*) override {
        data_of(c).user_agent = ua;
        return BDK_OK;
    }

    bdk_status_t Discard(Contents& c, bdk_error_t*) override {
        data_of(c).discarded = true;
        return BDK_OK;
    }

    bdk_status_t SetAutoDiscardable(Contents& c, bool discardable, bdk_error_t*) override {
        data_of(c).auto_discardable = discardable;
        return BDK_OK;
    }

    bdk_status_t NativeView(Contents&, bdk_native_handle_t*, bdk_error_t* err) override {
        return fail(err, BDK_DOMAIN_PLATFORM, "unsupported",
                    "the stub runtime has no native views");
    }

    bdk_status_t Focus(Runtime& rt, const std::shared_ptr<Contents>& c) override {
        for (auto& o : rt.contents) {
            if (o != c && data_of(*o).focused) {
                data_of(*o).focused = false;
                emit_contents_event(o, BDK_CONTENTS_EVENT_FOCUS_CHANGED, "", "{\"focused\":false}",
                                    0, 0);
            }
        }
        if (!data_of(*c).focused) {
            data_of(*c).focused = true;
            emit_contents_event(c, BDK_CONTENTS_EVENT_FOCUS_CHANGED, "", "{\"focused\":true}", 0,
                                0);
        }
        return BDK_OK;
    }

    bdk_status_t RegisterWindow(Runtime&, const Window&, bdk_error_t*) override {
        return BDK_OK;
    }

    void UnregisterWindow(const Window&) override {}
};

} // namespace

namespace bdkcore {

Backend& backend() {
    static StubBackend* b = new StubBackend;
    return *b;
}

} // namespace bdkcore

extern "C" {

BDK_EXPORT size_t bdk_stub_pump(void) {
    if (check_thread() != BDK_OK)
        return 0;
    size_t n = 0;
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(S().mutex);
            if (S().queue.empty())
                break;
            fn = std::move(S().queue.front());
            S().queue.pop_front();
        }
        fn();
        ++n;
    }
    return n;
}

BDK_EXPORT bdk_status_t bdk_stub_script_navigation(bdk_runtime_t h, bdk_string_t url,
                                                   const bdk_stub_navigation_script_t* script) {
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!script || script->struct_size < sizeof *script || !string_ok(url) ||
        !string_ok(script->title))
        return BDK_ERR_INVALID_ARGUMENT;
    S().scripts[str(url)] = std::make_pair(script->net_error, str(script->title));
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_raise_ui_request(bdk_runtime_t h, uint32_t surface,
                                                  bdk_contents_t contents, bdk_string_t payload) {
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    auto c = contents ? contents_of(contents) : nullptr;
    if ((contents && !c) || !string_ok(payload))
        return BDK_ERR_INVALID_ARGUMENT;
    auto r = std::make_shared<Request>();
    r->surface = surface;
    r->contents = contents;
    r->payload = str(payload);
    r->on_finish = [surface](UiOutcome outcome, const std::string& completion,
                             const std::string& args) {
        set_outcome(static_cast<uint32_t>(outcome), surface, completion, args);
    };
    set_outcome(BDK_STUB_UI_NONE, surface, "", "");
    raise_ui_request(rt, r);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_withdraw_ui_requests(bdk_runtime_t h) {
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    withdraw_contents_requests(rt, 0);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_last_ui_outcome(bdk_runtime_t h, bdk_stub_ui_outcome_t* out) {
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    *out = S().outcome;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_simulate_key(bdk_contents_t h, bdk_native_handle_t ev,
                                              uint8_t* out_reserved) {
    if (bdk_status_t s = check_thread())
        return s;
    auto c = contents_of(h);
    if (!c)
        return BDK_ERR_STALE_HANDLE;
    bool reserved = reserve_key(*c, ev);
    if (!reserved)
        report_unhandled_key(*c, ev);
    if (out_reserved)
        *out_reserved = reserved ? 1 : 0;
    return BDK_OK;
}

} /* extern "C" */
