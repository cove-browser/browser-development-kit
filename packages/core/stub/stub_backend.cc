/* Stub runtime: the in-memory backend of the runtime core, its task queue and its test hooks.
   Nothing renders; navigation is a scripted state machine. */
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "../runtime/core_internal.h"
#include "../runtime/routes.h"
#include "bdk_abi_hash.h"
#include "bdk_stub.h"
#include "stub_services.h"

using namespace bdkcore;

namespace {

constexpr int32_t kErrAborted = -3;
constexpr uint32_t kMainFrame = BDK_EVENT_FLAG_MAIN_FRAME;

/* Chromium's zoom steps (components/zoom/page_zoom_constants.cc), so zoom in and out move as they
   do in the engine. */
constexpr double kZoomSteps[] = {0.25, 1.0 / 3.0, 0.5,  2.0 / 3.0, 0.75, 0.8, 0.9, 1.0, 1.1,
                                 1.25, 1.5,       1.75, 2.0,       2.5,  3.0, 4.0, 5.0};

struct Nav {
    bool cancelled = false;
};

struct StubContents : BackendData {
    std::vector<Entry> entries;
    int current = -1;
    std::string user_agent;
    bool loading = false, muted = false, focused = false;
    bool auto_discardable = true, discarded = false;
    std::string find_text;  /* the search in progress; empty when none */
    bool find_open = false; /* a find request is outstanding */
    double progress = 0.0, zoom = 1.0;
    std::shared_ptr<Nav> nav;
    /* The broker callers bound for the page (bdk_stub_page_bind): revoked when it navigates. */
    std::vector<std::shared_ptr<Caller>> callers;
};

StubContents& data_of(const Contents& c) {
    return *static_cast<StubContents*>(c.data.get());
}

/* The URL the application sees for one the stub keeps: the branded form of an internal page. */
std::string shown(const Contents& c, const std::string& url) {
    auto rt = c.rt.lock();
    return rt ? display_url(*rt, url) : url;
}

/* Privilege ends when the page's document changes (architecture.md 9.2). */
void revoke_callers(Contents& c) {
    auto callers = std::move(data_of(c).callers);
    data_of(c).callers.clear();
    for (auto& caller : callers)
        broker_revoke_caller(caller);
}

/* The state of the stub that the tests script and inspect. */
struct StubState {
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    std::map<std::string, std::pair<int32_t, std::string>> scripts;
    bdk_stub_ui_outcome_t outcome{};
    /* What the stub keeps of application settings, by profile id ("" for the application): its
       stand-in for the disk, so a closed and reopened profile gets its values back. */
    std::map<std::string, std::string> app_settings;
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
    emit_contents_event(c, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, shown(*c, url),
                        std::string("{\"code\":\"") + why + "\"}", kErrAborted, kMainFrame);
}

void commit_nav(const std::shared_ptr<Contents>& c, const std::shared_ptr<Nav>& nav,
                const std::string& url, int target_index, uint32_t flags, int32_t route_error);

/* target_index -1: a new entry. Otherwise history navigation (or reload) to that entry. trusted is
   false for a navigation a web page starts (bdk_stub_page_navigate). */
void begin_nav(const std::shared_ptr<Contents>& c, const std::string& requested, int target_index,
               bool trusted = true) {
    auto rt = c->rt.lock();
    if (!rt)
        return;
    std::string url = requested;
    int32_t route_error = 0;
    RouteNavigation route = resolve_navigation(*rt, requested, trusted);
    switch (route.kind) {
    case RouteNavigation::Kind::kRefused:
        return; /* as Chromium drops a web page's navigation to chrome:// */
    case RouteNavigation::Kind::kLoad:
        url = route.url;
        break;
    case RouteNavigation::Kind::kBlocked:
        url = route.display;
        route_error = route.net_error;
        break;
    case RouteNavigation::Kind::kNative:
    case RouteNavigation::Kind::kPass:
        break;
    }
    cancel_nav(c, "superseded");
    StubContents& d = data_of(*c);
    auto nav = std::make_shared<Nav>();
    d.nav = nav;
    d.loading = true;
    d.progress = 0.1;
    uint32_t flags = kMainFrame | BDK_EVENT_FLAG_USER_INITIATED;
    emit_contents_event(c, BDK_CONTENTS_EVENT_NAVIGATION_STARTED, shown(*c, url), "{}", 0, flags);
    if (route.kind == RouteNavigation::Kind::kNative) {
        /* The navigation is cancelled in the page; the handler decides what the user sees. */
        std::string display = route.display;
        dispatch_native_page(rt, route, c, [c, nav, display, flags](bool handled) {
            if (nav->cancelled || c->dead)
                return;
            if (handled) {
                cancel_nav(c, "aborted");
                return;
            }
            commit_nav(c, nav, display, -1, flags, kNetErrorBlockedByClient);
        });
        return;
    }
    post([c, nav, url, target_index, flags, route_error] {
        commit_nav(c, nav, url, target_index, flags, route_error);
    });
}

void commit_nav(const std::shared_ptr<Contents>& c, const std::shared_ptr<Nav>& nav,
                const std::string& url, int target_index, uint32_t flags, int32_t route_error) {
    {
        if (nav->cancelled || c->dead)
            return;
        StubContents& d2 = data_of(*c);
        int32_t net_error = route_error;
        std::string title = shown(*c, url);
        {
            auto it = S().scripts.find(url);
            if (it == S().scripts.end())
                it = S().scripts.find(title);
            if (it != S().scripts.end() && net_error == 0) {
                net_error = it->second.first;
                if (!it->second.second.empty())
                    title = it->second.second;
            }
        }
        if (net_error != 0) {
            revoke_callers(*c);
            /* As in Chromium, the error page commits as an entry for the URL that failed (a new
               entry, or the history entry being revisited). */
            if (target_index < 0) {
                d2.entries.resize(static_cast<size_t>(d2.current) + 1);
                d2.entries.push_back(Entry{url, shown(*c, url)});
                d2.current = static_cast<int>(d2.entries.size()) - 1;
            } else {
                d2.current = target_index;
            }
            d2.nav.reset();
            d2.loading = false;
            d2.progress = 1.0;
            emit_contents_event_now(c, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, shown(*c, url),
                                    "{\"code\":\"netError\"}", net_error,
                                    flags | BDK_EVENT_FLAG_ERROR_PAGE);
            return;
        }
        revoke_callers(*c);
        /* Like Chromium, a title event only when the entry's title changes: a new entry always,
           a reload or a history navigation only if the title differs. */
        bool title_changed = true;
        if (target_index < 0) {
            d2.entries.resize(static_cast<size_t>(d2.current) + 1);
            d2.entries.push_back(Entry{url, title});
            d2.current = static_cast<int>(d2.entries.size()) - 1;
        } else {
            d2.current = target_index;
            Entry& entry = d2.entries[static_cast<size_t>(target_index)];
            title_changed = entry.title != title;
            entry.title = title;
        }
        d2.progress = 0.8;
        const std::string display = shown(*c, url);
        emit_contents_event_now(c, BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED, display, "{}", 0,
                                flags);
        if (title_changed) {
            emit_contents_event_now(c, BDK_CONTENTS_EVENT_TITLE_CHANGED, display,
                                    "{\"title\":\"" + json_escape(title) + "\"}", 0, kMainFrame);
        }
        post([c, nav, display, flags] {
            if (nav->cancelled || c->dead)
                return;
            StubContents& d3 = data_of(*c);
            d3.nav.reset();
            d3.loading = false;
            d3.progress = 1.0;
            emit_contents_event_now(c, BDK_CONTENTS_EVENT_LOAD_FINISHED, display, "{}", 0, flags);
        });
    }
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
        S().app_settings.clear();
        set_outcome(BDK_STUB_UI_NONE, 0, "", "");
        bdkstub::services_reset();
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
        /* Later, as Chromium's profile creation is, so a test can cancel an open in flight. */
        post([done] { done("", ""); });
    }

