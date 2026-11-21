/* The default reputation provider's block lists (decision D2): the list format, the expressions a
   URL is looked up by, and the warning a verdict raises. Platform-neutral; the backend downloads a
   list and checks its signature before handing it here. */
#include <algorithm>
#include <cctype>

#include "providers_internal.h"

namespace bdkcore {

namespace {

constexpr const char* kFormat = "bdk-reputation-list";

bool is_verdict(const std::string& v) {
    return v == "malware" || v == "phishing" || v == "unwantedSoftware";
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

/* An expression is a host (lower case, no port, no trailing dot) and a path starting with '/'. */
bool expression_ok(const std::string& e) {
    size_t slash = e.find('/');
    if (slash == std::string::npos || slash == 0)
        return false;
    std::string host = e.substr(0, slash);
    if (host != lower(host) || host.back() == '.' || host.find(':') != std::string::npos ||
        host.find('@') != std::string::npos)
        return false;
    return e.find('#') == std::string::npos &&
           std::none_of(e.begin(), e.end(), [](unsigned char ch) { return ch <= ' '; });
}

bool integer(const JsonValue* v, int64_t* out) {
    if (!v || v->type != JsonValue::Type::kNumber || !v->integral)
        return false;
    *out = static_cast<int64_t>(v->number);
    return true;
}

bool ip_literal(const std::string& host) {
    if (!host.empty() && host.front() == '[')
        return true;
    return !host.empty() && std::all_of(host.begin(), host.end(), [](unsigned char ch) {
        return std::isdigit(ch) || ch == '.';
    });
}

} // namespace

bool reputation_list_parse(const std::string& json, ReputationList* out, std::string* message) {
    JsonValue root;
    if (!valid_json(json) || !json_parse(json, &root) || root.type != JsonValue::Type::kObject) {
        *message = "the list is not a JSON object";
        return false;
    }
    const JsonValue* format = root.find("format");
    int64_t version = 0;
    if (!format || format->type != JsonValue::Type::kString || format->text != kFormat ||
        !integer(root.find("version"), &version) || version != 1) {
        *message = "the list is not a bdk-reputation-list of version 1";
        return false;
    }
    ReputationList list;
    if (!integer(root.find("sequence"), &list.sequence) || list.sequence < 1 ||
        !integer(root.find("generated"), &list.generated) ||
        !integer(root.find("expires"), &list.expires) || list.expires <= list.generated) {
        *message = "the list needs a sequence above 0, and generated before expires";
        return false;
    }
    const JsonValue* source = root.find("source");
    if (!source || source->type != JsonValue::Type::kString) {
        *message = "the list does not name its source";
        return false;
    }
    list.source = source->text;
    const JsonValue* entries = root.find("entries");
    if (!entries || entries->type != JsonValue::Type::kArray) {
        *message = "the list has no entries array";
        return false;
    }
    for (size_t i = 0; i < entries->items.size(); ++i) {
        const JsonValue& e = entries->items[i];
        const JsonValue* expression =
            e.type == JsonValue::Type::kObject ? e.find("expression") : nullptr;
        const JsonValue* verdict = e.type == JsonValue::Type::kObject ? e.find("verdict") : nullptr;
        if (!expression || expression->type != JsonValue::Type::kString ||
            !expression_ok(expression->text) || !verdict ||
            verdict->type != JsonValue::Type::kString || !is_verdict(verdict->text)) {
            *message = "entry " + std::to_string(i) + " is not an expression and a verdict";
            return false;
        }
        if (!list.entries.emplace(expression->text, verdict->text).second) {
            *message = "entry " + std::to_string(i) + " repeats \"" + expression->text + "\"";
            return false;
        }
    }
    *out = std::move(list);
    return true;
}

std::vector<std::string> reputation_expressions(const std::string& url) {
    std::string rest;
    if (url.compare(0, 7, "http://") == 0)
        rest = url.substr(7);
    else if (url.compare(0, 8, "https://") == 0)
        rest = url.substr(8);
    else
        return {};
    rest = rest.substr(0, rest.find('#'));
    size_t end = rest.find_first_of("/?");
    std::string authority = rest.substr(0, end);
    std::string path_and_query = end == std::string::npos ? "/" : rest.substr(end);
    if (path_and_query.front() == '?')
        path_and_query = "/" + path_and_query;
    if (size_t at = authority.rfind('@'); at != std::string::npos)
        authority = authority.substr(at + 1);
    std::string host = authority;
    if (!host.empty() && host.front() == '[') {
        host = host.substr(0, host.find(']') + 1);
    } else if (size_t colon = host.find(':'); colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    host = lower(host);
    while (!host.empty() && host.back() == '.')
        host.pop_back();
    if (host.empty())
        return {};

    std::vector<std::string> hosts = {host};
    if (!ip_literal(host)) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t dot = host.find('.', start);
            parts.push_back(host.substr(start, dot - start));
            if (dot == std::string::npos)
                break;
            start = dot + 1;
        }
        size_t n = parts.size();
        /* The last five components, successively removing the leading one, down to two. */
        for (size_t first = n > 5 ? n - 5 : 1; first + 2 <= n && hosts.size() < 5; ++first) {
            std::string h;
            for (size_t i = first; i < n; ++i)
                h += (i > first ? "." : "") + parts[i];
            if (std::find(hosts.begin(), hosts.end(), h) == hosts.end())
                hosts.push_back(h);
        }
    }

    std::string path = path_and_query.substr(0, path_and_query.find('?'));
    std::vector<std::string> paths = {path_and_query};
    if (path != path_and_query)
        paths.push_back(path);
    /* The root, then up to three more prefixes ending in '/'. */
    std::vector<std::string> prefixes = {"/"};
    for (size_t pos = 1; prefixes.size() < 4;) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos)
            break;
        prefixes.push_back(path.substr(0, slash + 1));
        pos = slash + 1;
    }
    for (const std::string& p : prefixes) {
        if (std::find(paths.begin(), paths.end(), p) == paths.end())
            paths.push_back(p);
    }

    std::vector<std::string> out;
    for (const std::string& h : hosts) {
        for (const std::string& p : paths)
            out.push_back(h + p);
    }
    return out;
}

std::string reputation_list_lookup(const ReputationList& list, const std::string& url,
                                   int64_t now_ms) {
    if (now_ms >= list.expires)
        return "unknown";
    for (const std::string& e : reputation_expressions(url)) {
        auto it = list.entries.find(e);
        if (it != list.entries.end())
            return it->second;
    }
    return "safe";
}

std::string reputation_warning_payload(const Runtime& rt, const std::string& url,
                                       const std::string& verdict) {
    const bool reduced = provider_choice(rt, Service::kReputation).provider == "default";
    std::string message =
        verdict == "phishing"
            ? "This site may try to trick you into giving away personal information."
            : (verdict == "unwantedSoftware"
                   ? "This site may try to install software that changes your computer."
                   : "This site may harm your computer.");
    if (reduced)
        message += " It was found in public block lists, which give reduced protection: an "
                   "unlisted site is not known to be safe.";
    return "{\"url\":\"" + json_escape(url) + "\",\"verdict\":\"" + json_escape(verdict) +
           "\",\"provider\":" + provider_descriptor(rt, Service::kReputation) +
           ",\"isOverridable\":true,\"text\":{\"title\":\"Dangerous site\",\"message\":\"" +
           json_escape(message) + "\"}}";
}

} // namespace bdkcore
