/* The stub's built-in commands, in memory. Chosen so the conformance suite can show every part of
   the broker with real commands: reads and writes (settings, history, bookmarks, permissions,
   passwords, addresses), pagination (history.search), progress and a long-running call
   (privacy.clear), and secrets that need authentication (passwords.reveal). Data is per profile id,
   and lives until the runtime stops. The rest of the catalogue answers broker.notImplemented. */
#include "stub_services.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <map>
#include <system_error>
#include <vector>

using namespace bdkcore;

namespace bdkstub {
namespace {

constexpr int64_t kEpoch = 1700000000000; /* 2023-11-14T22:13:20Z */

struct HistoryRow {
    std::string url, title;
    int64_t last_visit = 0;
    int visits = 0, typed = 0;
};
struct Bookmark {
    std::string id, parent, title, url;
    bool folder = false;
    int64_t added = 0;
};
struct Password {
    std::string id, origin, username, password;
    int64_t created = 0;
};
struct Address {
    std::string id, fields_json;
};
struct Engine {
    std::string id, name, keyword, search_url, suggest_url, favicon_url; /* empty: not set */
};

/* One profile's data. */
struct Store {
    std::map<std::string, std::string> settings; /* key -> JSON */
    std::vector<HistoryRow> history;
    std::vector<Bookmark> bookmarks;
    std::map<std::string, std::string> permissions; /* "origin kind" -> value */
    std::map<std::string, std::string> permission_defaults;
    std::vector<Password> passwords;
    std::vector<Address> addresses;
    std::vector<Engine> engines; /* none until the application or the user adds one */
    std::string default_engine;  /* an id in engines, or empty */
};

struct State {
    std::map<std::string, Store> stores;
    int64_t clock = kEpoch;
    int next_id = 1;
    Controls controls;
    std::vector<std::shared_ptr<CommandControl>> held;
    std::function<void(bool)> waiting_authentication;
    std::map<uint64_t, std::shared_ptr<Caller>> callers;
};

State& st() {
    static State* s = new State;
    return *s;
}

int64_t tick() {
    st().clock += 1000;
    return st().clock;
}

std::string q(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

std::string next_id(const char* prefix) {
    return prefix + std::to_string(st().next_id++);
}

/* What a handler has to work with. */
struct Ctx {
    Runtime& rt;
    const BuiltinCall& call;
    JsonValue args;
    Store& store;
    std::string out; /* the result JSON */
    Err err;         /* set instead of out to fail */
    bool failed = false;
    bool answered_later = false;

    const JsonValue* arg(const char* name) const {
        const JsonValue* v = args.find(name);
        return v && v->type != JsonValue::Type::kNull ? v : nullptr;
    }
    std::string text(const char* name, const std::string& fallback = std::string()) const {
        const JsonValue* v = arg(name);
        return v ? v->text : fallback;
    }
    bool fail(const char* code, const std::string& message) {
        failed = true;
        err = Err{BDK_DOMAIN_BROKER, code, message, "{}"};
        return false;
    }
};

std::string node_json(const Bookmark& b) {
    return "{\"id\":" + q(b.id) + ",\"parentId\":" + (b.parent.empty() ? "null" : q(b.parent)) +
           ",\"index\":0,\"title\":" + q(b.title) + ",\"url\":" + (b.folder ? "null" : q(b.url)) +
           ",\"isFolder\":" + (b.folder ? "true" : "false") +
           ",\"dateAdded\":" + std::to_string(b.added) + "}";
}

Bookmark* find_bookmark(Store& s, const std::string& id) {
    for (auto& b : s.bookmarks) {
        if (b.id == id)
            return &b;
    }
    return nullptr;
}

void ensure_roots(Store& s) {
    if (!s.bookmarks.empty())
        return;
    s.bookmarks.push_back(Bookmark{"bar", "", "Bookmarks bar", "", true, kEpoch});
    s.bookmarks.push_back(Bookmark{"other", "", "Other bookmarks", "", true, kEpoch});
}

std::string history_json(const HistoryRow& h) {
    return "{\"url\":" + q(h.url) + ",\"title\":" + q(h.title) +
           ",\"lastVisit\":" + std::to_string(h.last_visit) +
           ",\"visitCount\":" + std::to_string(h.visits) +
           ",\"typedCount\":" + std::to_string(h.typed) + "}";
}

bool in_range(const JsonValue* range, int64_t t) {
    if (!range)
        return true;
    const JsonValue* start = range->find("start");
    const JsonValue* end = range->find("end");
    if (start && start->type == JsonValue::Type::kNumber && static_cast<double>(t) < start->number)
        return false;
    if (end && end->type == JsonValue::Type::kNumber && static_cast<double>(t) > end->number)
        return false;
    return true;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

void erase_history_url(Store& s, const std::string& url) {
    s.history.erase(std::remove_if(s.history.begin(), s.history.end(),
                                   [&](const HistoryRow& h) { return h.url == url; }),
                    s.history.end());
}

/* ---- Search ---- */

std::string lower(std::string s) {
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool has_space(const std::string& s) {
    return std::any_of(s.begin(), s.end(),
                       [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; });
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool is_http_url(const std::string& u) {
    std::string l = lower(u.substr(0, 8));
    return starts_with(l, "http:") || starts_with(l, "https:");
}

const Engine* find_engine(const Store& s, const std::string& id) {
    for (auto& e : s.engines) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

std::string engine_json(const Store& s, const Engine& e) {
    return "{\"id\":" + q(e.id) + ",\"name\":" + q(e.name) + ",\"keyword\":" + q(e.keyword) +
           ",\"searchUrl\":" + q(e.search_url) +
           ",\"suggestUrl\":" + (e.suggest_url.empty() ? "null" : q(e.suggest_url)) +
           ",\"faviconUrl\":" + (e.favicon_url.empty() ? "null" : q(e.favicon_url)) +
           ",\"isDefault\":" + (s.default_engine == e.id ? "true" : "false") +
           ",\"source\":\"user\"}";
}

std::string engines_json(const Store& s) {
    std::string out = "[";
    for (size_t i = 0; i < s.engines.size(); ++i)
        out += (i ? "," : "") + engine_json(s, s.engines[i]);
    return out + "]";
}

/* The terms as a URL query component: everything but unreserved characters is percent-encoded,
   so a space is %20 (not +, which only form encoding reads as a space). */
std::string encode_terms(const std::string& terms) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : terms) {
        if (std::isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
            out += static_cast<char>(ch);
        } else {
            out += '%';
            out += hex[ch / 16];
            out += hex[ch % 16];
        }
    }
    return out;
}

std::string search_url_for(const Engine& e, const std::string& terms) {
    std::string url = e.search_url;
    const std::string marker = "{searchTerms}";
    std::string encoded = encode_terms(terms);
    for (size_t at = url.find(marker); at != std::string::npos; at = url.find(marker, at)) {
        url.replace(at, marker.size(), encoded);
        at += encoded.size();
    }
    return url;
}

struct Classified {
    bool is_url = false;
    std::string url;       /* what to load; empty for a search with no default engine */
    std::string engine_id; /* the engine a search uses */
    bool keyword = false;  /* the engine was chosen by its keyword */
};

/* `text` is "scheme:" followed by something that is not a space (about:blank, conf://settings). */
bool has_scheme(const std::string& t) {
    size_t colon = t.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= t.size() || has_space(t) ||
        !std::isalpha(static_cast<unsigned char>(t[0])))
        return false;
    for (size_t i = 0; i < colon; ++i) {
        char ch = t[i];
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '+' && ch != '-' && ch != '.')
            return false;
    }
    return true;
}

/* A host, optionally with a port and a path: no spaces, and a dot or the name localhost. */
bool looks_like_host(const std::string& t, bool* is_localhost) {
    if (t.empty() || has_space(t))
        return false;
    std::string host = t.substr(0, t.find_first_of("/?#"));
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        std::string port = host.substr(colon + 1);
        if (port.empty() || !std::all_of(port.begin(), port.end(), [](char ch) {
                return std::isdigit(static_cast<unsigned char>(ch)) != 0;
            }))
            return false;
        host.resize(colon);
    }
    if (host.empty() || host.front() == '.' || host.back() == '.')
        return false;
    for (char ch : host) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '.' && ch != '_')
            return false;
    }
    *is_localhost = lower(host) == "localhost";
    return *is_localhost || host.find('.') != std::string::npos;
}

