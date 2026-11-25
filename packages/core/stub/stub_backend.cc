/* Stub runtime: the in-memory backend of the runtime core, its task queue and its test hooks.
   Nothing renders; navigation is a scripted state machine. */
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>

#include "../runtime/automation_internal.h"
#include "../runtime/core_internal.h"
#include "../runtime/navigation_policy.h"
#include "../runtime/routes.h"
#include "bdk_abi_hash.h"
#include "bdk_stub.h"
#include "stub_providers.h"
#include "stub_services.h"

using namespace bdkcore;

namespace {

/* The engine version the stub reports to the profile snapshot and downgrade policy: older than any
   Chromium, so a test's "newer" and "older" versions mean the same against the engine. */
constexpr const char* kStubEngineVersion = "1.0.0.0";
constexpr const char* kCrashUrl = "chrome://crash";
constexpr int32_t kErrAborted = -3;
constexpr int32_t kErrBlocked = -20; /* net::ERR_BLOCKED_BY_CLIENT */
constexpr uint32_t kMainFrame = BDK_EVENT_FLAG_MAIN_FRAME;

/* Chromium's zoom steps (components/zoom/page_zoom_constants.cc), so zoom in and out move as they
   do in the engine. */
constexpr double kZoomSteps[] = {0.25, 1.0 / 3.0, 0.5,  2.0 / 3.0, 0.75, 0.8, 0.9, 1.0, 1.1,
                                 1.25, 1.5,       1.75, 2.0,       2.5,  3.0, 4.0, 5.0};

struct Nav {
    bool cancelled = false;
    /* The navigation policy decision it waits for, if any (navigation_policy.h). */
    std::shared_ptr<PolicyDecision> policy;
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
    /* Page health (bdk_stub_page_unresponsive, bdk_stub_block_popup): whether an unresponsive
       request is open, and the popups blocked since the last notice was answered, by id. */
    bool unresponsive = false;
    std::vector<std::pair<std::string, std::string>> blocked_popups;
    std::weak_ptr<Request> blocked_notice;
    uint32_t next_popup_id = 1;
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
    /* PostDelayed: the stub has no clock. Its time moves only when nothing else is pending: pump
       then runs the earliest delayed task (ties in order posted). */
    int64_t now_ms = 0;
    uint64_t delayed_seq = 0;
    std::map<std::pair<int64_t, uint64_t>, std::function<void()>> delayed;
    std::map<std::string, std::pair<int32_t, std::string>> scripts;
    /* Server redirects (bdk_stub_script_redirect): a request for the key is redirected to the
       value. */
    std::map<std::string, std::string> redirects;
    bdk_stub_ui_outcome_t outcome{};
    /* What the stub keeps of application settings, by profile id ("" for the application): its
       stand-in for the disk, so a closed and reopened profile gets its values back. */
    std::map<std::string, std::string> app_settings;
    /* The request bdk_stub_send_ui_update addresses, and the last update the application sent
       to it. */
    std::weak_ptr<Request> last_request;
    std::string last_update;
    /* Registered windows: the pages attached to each, in arrival order, the active one, and the
       identifier extensions would see. */
    struct WindowState {
        std::vector<bdk_contents_t> contents;
        bdk_contents_t active = 0;
        int32_t extension_id = -1;
    };
    std::map<bdk_window_t, WindowState> windows;
    int32_t next_window_id = 1;
    uint32_t window_answer = 0;   /* the last answer to bdk_stub_window_request, 0 for none */
    uint32_t menus_presented = 0; /* bdk_extension_menu_present calls that reached the stub */
    /* Sites whose popups are always allowed, as "<profile id> <origin>": the popup content
       setting a blocked-popup grant {"always":true} writes. */
    std::set<std::string> popups_allowed;
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

/* The window a page is attached to, or null. */
StubState::WindowState* window_with(bdk_contents_t c, bdk_window_t* handle = nullptr) {
    for (auto& [h, w] : S().windows) {
        if (std::find(w.contents.begin(), w.contents.end(), c) != w.contents.end()) {
            if (handle)
                *handle = h;
            return &w;
        }
    }
    return nullptr;
}

void detach(bdk_contents_t c) {
    if (StubState::WindowState* w = window_with(c)) {
        w->contents.erase(std::remove(w->contents.begin(), w->contents.end(), c),
                          w->contents.end());
        if (w->active == c)
            w->active = w->contents.empty() ? 0 : w->contents.back();
    }
}

/* Ends the navigation in progress with no commit: the page stays where it is. */
void end_nav(const std::shared_ptr<Contents>& c, const std::string& detail) {
    StubContents& d = data_of(*c);
    if (!d.nav)
        return;
    d.nav->cancelled = true;
    withdraw_navigation_policy(d.nav->policy);
    d.nav.reset();
    d.loading = false;
    std::string url =
        d.current >= 0 ? d.entries[static_cast<size_t>(d.current)].url : "about:blank";
    emit_contents_event(c, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, shown(*c, url), detail,
                        kErrAborted, kMainFrame);
}

void cancel_nav(const std::shared_ptr<Contents>& c, const char* why) {
    end_nav(c, std::string("{\"code\":\"") + why + "\"}");
}

void commit_nav(const std::shared_ptr<Contents>& c, const std::shared_ptr<Nav>& nav,
                const std::string& url, int target_index, uint32_t flags, int32_t route_error);

/* target_index -1: a new entry. Otherwise history navigation (or reload) to that entry. trusted is
   false for a navigation a web page starts (bdk_stub_page_navigate), and by_page true for any a
   page starts. policy_redirects counts the redirect decisions in a row that began this one. */
void begin_nav(const std::shared_ptr<Contents>& c, const std::string& requested, int target_index,
               bool trusted = true, bool by_page = false, uint32_t policy_redirects = 0);

/* Carries out the application's answer about the navigation to `url` (navigation_policy.h). */
void apply_policy(const std::shared_ptr<Contents>& c, const std::shared_ptr<Nav>& nav,
                  PolicyAction action, const std::string& redirect_url, const std::string& url,
                  int target_index, uint32_t flags, uint32_t policy_redirects,
                  const std::function<void()>& proceed) {
    switch (action) {
    case PolicyAction::kAllow:
        proceed();
        return;
    case PolicyAction::kCancel:
        end_nav(c, policy_failure_detail(action, redirect_url));
        return;
    case PolicyAction::kRedirect:
        end_nav(c, policy_failure_detail(action, redirect_url));
        begin_nav(c, redirect_url, -1, true, false, policy_redirects + 1);
        return;
    case PolicyAction::kTooManyRedirects:
        commit_nav(c, nav, url, target_index, flags, kNetErrorTooManyRedirects);
        return;
    }
}

/* Asks the policy about the navigation, then runs `proceed` if it is allowed. */
void ask_then(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c,
              const std::shared_ptr<Nav>& nav, const PolicyNavigation& ask, const std::string& url,
              int target_index, uint32_t flags, const std::function<void()>& proceed) {
    auto go = std::make_shared<std::function<void()>>(proceed);
    nav->policy = ask_navigation_policy(
        rt, c, ask,
        [c, nav, url, target_index, flags, redirects = ask.policy_redirects,
         go](PolicyAction action, const std::string& redirect_url) {
            if (nav->cancelled || c->dead)
                return;
            nav->policy.reset();
            apply_policy(c, nav, action, redirect_url, url, target_index, flags, redirects, *go);
        });
    if (!nav->policy)
        (*go)();
}

/* Sends the request for `url`: follows the scripted server redirects, asking the policy at each,
   then lets the reputation provider see the URL before it commits (T-PRV-05). */
void fetch_nav(const std::shared_ptr<Contents>& c, const std::shared_ptr<Nav>& nav,
               const std::string& url, int target_index, uint32_t flags, int32_t route_error,
               const PolicyNavigation& asked, uint32_t hops) {
    auto rt = c->rt.lock();
    auto redirect = S().redirects.find(url);
    if (rt && route_error == 0 && redirect != S().redirects.end()) {
        if (hops >= kMaxPolicyRedirects) {
            commit_nav(c, nav, url, target_index, flags, kNetErrorTooManyRedirects);
            return;
        }
        const std::string to = redirect->second;
        PolicyNavigation ask = asked;
        ask.url = shown(*c, to);
        ask.redirected_from = shown(*c, url);
        ask_then(rt, c, nav, ask, to, target_index, flags,
                 [c, nav, to, target_index, flags, ask, hops] {
                     post([c, nav, to, target_index, flags, ask, hops] {
                         if (!nav->cancelled && !c->dead)
                             fetch_nav(c, nav, to, target_index, flags, 0, ask, hops + 1);
                     });
                 });
        return;
    }
    bdkstub::providers_check_navigation(
        c, url, [c, nav, url, target_index, flags, route_error](bool go) {
            if (nav->cancelled || c->dead)
                return;
            if (go) {
                commit_nav(c, nav, url, target_index, flags, route_error);
                return;
            }
            StubContents& blocked = data_of(*c);
            blocked.nav.reset();
            blocked.loading = false;
            emit_contents_event_now(c, BDK_CONTENTS_EVENT_NAVIGATION_FAILED, shown(*c, url),
                                    "{\"code\":\"blocked\"}", kErrBlocked, kMainFrame);
        });
}

void begin_nav(const std::shared_ptr<Contents>& c, const std::string& requested, int target_index,
               bool trusted, bool by_page, uint32_t policy_redirects) {
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
    PolicyNavigation ask;
    ask.url = shown(*c, url);
    ask.type = target_index < 0 ? "load" : target_index == d.current ? "reload" : "history";
    ask.initiator = by_page ? "page" : "application";
    ask.user_initiated = !by_page;
    ask.policy_redirects = policy_redirects;
    auto nav = std::make_shared<Nav>();
    d.nav = nav;
    d.loading = true;
    d.progress = 0.1;
    uint32_t flags = kMainFrame | BDK_EVENT_FLAG_USER_INITIATED;
    emit_contents_event(c, BDK_CONTENTS_EVENT_NAVIGATION_STARTED, shown(*c, url), "{}", 0, flags);
    std::function<void()> proceed;
    if (route.kind == RouteNavigation::Kind::kNative) {
        /* The navigation is cancelled in the page; the handler decides what the user sees. */
        proceed = [rt, route, c, nav, flags] {
            std::string display = route.display;
            dispatch_native_page(rt, route, c, [c, nav, display, flags](bool handled) {
                if (nav->cancelled || c->dead)
                    return;
                if (handled) {
                    /* The navigation ends there; navigate(to:) learns why (T-SWF-09). */
                    cancel_nav(c, "handledNatively");
                    return;
                }
                commit_nav(c, nav, display, -1, flags, kNetErrorBlockedByClient);
            });
        };
    } else {
        proceed = [c, nav, url, target_index, flags, route_error, ask] {
            post([c, nav, url, target_index, flags, route_error, ask] {
                if (!nav->cancelled && !c->dead)
                    fetch_nav(c, nav, url, target_index, flags, route_error, ask, 0);
            });
        };
    }
    /* The application is asked before the request is sent (navigation_policy.h). */
    ask_then(rt, c, nav, ask, url, target_index, flags, proceed);
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

    void PostDelayed(std::function<void()> task, int64_t delay_ms) override {
        std::lock_guard<std::mutex> lock(S().mutex);
        S().delayed[{S().now_ms + std::max<int64_t>(0, delay_ms), S().delayed_seq++}] =
            std::move(task);
    }

    bdk_status_t Start(Runtime&, const bdk_start_config_t&, const std::string&,
                       bdk_error_t*) override {
        S().scripts.clear();
        S().redirects.clear();
        S().app_settings.clear();
        S().last_request.reset();
        S().last_update.clear();
        S().windows.clear();
        S().next_window_id = 1;
        S().window_answer = 0;
        S().menus_presented = 0;
        S().popups_allowed.clear();
        {
            std::lock_guard<std::mutex> lock(S().mutex);
            S().delayed.clear();
        }
        bdkstub::automation_reset();
        set_outcome(BDK_STUB_UI_NONE, 0, "", "");
        bdkstub::services_reset();
        bdkstub::providers_reset();
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

    /* The stub keeps everything in memory. Given a data directory (tests, launch options), it
       applies the snapshot and downgrade policy to it as if it were an engine of version
       kStubEngineVersion. */
    std::string ProfileDataDirectory(const bdk_start_config_t& config,
                                     const std::string&) override {
        return str(config.user_data_path_override);
    }
    std::string EngineVersion() override {
        return kStubEngineVersion;
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
        detach(c.handle);
    }

    bdk_status_t Load(const std::shared_ptr<Contents>& c, const std::string& url,
                      bdk_error_t*) override {
        /* Chromium's debug URL kills the page's renderer and commits nothing; the stub reports it
           the same way, so a crash-recovery test runs on both (tests_robustness.c). */
        if (url == kCrashUrl) {
            Post([weak = std::weak_ptr<Contents>(c)] {
                if (auto page = weak.lock())
                    bdkstub::page_renderer_gone(page, "crashed");
            });
            return BDK_OK;
        }
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
        if (StubState::WindowState* w = window_with(c->handle))
            w->active = c->handle;
        if (!data_of(*c).focused) {
            data_of(*c).focused = true;
            emit_contents_event(c, BDK_CONTENTS_EVENT_FOCUS_CHANGED, "", "{\"focused\":true}", 0,
                                0);
        }
        return BDK_OK;
    }

    bdk_status_t RegisterWindow(Runtime&, const Window& w, bdk_error_t*) override {
        S().windows[w.handle] = StubState::WindowState{};
        return BDK_OK;
    }

    void UnregisterWindow(const Window& w) override {
        S().windows.erase(w.handle);
    }

    bdk_window_t WindowForContents(const Contents& c) override {
        bdk_window_t h = 0;
        window_with(c.handle, &h);
        return h;
    }

    void WindowContents(const Window& w, std::vector<bdk_contents_t>* contents,
                        bdk_contents_t* active) override {
        auto it = S().windows.find(w.handle);
        *contents = it == S().windows.end() ? std::vector<bdk_contents_t>{} : it->second.contents;
        *active = it == S().windows.end() ? 0 : it->second.active;
    }

    int32_t WindowExtensionId(const Window& w) override {
        auto it = S().windows.find(w.handle);
        return it == S().windows.end() ? -1 : it->second.extension_id;
    }

    bdk_status_t ExtensionActivateAction(Runtime& rt, const std::shared_ptr<Contents>& c,
                                         const std::string& id, bdk_native_handle_t,
                                         std::string* result, bdk_error_t* err) override {
        return bdkstub::extensions_activate(rt, c, id, "", result, err);
    }

    bdk_status_t ExtensionPerformCommand(Runtime& rt, const std::shared_ptr<Contents>& c,
                                         const std::string& id, const std::string& command,
                                         bdk_native_handle_t, std::string* result,
                                         bdk_error_t* err) override {
        return bdkstub::extensions_activate(rt, c, id, command, result, err);
    }

    bdk_status_t ExtensionPresentMenu(Runtime&, const Window&, bdk_native_handle_t,
                                      bdk_error_t*) override {
        ++S().menus_presented;
        return BDK_OK;
    }

    void ExecuteCommand(Runtime& rt, const std::shared_ptr<BuiltinCall>& call) override {
        if (!bdkstub::providers_execute(rt, call))
            bdkstub::services_execute(rt, call);
    }

    void Authenticate(Runtime&, const std::string&, std::function<void(bool)> done) override {
        bdkstub::services_authenticate(std::move(done));
    }

    void StartTask(Runtime&, const std::shared_ptr<TaskPort>& task) override {
        bdkstub::automation_start(task);
    }

    void EndTask(Runtime&, TaskPort& task) override {
        bdkstub::automation_end(task);
    }

    void UserScriptsChanged(Runtime&, Profile&, const std::vector<UserScript>&) override {
        bdkstub::automation_scripts_changed();
    }
};

} // namespace

namespace bdkstub {

void track_request(const std::shared_ptr<Request>& r) {
    S().last_request = r;
    S().last_update.clear();
    r->on_update = [](const std::string& update) { S().last_update = update; };
    /* Requests the stub raises itself report their outcome like the raised ones do. */
    const uint32_t surface = r->surface;
    set_outcome(BDK_STUB_UI_NONE, surface, "", "");
    r->on_finish = [surface, inner = std::move(r->on_finish)](
                       UiOutcome outcome, const std::string& completion, const std::string& args) {
        set_outcome(static_cast<uint32_t>(outcome), surface, completion, args);
        if (inner)
            inner(outcome, completion, args);
    };
}

} // namespace bdkstub

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
        begin_nav(c, url, -1, is_internal_page(*rt, page_committed_url(*c)), true);
}

void page_renderer_gone(const std::shared_ptr<Contents>& c, const char* reason) {
    revoke_callers(*c);
    emit_contents_event(c, BDK_CONTENTS_EVENT_RENDERER_GONE, shown(*c, page_committed_url(*c)),
                        std::string("{\"reason\":\"") + reason + "\"}", 0, kMainFrame);
}

} // namespace bdkstub

