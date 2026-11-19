/* The stub's built-in commands, in memory. Chosen so the conformance suite can show every part of
   the broker with real commands: reads and writes (settings, history, bookmarks, permissions,
   passwords, addresses), pagination (history.search), progress and a long-running call
   (privacy.clear), and secrets that need authentication (passwords.reveal). Data is per profile id,
   and lives until the runtime stops. The rest of the catalogue answers broker.notImplemented. */
#include "stub_services.h"

#include <algorithm>
#include <map>
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

/* One profile's data. */
struct Store {
    std::map<std::string, std::string> settings; /* key -> JSON */
    std::vector<HistoryRow> history;
    std::vector<Bookmark> bookmarks;
    std::map<std::string, std::string> permissions; /* "origin kind" -> value */
    std::map<std::string, std::string> permission_defaults;
    std::vector<Password> passwords;
    std::vector<Address> addresses;
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
        c.out = "null";
        return true;
    }
    if (name == "history.search") {
        std::string text = c.text("text");
        size_t limit = c.arg("limit") ? static_cast<size_t>(c.arg("limit")->number) : 100;
        size_t offset = 0;
        if (c.arg("cursor")) {
            const std::string& cursor = c.arg("cursor")->text;
            if (cursor.empty() || cursor.find_first_not_of("0123456789") != std::string::npos)
                return c.fail(BDK_CODE_INVALID_ARGUMENTS,
                              "the cursor is not one this call returned");
            offset = static_cast<size_t>(std::stoull(cursor));
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
        for (auto& u : c.arg("urls")->items) {
            s.history.erase(std::remove_if(s.history.begin(), s.history.end(),
                                           [&](const HistoryRow& h) { return h.url == u.text; }),
                            s.history.end());
        }
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
}

std::map<uint64_t, std::shared_ptr<Caller>>& callers() {
    return st().callers;
}

Controls& controls() {
    return st().controls;
}

void services_execute(Runtime& rt, const std::shared_ptr<BuiltinCall>& call) {
    State& s = st();
    ++s.controls.executed;
    if (s.controls.hold) {
        ++s.controls.held;
        s.held.push_back(call->control);
        call->control->OnCancel([] { ++st().controls.stopped; });
        return;
    }
    /* Every command reports progress; the core drops it unless the command is marked P. */
    call->control->Progress(0.1, "starting");
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