/* Tells a URL from a query. The stub's rules are a stand-in: the engine uses Chromium's own
   classifier. In order: a keyword and terms; a scheme; a host (localhost gets http://, any other
   host https://); anything else is a search with the default engine, if there is one. */
Classified classify(const Store& s, const std::string& text) {
    Classified r;
    size_t space = text.find(' ');
    if (space != std::string::npos) {
        std::string rest = trim(text.substr(space + 1));
        for (auto& e : s.engines) {
            if (!rest.empty() && lower(e.keyword) == lower(text.substr(0, space))) {
                r.url = search_url_for(e, rest);
                r.engine_id = e.id;
                r.keyword = true;
                return r;
            }
        }
    }
    bool localhost = false;
    /* host:port looks like a scheme, and is a host. */
    if (looks_like_host(text, &localhost)) {
        r.is_url = true;
        r.url = (localhost ? "http://" : "https://") + text;
        return r;
    }
    if (has_scheme(text)) {
        r.is_url = true;
        r.url = text;
        return r;
    }
    if (const Engine* e = find_engine(s, s.default_engine)) {
        r.url = search_url_for(*e, text);
        r.engine_id = e->id;
    }
    return r;
}

/* The URL without its scheme and a leading www., lower case: what typed text is completed against.
 */
std::string completion_base(const std::string& url) {
    std::string u = lower(url);
    size_t scheme = u.find("://");
    if (scheme != std::string::npos)
        u.erase(0, scheme + 3);
    if (starts_with(u, "www."))
        u.erase(0, 4);
    return u;
}