namespace {

/* scheme://host[:port] of a URL, or the URL itself when it has no authority. */
std::string origin_of(const std::string& url) {
    const size_t start = url.find("://");
    if (start == std::string::npos)
        return url;
    return url.substr(0, url.find_first_of("/?#", start + 3));
}

std::string page_origin(const Contents& c) {
    return origin_of(bdkstub::page_committed_url(c));
}

std::string popup_site(const Contents& c) {
    auto profile = c.profile;
    return (profile ? profile->id : std::string()) + " " + page_origin(c);
}

/* A page health request the stub raises for the page (ui-requests.md 5.19), reporting its outcome
   like the requests a test raises. */
std::shared_ptr<Request> raise_page_health(
    const std::shared_ptr<Contents>& c, const std::string& payload,
    std::function<void(UiOutcome, const std::string&, const std::string&)> on_finish) {
    auto rt = c->rt.lock();
    if (!rt)
        return nullptr;
    auto r = std::make_shared<Request>();
    r->surface = BDK_SURFACE_PAGE_HEALTH;
    r->contents = c->handle;
    r->payload = payload;
    r->on_finish = std::move(on_finish);
    bdkstub::track_request(r);
    raise_ui_request(rt, r);
    return r;
}

/* The page opens a blocked popup: a new-window request, as a popup the blocker let through. */
void open_popup(const std::shared_ptr<Contents>& opener, const std::string& url) {
    auto rt = opener->rt.lock();
    if (!rt)
        return;
    auto r = std::make_shared<Request>();
    r->surface = BDK_SURFACE_NEW_WINDOWS;
    r->contents = opener->handle;
    r->payload = "{\"kind\":\"popup\",\"disposition\":\"popup\",\"targetURL\":\"" +
                 json_escape(url) + "\",\"opener\":\"" + std::to_string(opener->handle) +
                 "\",\"userGesture\":true}";
    bdkstub::track_request(r);
    raise_ui_request(rt, r);
}

/* Raises the blocked-popup notice for the page's list, withdrawing the one it replaces. */
void raise_blocked_notice(const std::shared_ptr<Contents>& c) {
    StubContents& d = data_of(*c);
    if (auto open = d.blocked_notice.lock())
        withdraw_ui_request(*open);
    std::string popups;
    for (const auto& [id, url] : d.blocked_popups)
        popups += std::string(popups.empty() ? "" : ",") + "{\"id\":\"" + id + "\",\"url\":\"" +
                  json_escape(url) + "\"}";
    std::weak_ptr<Contents> weak = c;
    d.blocked_notice = raise_page_health(
        c,
        "{\"kind\":\"blockedPopups\",\"origin\":\"" + json_escape(page_origin(*c)) +
            "\",\"popups\":[" + popups + "]}",
        [weak](UiOutcome outcome, const std::string& completion, const std::string& args) {
            auto page = weak.lock();
            if (!page || outcome == UiOutcome::kWithdrawn)
                return;
            StubContents& pd = data_of(*page);
            if (outcome == UiOutcome::kCompleted && completion == "grant") {
                if (args.find("\"always\":true") != std::string::npos) {
                    S().popups_allowed.insert(popup_site(*page));
                    return;
                }
                for (auto it = pd.blocked_popups.begin(); it != pd.blocked_popups.end(); ++it) {
                    if (args.find("\"popup\":\"" + it->first + "\"") != std::string::npos) {
                        const std::string url = it->second;
                        pd.blocked_popups.erase(it);
                        open_popup(page, url);
                        return;
                    }
                }
                return;
            }
            /* Dismissed, rejected, dropped or shown by the default presenter: the list is
               answered, and the next blocked popup starts a new one. */
            pd.blocked_popups.clear();
        });
}

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
            if (S().queue.empty()) {
                if (S().delayed.empty())
                    break;
                auto first = S().delayed.begin();
                S().now_ms = std::max(S().now_ms, first->first.first);
                S().queue.push_back(std::move(first->second));
                S().delayed.erase(first);
            }
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

BDK_EXPORT bdk_status_t bdk_stub_script_redirect(bdk_runtime_t h, bdk_string_t url,
                                                 bdk_string_t to) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(url) || !string_ok(to) || to.length == 0)
        return BDK_ERR_INVALID_ARGUMENT;
    S().redirects[str(url)] = str(to);
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
    bdkstub::track_request(r);
    raise_ui_request(rt, r);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_page_unresponsive(bdk_contents_t h) {
    if (bdk_status_t s = check_thread())
        return s;
    auto c = contents_of(h);
    if (!c)
        return BDK_ERR_STALE_HANDLE;
    StubContents& d = data_of(*c);
    if (d.unresponsive)
        return BDK_OK; /* one question per hang */
    d.unresponsive = true;
    std::string title;
    if (d.current >= 0)
        title = d.entries[static_cast<size_t>(d.current)].title;
    std::weak_ptr<Contents> weak = c;
    raise_page_health(
        c,
        "{\"kind\":\"unresponsive\",\"origin\":\"" + json_escape(page_origin(*c)) +
            "\",\"pageTitle\":\"" + json_escape(title) + "\"}",
        [weak](UiOutcome outcome, const std::string& completion, const std::string& args) {
            auto page = weak.lock();
            if (!page)
                return;
            data_of(*page).unresponsive = false;
            if (outcome == UiOutcome::kCompleted && completion == "accept" &&
                args.find("\"terminate\"") != std::string::npos)
                bdkstub::page_renderer_gone(page, "killed");
        });
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_page_responsive(bdk_contents_t h) {
    if (bdk_status_t s = check_thread())
        return s;
    auto c = contents_of(h);
    if (!c)
        return BDK_ERR_STALE_HANDLE;
    auto rt = c->rt.lock();
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    for (auto& r : std::vector<std::shared_ptr<Request>>(rt->requests)) {
        if (r->contents != h || r->surface != BDK_SURFACE_PAGE_HEALTH || r->finished)
            continue;
        if (r->payload.find("\"kind\":\"unresponsive\"") != std::string::npos)
            withdraw_ui_request(*r);
    }
    data_of(*c).unresponsive = false;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_block_popup(bdk_contents_t h, bdk_string_t url,
                                             uint8_t* out_blocked) {
    if (bdk_status_t s = check_thread())
        return s;
    auto c = contents_of(h);
    if (!c)
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(url) || url.length == 0 || !out_blocked)
        return BDK_ERR_INVALID_ARGUMENT;
    if (S().popups_allowed.count(popup_site(*c))) {
        *out_blocked = 0;
        open_popup(c, str(url));
        return BDK_OK;
    }
    *out_blocked = 1;
    StubContents& d = data_of(*c);
    d.blocked_popups.emplace_back(std::to_string(d.next_popup_id++), str(url));
    raise_blocked_notice(c);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_send_ui_update(bdk_runtime_t h, bdk_string_t update) {
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    auto r = S().last_request.lock();
    bool object = false;
    if (!r || !string_ok(update) || !valid_json(str(update), &object) || !object)
        return BDK_ERR_INVALID_ARGUMENT;
    ui_send_update(rt, r, str(update));
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_last_ui_update(bdk_runtime_t h, char* out, size_t cap) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    if (!out || cap == 0)
        return BDK_ERR_INVALID_ARGUMENT;
    snprintf(out, cap, "%s", S().last_update.c_str());
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_attach_contents(bdk_contents_t c, bdk_window_t w) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!contents_of(c) || (w && !window_of(w)))
        return BDK_ERR_STALE_HANDLE;
    detach(c);
    if (!w)
        return BDK_OK;
    StubState::WindowState& state = S().windows[w];
    state.contents.push_back(c);
    if (!state.active)
        state.active = c;
    if (state.extension_id < 0)
        state.extension_id = S().next_window_id++;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_window_request(bdk_window_t h, bdk_string_t request) {
    if (bdk_status_t s = check_thread())
        return s;
    auto w = window_of(h);
    auto rt = current_runtime();
    if (!w || !rt)
        return BDK_ERR_STALE_HANDLE;
    bool object = false;
    if (!string_ok(request) || !valid_json(str(request), &object) || !object)
        return BDK_ERR_INVALID_ARGUMENT;
    S().window_answer = 0;
    window_request(rt, w, str(request),
                   [](bdk_ui_response_t answer) { S().window_answer = answer; });
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_last_window_answer(uint32_t* out) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    *out = S().window_answer;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_window_anchor(bdk_window_t h, uint32_t role, double out_rect[4],
                                               uint8_t* out_found) {
    if (bdk_status_t s = check_thread())
        return s;
    auto w = window_of(h);
    if (!w)
        return BDK_ERR_STALE_HANDLE;
    if (!out_rect || !out_found)
        return BDK_ERR_INVALID_ARGUMENT;
    *out_found = window_anchor(*w, role, out_rect) ? 1 : 0;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_extension_package(bdk_runtime_t h, bdk_string_t kind,
                                                   bdk_string_t key, bdk_string_t manifest) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(kind) || !string_ok(key) || !string_ok(manifest) || key.length == 0)
        return BDK_ERR_INVALID_ARGUMENT;
    bdkstub::extensions_add_package(str(kind), str(key), str(manifest));
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_menus_presented(uint32_t* out) {
    if (bdk_status_t s = check_thread())
        return s;
    if (!out)
        return BDK_ERR_INVALID_ARGUMENT;
    *out = S().menus_presented;
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