    bdk_status_t OpenPrivateProfile(Runtime&, const std::shared_ptr<Profile>&,
                                    bdk_error_t*) override {
        return BDK_OK;
    }

    void CloseProfile(Runtime&, Profile&) override {}

    std::string LoadAppSettings(Runtime&, const Profile* profile) override {
        auto it = S().app_settings.find(profile ? profile->id : std::string());
        return it == S().app_settings.end() ? std::string() : it->second;
    }

    void SaveAppSettings(Runtime&, const Profile* profile, const std::string& json) override {
        S().app_settings[profile ? profile->id : std::string()] = json;
    }

    void DeleteProfile(Runtime&, const Profile& profile, Done done) override {
        S().app_settings.erase(profile.id);
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
        revoke_callers(*c);
        cancel_nav(c, "aborted");
        emit_contents_event(c, BDK_CONTENTS_EVENT_CLOSED, "", "{}", 0, 0);
    }

    void DestroyContents(Runtime&, Contents& c) override {
        revoke_callers(c);
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
        for (Entry& e : *entries)
            e.url = shown(c, e.url);
    }

    void State(const Contents& c, ContentsState* st) override {
        const StubContents& d = data_of(c);
        bool has = d.current >= 0;
        st->url = shown(c, has ? d.entries[static_cast<size_t>(d.current)].url : "about:blank");
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

    bdk_status_t SetZoom(Contents& c, double zoom, uint32_t scope, bdk_error_t*) override {
        data_of(c).zoom = zoom;
        auto rt = c.rt.lock();
        if (scope != BDK_ZOOM_SCOPE_SITE || !rt)
            return BDK_OK;
        /* The site's level: every page of the profile on the same host follows it. */
        const std::string host = host_of(c);
        for (auto& o : rt->contents) {
            if (o.get() != &c && o->profile == c.profile && !host.empty() && host_of(*o) == host)
                data_of(*o).zoom = zoom;
        }
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

    /* The stub renders nothing, so every search finds no match: one final result with zero
       matches, no active match and an empty rect, for each start and each move. */
    void report_find(const std::shared_ptr<Contents>& c) {
        emit_contents_event(c, BDK_CONTENTS_EVENT_FIND_RESULT, "",
                            "{\"matchCount\":0,\"activeIndex\":0,\"rect\":{\"x\":0,\"y\":0,"
                            "\"width\":0,\"height\":0},\"final\":true}",
                            0, 0);
    }

    bdk_status_t FindStart(const std::shared_ptr<Contents>& c, const std::string& text, bool,
                           bdk_error_t*) override {
        data_of(*c).find_text = text;
        report_find(c);
        return BDK_OK;
    }

    bdk_status_t FindNext(const std::shared_ptr<Contents>& c, bool, bdk_error_t*) override {
        if (!data_of(*c).find_text.empty())
            report_find(c);
        return BDK_OK;
    }

    bdk_status_t FindStop(const std::shared_ptr<Contents>& c, bool, bdk_error_t*) override {
        data_of(*c).find_text.clear();
        return BDK_OK;
    }

    /* A request raised for the page, as the engine raises its own. */
    static void raise_for(const std::shared_ptr<Contents>& c, uint32_t surface,
                          const std::string& payload, const std::function<void()>& on_end) {
        auto rt = c->rt.lock();
        if (!rt)
            return;
        auto r = std::make_shared<Request>();
        r->surface = surface;
        r->contents = c->handle;
        r->payload = payload;
        r->on_finish = [on_end](UiOutcome, const std::string&, const std::string&) {
            if (on_end)
                on_end();
        };
        raise_ui_request(rt, r);
    }

    bdk_status_t FindPresent(const std::shared_ptr<Contents>& c, bdk_error_t*) override {
        StubContents& d = data_of(*c);
        if (d.find_open)
            return BDK_OK; /* only one find UI is open per page */
        d.find_open = true;
        std::weak_ptr<Contents> weak = c;
        const std::string payload =
            d.find_text.empty() ? "{}" : "{\"initialText\":\"" + json_escape(d.find_text) + "\"}";
        raise_for(c, BDK_SURFACE_FIND, payload, [weak] {
            if (auto p = weak.lock())
                data_of(*p).find_open = false;
        });
        return BDK_OK;
    }

    bdk_status_t Print(const std::shared_ptr<Contents>& c, bdk_error_t*) override {
        raise_for(c, BDK_SURFACE_PRINTING, "{\"isUserInitiated\":true}", nullptr);
        return BDK_OK;
    }

    /* The host of the page's current URL, or empty. */
    static std::string host_of(const Contents& c) {
        const StubContents& d = data_of(c);
        if (d.current < 0)
            return std::string();
        const std::string& url = d.entries[static_cast<size_t>(d.current)].url;
        size_t start = url.find("://");
        if (start == std::string::npos)
            return std::string();
        start += 3;
        return url.substr(start, url.find_first_of("/?#:", start) - start);
    }

    /* Whether the current URL is one Chromium can save or show the source of (IsSavableURL). */
    static bool savable(const Contents& c) {
        const StubContents& d = data_of(c);
        if (d.current < 0)
            return false;
        const std::string& url = d.entries[static_cast<size_t>(d.current)].url;
        for (const char* scheme : {"http:", "https:", "file:", "data:"}) {
            if (url.compare(0, strlen(scheme), scheme) == 0)
                return true;
        }
        return false;
    }

    static double zoom_step(double zoom, int direction) {
        if (direction > 0) {
            for (double step : kZoomSteps) {
                if (step > zoom + 1e-9)
                    return step;
            }
            return zoom;
        }
        double found = zoom;
        for (double step : kZoomSteps) {
            if (step < zoom - 1e-9)
                found = step;
        }
        return found;
    }

    /* The stub renders nothing, so edit commands are enabled as Chromium's command controller
       enables them (whenever there is a page) and do nothing. */
    bool CanPerformCommand(const std::shared_ptr<Contents>& c, uint32_t command) override {
        const StubContents& d = data_of(*c);
        const bool has_page = d.current >= 0;
        auto rt = c->rt.lock();
        switch (command) {
        case BDK_PAGE_COMMAND_BACK:
            return d.current > 0;
        case BDK_PAGE_COMMAND_FORWARD:
            return has_page && d.current + 1 < static_cast<int>(d.entries.size());
        case BDK_PAGE_COMMAND_STOP:
            return d.loading;
        case BDK_PAGE_COMMAND_FIND_NEXT:
        case BDK_PAGE_COMMAND_FIND_PREVIOUS:
            return !d.find_text.empty();
        case BDK_PAGE_COMMAND_ZOOM_IN:
            return zoom_step(d.zoom, 1) != d.zoom;
        case BDK_PAGE_COMMAND_ZOOM_OUT:
            return zoom_step(d.zoom, -1) != d.zoom;
        case BDK_PAGE_COMMAND_ZOOM_RESET:
            return d.zoom != 1.0;
        case BDK_PAGE_COMMAND_SAVE_PAGE_AS:
        case BDK_PAGE_COMMAND_VIEW_SOURCE:
            return savable(*c);
        case BDK_PAGE_COMMAND_TOGGLE_DEV_TOOLS:
            return has_page && rt && ui_surface_available(*rt, BDK_SURFACE_DEVTOOLS);
        case BDK_PAGE_COMMAND_TOGGLE_FULLSCREEN:
            return false; /* the stub's pages never enter HTML fullscreen */
        default:
            return has_page;
        }
    }

    bdk_status_t PerformCommand(const std::shared_ptr<Contents>& c, uint32_t command,
                                bdk_error_t* err) override {
        StubContents& d = data_of(*c);
        switch (command) {
        case BDK_PAGE_COMMAND_BACK:
            return GoToIndex(c, d.current - 1, err);
        case BDK_PAGE_COMMAND_FORWARD:
            return GoToIndex(c, d.current + 1, err);
        case BDK_PAGE_COMMAND_RELOAD:
        case BDK_PAGE_COMMAND_RELOAD_BYPASSING_CACHE:
            return Reload(c, command == BDK_PAGE_COMMAND_RELOAD_BYPASSING_CACHE, err);
        case BDK_PAGE_COMMAND_STOP:
            Stop(c);
            return BDK_OK;
        case BDK_PAGE_COMMAND_FIND:
            return FindPresent(c, err);
        case BDK_PAGE_COMMAND_FIND_NEXT:
        case BDK_PAGE_COMMAND_FIND_PREVIOUS:
            return FindNext(c, command == BDK_PAGE_COMMAND_FIND_NEXT, err);
        case BDK_PAGE_COMMAND_ZOOM_IN:
        case BDK_PAGE_COMMAND_ZOOM_OUT:
            d.zoom = zoom_step(d.zoom, command == BDK_PAGE_COMMAND_ZOOM_IN ? 1 : -1);
            return BDK_OK;
        case BDK_PAGE_COMMAND_ZOOM_RESET:
            d.zoom = 1.0;
            return BDK_OK;
        case BDK_PAGE_COMMAND_PRINT:
            return Print(c, err);
        case BDK_PAGE_COMMAND_TOGGLE_MUTE:
            d.muted = !d.muted;
            return BDK_OK;
        default:
            return BDK_OK; /* edit commands, save, view source, DevTools: nothing to act on */
        }
    }

    bdk_status_t ExitPictureInPicture(const std::shared_ptr<Contents>&, bdk_error_t*) override {
        return BDK_OK; /* the stub never opens picture-in-picture */
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

    void ExecuteCommand(Runtime& rt, const std::shared_ptr<BuiltinCall>& call) override {
        bdkstub::services_execute(rt, call);
    }

    void Authenticate(Runtime&, const std::string&, std::function<void(bool)> done) override {
        bdkstub::services_authenticate(std::move(done));
    }
};

} // namespace

namespace bdkstub {

std::string page_committed_url(const Contents& c) {
    const StubContents& d = data_of(c);
    return d.current >= 0 ? d.entries[static_cast<size_t>(d.current)].url : "about:blank";
}

void page_keep_caller(Contents& c, std::shared_ptr<Caller> caller) {
    data_of(c).callers.push_back(std::move(caller));
}

void page_navigate(const std::shared_ptr<Contents>& c, const std::string& url) {
    auto rt = c->rt.lock();
    if (rt)
        begin_nav(c, url, -1, is_internal_page(*rt, page_committed_url(*c)));
}

void page_renderer_gone(const std::shared_ptr<Contents>& c, const char* reason) {
    revoke_callers(*c);
    emit_contents_event(c, BDK_CONTENTS_EVENT_RENDERER_GONE, shown(*c, page_committed_url(*c)),
                        std::string("{\"reason\":\"") + reason + "\"}", 0, kMainFrame);
}

} // namespace bdkstub

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