struct Suggestion {
    const char* type;
    std::string text, url, inline_completion;
    int relevance;
};

std::string suggestion_json(const Suggestion& g) {
    return std::string("{\"type\":\"") + g.type + "\",\"text\":" + q(g.text) +
           ",\"url\":" + q(g.url) + ",\"relevance\":" + std::to_string(g.relevance) +
           ",\"inlineCompletion\":" +
           (g.inline_completion.empty() ? "null" : q(g.inline_completion)) + "}";
}

/* Suggestions come only from the profile's own data: nothing goes to the network. */
std::vector<Suggestion> suggest(const Store& s, const std::string& text) {
    std::vector<Suggestion> out;
    if (text.empty())
        return out;
    std::string needle = lower(text);
    std::string base = completion_base(text);
    auto completion = [&](const std::string& url) {
        std::string b = completion_base(url);
        return starts_with(b, base) && b.size() > base.size() ? b.substr(base.size()) : "";
    };
    auto matches = [&](const std::string& a, const std::string& b) {
        return contains(lower(a), needle) || contains(lower(b), needle);
    };
    Classified c = classify(s, text);
    if (c.is_url)
        out.push_back({"url", text, c.url, "", 1300});
    else if (c.keyword)
        out.push_back({"keyword", text, c.url, "", 1200});
    for (auto& b : s.bookmarks) {
        if (!b.folder && matches(b.title, b.url))
            out.push_back({"bookmark", b.title, b.url, completion(b.url), 1100});
    }
    for (auto& h : s.history) {
        if (matches(h.title, h.url))
            out.push_back(
                {"history", h.title, h.url, completion(h.url), 900 + std::min(h.visits, 99)});
    }
    if (const Engine* e = find_engine(s, s.default_engine); e && !c.keyword)
        out.push_back({"search", text, search_url_for(*e, text), "", 800});
    std::stable_sort(out.begin(), out.end(), [](const Suggestion& a, const Suggestion& b) {
        return a.relevance > b.relevance;
    });
    return out;
}

/* The checks addEngine and updateEngine share, on the values the engine will have. */
bool check_engine(Ctx& c, const Engine& e) {
    if (trim(e.name).empty())
        return c.fail(BDK_CODE_INVALID_ARGUMENTS, "'name' must not be empty");
    if (trim(e.keyword).empty() || has_space(e.keyword))
        return c.fail(BDK_CODE_INVALID_ARGUMENTS, "'keyword' must be one word and not empty");
    for (auto& other : c.store.engines) {
        if (other.id != e.id && lower(other.keyword) == lower(e.keyword))
            return c.fail(BDK_CODE_INVALID_ARGUMENTS,
                          "'keyword' is already used by engine '" + other.id + "'");
    }
    if (!is_http_url(e.search_url) || !contains(e.search_url, "{searchTerms}"))
        return c.fail(BDK_CODE_INVALID_ARGUMENTS,
                      "'searchUrl' must start with http: or https: and contain {searchTerms}");
    if (!e.suggest_url.empty() && !is_http_url(e.suggest_url))
        return c.fail(BDK_CODE_INVALID_ARGUMENTS, "'suggestUrl' must start with http: or https:");
    return true;
}

bool run_search(Ctx& c) {
    const std::string& name = c.call.name;
    Store& s = c.store;
    auto announce = [&] {
        emit_builtin_event(current_runtime(), "search.enginesChanged", c.call.profile->handle,
                           "{\"engines\":" + engines_json(s) + ",\"source\":\"application\"}");
    };
    auto find = [&](const std::string& id) -> Engine* {
        for (auto& e : s.engines) {
            if (e.id == id)
                return &e;
        }
        c.fail(BDK_CODE_INVALID_ARGUMENTS, "no search engine '" + id + "'");
        return nullptr;
    };

    if (name == "search.engines") {
        c.out = "{\"engines\":" + engines_json(s) + "}";
        return true;
    }
    if (name == "search.addEngine") {
        Engine e{next_id("e"),        c.text("name"),       c.text("keyword"),
                 c.text("searchUrl"), c.text("suggestUrl"), c.text("faviconUrl")};
        if (!check_engine(c, e))
            return false;
        s.engines.push_back(e);
        c.out = engine_json(s, e);
        announce();
        return true;
    }
    if (name == "search.updateEngine") {
        Engine* e = find(c.text("id"));
        if (!e)
            return false;
        Engine changed = *e;
        if (c.arg("name"))
            changed.name = c.text("name");
        if (c.arg("keyword"))
            changed.keyword = c.text("keyword");
        if (c.arg("searchUrl"))
            changed.search_url = c.text("searchUrl");
        if (c.arg("suggestUrl"))
            changed.suggest_url = c.text("suggestUrl");
        if (c.arg("faviconUrl"))
            changed.favicon_url = c.text("faviconUrl");
        if (!check_engine(c, changed))
            return false;
        *e = std::move(changed);
        c.out = engine_json(s, *e);
        announce();
        return true;
    }
    if (name == "search.removeEngine") {
        Engine* e = find(c.text("id"));
        if (!e)
            return false;
        if (s.default_engine == e->id)
            return c.fail(BDK_CODE_INVALID_ARGUMENTS,
                          "the default engine cannot be removed; make another the default first");
        s.engines.erase(s.engines.begin() + (e - s.engines.data()));
        c.out = "null";
        announce();
        return true;
    }
    if (name == "search.setDefault") {
        Engine* e = find(c.text("id"));
        if (!e)
            return false;
        s.default_engine = e->id;
        c.out = "null";
        announce();
        return true;
    }
    if (name == "search.classify") {
        std::string text = trim(c.text("text"));
        if (text.empty())
            return c.fail(BDK_CODE_INVALID_ARGUMENTS, "'text' must not be empty");
        Classified r = classify(s, text);
        c.out = std::string("{\"kind\":\"") + (r.is_url ? "url" : "search") +
                "\",\"url\":" + (r.url.empty() ? "null" : q(r.url)) +
                ",\"engineId\":" + (r.engine_id.empty() ? "null" : q(r.engine_id)) + "}";
        return true;
    }
    if (name == "search.suggestions") {
        int64_t limit = c.arg("limit") ? static_cast<int64_t>(c.arg("limit")->number) : 8;
        if (limit < 1)
            return c.fail(BDK_CODE_INVALID_ARGUMENTS, "'limit' must be at least 1");
        std::vector<Suggestion> all = suggest(s, trim(c.text("text")));
        c.out = "{\"suggestions\":[";
        for (size_t i = 0; i < all.size() && static_cast<int64_t>(i) < limit; ++i)
            c.out += (i ? "," : "") + suggestion_json(all[i]);
        c.out += "]}";
        return true;
    }
    /* search.deleteSuggestion */
    erase_history_url(s, c.text("url"));
    c.out = "null";
    return true;
}

bool run(Ctx& c) {
    const std::string& name = c.call.name;
    Store& s = c.store;

    if (name == "runtime.info") {
        c.out =
            "{\"bdkVersion\":\"0.0.0-stub\",\"chromiumVersion\":\"\",\"browserName\":\"BDKStub\","
            "\"internalScheme\":\"bdk\",\"platform\":\"stub\"}";
        return true;
    }

    if (name == "profiles.list") {
        c.out = "{\"profiles\":[";
        bool first = true;
        for (auto& p : c.rt.profiles) {
            if (p->ephemeral)
                continue;
            c.out += (first ? "{\"id\":" : ",{\"id\":") + q(p->id) + ",\"name\":" + q(p->name) +
                     ",\"isPrivate\":false}";
            first = false;
        }
        c.out += "]}";
        return true;
    }
    if (name == "profiles.current") {
        const Profile& p = *c.call.profile;
        c.out = "{\"id\":" + q(p.id) + ",\"name\":" + q(p.name) +
                ",\"isPrivate\":" + (p.ephemeral ? "true" : "false") + "}";
        return true;
    }

    if (name == "settings.get") {
        auto it = s.settings.find(c.text("key"));
        c.out = "{\"value\":" + (it == s.settings.end() ? std::string("null") : it->second) + "}";
        return true;
    }
    if (name == "settings.getMany") {
        c.out = "{\"values\":{";
        bool first = true;
        for (auto& key : c.arg("keys")->items) {
            auto it = s.settings.find(key.text);
            c.out += (first ? "" : ",") + q(key.text) + ":" +
                     (it == s.settings.end() ? std::string("null") : it->second);
            first = false;
        }
        c.out += "}}";
        return true;
    }
    if (name == "settings.set") {
        const JsonValue* value = c.args.find("value");
        s.settings[c.text("key")] = json_serialize(*value);
        c.out = "null";
        return true;
    }
    if (name == "settings.reset") {
        s.settings.erase(c.text("key"));
        c.out = "null";
        return true;
    }

    if (name == "history.add") {
        std::string url = c.text("url");
        int64_t when = c.arg("time") ? static_cast<int64_t>(c.arg("time")->number) : tick();
        HistoryRow* row = nullptr;
        for (auto& h : s.history) {
            if (h.url == url)
                row = &h;
        }
        if (!row) {
            s.history.push_back(HistoryRow{url, url, 0, 0, 0});
            row = &s.history.back();
        }
        if (c.arg("title"))
            row->title = c.text("title");
        row->last_visit = std::max(row->last_visit, when);
        ++row->visits;
        if (c.text("transition") == "typed")
            ++row->typed;
        emit_builtin_event(current_runtime(), "history.visited", c.call.profile->handle,
                           "{\"entry\":" + history_json(*row) + ",\"source\":\"application\"}");
        c.out = "null";
        return true;
    }
    if (name == "history.search") {
        std::string text = c.text("text");
        size_t limit = c.arg("limit") ? static_cast<size_t>(c.arg("limit")->number) : 100;
        size_t offset = 0;
        if (c.arg("cursor")) {
            /* Checked: a cursor of more digits than a size_t holds is refused, not thrown on. */
            const std::string& cursor = c.arg("cursor")->text;
            const char* end = cursor.data() + cursor.size();
            auto [last, error] = std::from_chars(cursor.data(), end, offset);
            if (cursor.empty() || error != std::errc() || last != end)
                return c.fail(BDK_CODE_INVALID_ARGUMENTS,
                              "the cursor is not one this call returned");
        }
        std::vector<HistoryRow> rows;
        for (auto& h : s.history) {
            if (in_range(c.arg("range"), h.last_visit) &&
                (contains(h.url, text) || contains(h.title, text)))
                rows.push_back(h);
        }
        std::sort(rows.begin(), rows.end(), [](const HistoryRow& a, const HistoryRow& b) {
            return a.last_visit > b.last_visit;
        });
        c.out = "{\"items\":[";
        size_t end = std::min(rows.size(), offset + limit);
        for (size_t i = offset; i < end; ++i)
            c.out += (i > offset ? "," : "") + history_json(rows[i]);
        c.out += "],\"nextCursor\":" + (end < rows.size() ? q(std::to_string(end)) : "null") + "}";
        return true;
    }
    if (name == "history.visits") {
        c.out = "{\"visits\":[";
        bool first = true;
        for (auto& h : s.history) {
            if (h.url != c.text("url"))
                continue;
            c.out += (first ? "" : ",") + std::string("{\"time\":") + std::to_string(h.last_visit) +
                     ",\"transition\":\"link\",\"referrer\":null}";
            first = false;
        }
        c.out += "]}";
        return true;
    }
    if (name == "history.deleteUrls") {
        for (auto& u : c.arg("urls")->items)
            erase_history_url(s, u.text);
        c.out = "null";
        return true;
    }
    if (name == "history.deleteRange") {
        const JsonValue* range = c.arg("range");
        s.history.erase(
            std::remove_if(s.history.begin(), s.history.end(),
                           [&](const HistoryRow& h) { return in_range(range, h.last_visit); }),
            s.history.end());
        c.out = "null";
        return true;
    }
    if (name == "history.deleteAll") {
        s.history.clear();
        c.out = "null";
        return true;
    }

    if (name == "bookmarks.roots") {
        ensure_roots(s);
        c.out = "{\"roots\":[" + node_json(s.bookmarks[0]) + "," + node_json(s.bookmarks[1]) + "]}";
        return true;
    }
    if (name == "bookmarks.get") {
        ensure_roots(s);
        Bookmark* b = find_bookmark(s, c.text("id"));
        if (!b)
            return c.fail(BDK_CODE_INVALID_ARGUMENTS, "no bookmark '" + c.text("id") + "'");
        c.out = node_json(*b);
        return true;
    }
    if (name == "bookmarks.children") {
        ensure_roots(s);
        if (!find_bookmark(s, c.text("id")))
            return c.fail(BDK_CODE_INVALID_ARGUMENTS, "no bookmark '" + c.text("id") + "'");
        c.out = "{\"items\":[";
        bool first = true;
        for (auto& b : s.bookmarks) {
            if (b.parent != c.text("id"))
                continue;
            c.out += (first ? "" : ",") + node_json(b);
            first = false;
        }
        c.out += "]}";
        return true;
    }
    if (name == "bookmarks.create" || name == "bookmarks.createFolder") {
        ensure_roots(s);
        Bookmark* parent = find_bookmark(s, c.text("parentId"));
        if (!parent || !parent->folder)
            return c.fail(BDK_CODE_INVALID_ARGUMENTS,
                          "'" + c.text("parentId") + "' is not a folder");
        Bookmark b{next_id("b"),
                   parent->id,
                   c.text("title"),
                   c.text("url"),
                   name == "bookmarks.createFolder",
                   tick()};
        s.bookmarks.push_back(b);
        c.out = node_json(b);
        return true;
    }
    if (name == "bookmarks.update") {
        ensure_roots(s);
        Bookmark* b = find_bookmark(s, c.text("id"));
        if (!b || b->parent.empty())
            return c.fail(BDK_CODE_INVALID_ARGUMENTS, "'" + c.text("id") + "' cannot be changed");
        if (c.arg("title"))
            b->title = c.text("title");
        if (c.arg("url")) {
            if (b->folder)
                return c.fail(BDK_CODE_INVALID_ARGUMENTS, "a folder has no URL");
            b->url = c.text("url");
        }
        c.out = node_json(*b);
        return true;
    }
    if (name == "bookmarks.move") {
        /* The stub keeps children in creation order, so `index` is accepted and not applied. */
        ensure_roots(s);
        Bookmark* b = find_bookmark(s, c.text("id"));
        Bookmark* parent = find_bookmark(s, c.text("parentId"));
        if (!b || b->parent.empty())
            return c.fail(BDK_CODE_INVALID_ARGUMENTS, "'" + c.text("id") + "' cannot be moved");
        if (!parent || !parent->folder)
            return c.fail(BDK_CODE_INVALID_ARGUMENTS,
                          "'" + c.text("parentId") + "' is not a folder");
        for (const Bookmark* up = parent; up; up = find_bookmark(s, up->parent)) {
            if (up == b)
                return c.fail(BDK_CODE_INVALID_ARGUMENTS, "a folder cannot move into itself");
        }
        b->parent = parent->id;
        c.out = node_json(*b);
        return true;
    }
    if (name == "bookmarks.remove") {
        ensure_roots(s);
        std::string id = c.text("id");
        if (id == "bar" || id == "other" || !find_bookmark(s, id))
            return c.fail(BDK_CODE_INVALID_ARGUMENTS, "'" + id + "' cannot be removed");
        std::vector<std::string> doomed{id};
        for (size_t i = 0; i < doomed.size(); ++i) {
            for (auto& b : s.bookmarks) {
                if (b.parent == doomed[i])
                    doomed.push_back(b.id);
            }
        }
        s.bookmarks.erase(std::remove_if(s.bookmarks.begin(), s.bookmarks.end(),
                                         [&](const Bookmark& b) {
                                             return std::find(doomed.begin(), doomed.end(), b.id) !=
                                                    doomed.end();
                                         }),
                          s.bookmarks.end());
        c.out = "null";
        return true;
    }
    if (name == "bookmarks.forUrl" || name == "bookmarks.search") {
        ensure_roots(s);
        bool by_url = name == "bookmarks.forUrl";
        std::string needle = c.text(by_url ? "url" : "query");
        c.out = "{\"items\":[";
        bool first = true;
        for (auto& b : s.bookmarks) {
            bool hit = by_url ? (!b.folder && b.url == needle)
                              : (contains(b.title, needle) || contains(b.url, needle));
            if (!hit || b.parent.empty())
                continue;
            c.out += (first ? "" : ",") + node_json(b);
            first = false;
        }
        c.out += "]}";
        return true;
    }

    if (name == "downloads.list") {
        c.out = "{\"items\":[],\"nextCursor\":null}";
        return true;
    }
    if (name == "downloads.get")
        return c.fail(BDK_CODE_INVALID_ARGUMENTS, "no download '" + c.text("id") + "'");

    if (name == "permissions.set") {
        s.permissions[c.text("origin") + " " + c.text("kind")] = c.text("value");
        c.out = "null";
        return true;
    }
    if (name == "permissions.get") {
        auto it = s.permissions.find(c.text("origin") + " " + c.text("kind"));
        bool set = it != s.permissions.end();
        auto def = s.permission_defaults.find(c.text("kind"));
        std::string value = set                                  ? it->second
                            : def != s.permission_defaults.end() ? def->second
                                                                 : "ask";
        c.out = "{\"kind\":" + q(c.text("kind")) + ",\"value\":" + q(value) +
                ",\"source\":" + (set ? "\"user\"" : "\"default\"") + "}";
        return true;
    }
    if (name == "permissions.reset") {
        std::string origin = c.text("origin") + " ";
        for (auto it = s.permissions.begin(); it != s.permissions.end();) {
            bool match = c.arg("kind") ? it->first == origin + c.text("kind")
                                       : it->first.compare(0, origin.size(), origin) == 0;
            it = match ? s.permissions.erase(it) : std::next(it);
        }
        c.out = "null";
        return true;
    }
    if (name == "permissions.setDefault") {
        s.permission_defaults[c.text("kind")] = c.text("value");
        c.out = "null";
        return true;
    }
    if (name == "permissions.getDefault") {
        auto it = s.permission_defaults.find(c.text("kind"));
        c.out = "{\"value\":" + q(it == s.permission_defaults.end() ? "ask" : it->second) + "}";
        return true;
    }

    if (name == "cookies.sites") {
        c.out = "{\"items\":[],\"nextCursor\":null}";
        return true;
    }
    if (name == "privacy.clear") {
        for (auto& type : c.arg("types")->items) {
            if (type.text == "history")
                s.history.clear();
        }
        c.call.control->Progress(0.5, "halfway");
        c.out = "null";
        return true;
    }

    if (name == "passwords.add") {
        Password p{next_id("p"), c.text("origin"), c.text("username"), c.text("password"), tick()};
        s.passwords.push_back(p);
        c.out = "{\"id\":" + q(p.id) + ",\"origin\":" + q(p.origin) +
                ",\"username\":" + q(p.username) + ",\"created\":" + std::to_string(p.created) +
                ",\"lastUsed\":null}";
        return true;
    }
    if (name == "passwords.list" || name == "passwords.forOrigin") {
        c.out = "{\"items\":[";
        bool first = true;
        for (auto& p : s.passwords) {
            if (name == "passwords.forOrigin" && p.origin != c.text("origin"))
                continue;
            c.out += (first ? "{\"id\":" : ",{\"id\":") + q(p.id) + ",\"origin\":" + q(p.origin) +
                     ",\"username\":" + q(p.username) +
                     ",\"created\":" + std::to_string(p.created) + ",\"lastUsed\":null}";
            first = false;
        }
        c.out += "]}";
        return true;
    }
    if (name == "passwords.reveal" || name == "passwords.update" || name == "passwords.remove") {
        auto it = std::find_if(s.passwords.begin(), s.passwords.end(),
                               [&](const Password& p) { return p.id == c.text("id"); });
        if (it == s.passwords.end())
            return c.fail(BDK_CODE_INVALID_ARGUMENTS, "no saved login '" + c.text("id") + "'");
        if (name == "passwords.reveal") {
            c.out = "{\"password\":" + q(it->password) + "}";
        } else if (name == "passwords.remove") {
            s.passwords.erase(it);
            c.out = "null";
        } else {
            if (c.arg("username"))
                it->username = c.text("username");
            if (c.arg("password"))
                it->password = c.text("password");
            c.out = "{\"id\":" + q(it->id) + ",\"origin\":" + q(it->origin) +
                    ",\"username\":" + q(it->username) +
                    ",\"created\":" + std::to_string(it->created) + ",\"lastUsed\":null}";
        }
        return true;
    }

    if (name == "autofill.addresses.add") {
        Address a{next_id("a"), json_serialize(*c.args.find("fields"))};
        s.addresses.push_back(a);
        c.out = "{\"id\":" + q(a.id) + ",\"fields\":" + a.fields_json + "}";
        return true;
    }
    if (name == "autofill.addresses.list") {
        c.out = "{\"items\":[";
        for (size_t i = 0; i < s.addresses.size(); ++i)
            c.out += (i ? "," : "") + std::string("{\"id\":") + q(s.addresses[i].id) +
                     ",\"fields\":" + s.addresses[i].fields_json + "}";
        c.out += "]}";
        return true;
    }
    if (name == "autofill.addresses.remove") {
        s.addresses.erase(std::remove_if(s.addresses.begin(), s.addresses.end(),
                                         [&](const Address& a) { return a.id == c.text("id"); }),
                          s.addresses.end());
        c.out = "null";
        return true;
    }

    if (name.compare(0, 7, "search.") == 0)
        return run_search(c);

    if (name == "system.defaultBrowser.status") {
        c.out = "{\"isDefault\":false,\"canBeDefault\":true}";
        return true;
    }

    return c.fail(BDK_CODE_NOT_IMPLEMENTED, "'" + name + "' is not implemented by the stub");
}

} // namespace

void services_reset() {
    State& s = st();
    s.stores.clear();
    s.clock = kEpoch;
    s.next_id = 1;
    s.controls = Controls{};
    s.held.clear();
    s.waiting_authentication = nullptr;
    s.callers.clear();
    extensions_reset();
    web_apps_reset();
}

std::map<uint64_t, std::shared_ptr<Caller>>& callers() {
    return st().callers;
}

Controls& controls() {
    return st().controls;
}

namespace {

/* The smallest valid PNG (one transparent pixel) and PDF: the stub renders nothing, so its
   captures are fixed bytes with the result shape of interface/contents.json. */
const unsigned char kPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
    0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
    0x9c, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
const char kPdf[] = "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
                    "2 0 obj<</Type/Pages/Kids[]/Count 0>>endobj\n"
                    "trailer<</Root 1 0 R>>\n%%EOF\n";

/* Answers a capture: bytes outside the JSON, or written to the file the caller named. */
void capture(BuiltinCall& call, const std::string& bytes, const char* mime) {
    JsonValue args;
    json_parse(call.arguments, &args);
    const JsonValue* path = args.find("path");
    std::string json = std::string("{\"mimeType\":\"") + mime +
                       "\",\"byteLength\":" + std::to_string(bytes.size());
    if (path && path->type == JsonValue::Type::kString) {
        FILE* f = fopen(path->text.c_str(), "wb");
        bool written = f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        if (f)
            fclose(f);
        if (!written) {
            call.control->Reject(BDK_DOMAIN_PLATFORM, "failed",
                                 "could not write '" + path->text + "'");
            return;
        }
        call.control->Resolve(json + ",\"path\":\"" + json_escape(path->text) + "\"}");
        return;
    }
    call.control->ResolveBinary(json + "}", bytes);
}

/* The contents.* commands the stub can answer without a renderer. */
bool page_command(Runtime&, BuiltinCall& call) {
    const std::string& name = call.name;
    if (name.compare(0, 9, "contents.") != 0 || !call.contents)
        return false;
    ContentsState state;
    backend().State(*call.contents, &state);
    if (name == "contents.frames") {
        call.control->Resolve("{\"frames\":[{\"id\":\"main\",\"parent\":null,\"url\":\"" +
                              json_escape(state.url) + "\",\"origin\":\"\",\"name\":\"\"}]}");
        return true;
    }
    if (name == "contents.text") {
        call.control->Resolve("{\"text\":\"\"}");
        return true;
    }
    if (name == "contents.screenshot") {
        capture(call, std::string(reinterpret_cast<const char*>(kPng), sizeof kPng), "image/png");
        return true;
    }
    if (name == "contents.pdf") {
        call.control->Progress(1.0, "done");
        capture(call, std::string(kPdf, sizeof kPdf - 1), "application/pdf");
        return true;
    }
    return false;
}

} // namespace

void services_execute(Runtime& rt, const std::shared_ptr<BuiltinCall>& call) {
    State& s = st();
    ++s.controls.executed;
    if (s.controls.hold) {
        ++s.controls.held;
        s.held.push_back(call->control);
        call->control->OnCancel([] { ++st().controls.stopped; });
        return;
    }
    if (extensions_execute(rt, call))
        return;
    if (web_apps_execute(rt, call))
        return;
    /* Every command reports progress; the core drops it unless the command is marked P. */
    call->control->Progress(0.1, "starting");
    if (page_command(rt, *call))
        return;
    JsonValue args;
    json_parse(call->arguments, &args);
    Ctx c{rt, *call, std::move(args), s.stores[call->profile ? call->profile->id : std::string()],
          {}, {}};
    if (run(c))
        call->control->Resolve(c.out);
    else
        call->control->Reject(c.err.domain.c_str(), c.err.code.c_str(), c.err.message);
}

void services_release_held() {
    auto held = std::move(st().held);
    st().held.clear();
    for (auto& control : held)
        control->Resolve("null");
}

void services_authenticate(std::function<void(bool)> done) {
    State& s = st();
    ++s.controls.authentications;
    if (s.controls.authentication == 2) {
        s.waiting_authentication = std::move(done);
        return;
    }
    bool granted = s.controls.authentication == 1;
    post([done, granted] { done(granted); });
}

bool services_answer_authentication(bool granted) {
    State& s = st();
    if (!s.waiting_authentication)
        return false;
    auto done = std::move(s.waiting_authentication);
    s.waiting_authentication = nullptr;
    post([done, granted] { done(granted); });
    return true;
}

} // namespace bdkstub
