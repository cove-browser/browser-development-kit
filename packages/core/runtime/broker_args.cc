/* Broker: parsing JSON arguments and checking them against the types of the interface definitions.
   Nothing here depends on a backend, so hosted CI tests it without Chromium. */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string_view>

#include "bdk_command_table.inc"
#include "broker_internal.h"

namespace bdkcore {
namespace {

constexpr int kMaxDepth = 64;
/* 2^53: the largest integer a double holds exactly, so the largest a JSON number can carry
   everywhere. */
constexpr double kMaxExactInteger = 9007199254740992.0;

/* UTF-8 for a code point, by arithmetic only. */
void append_utf8(std::string& out, uint32_t cp) {
    auto byte = [&out](uint32_t value) { out += static_cast<char>(value); };
    if (cp < 128u) {
        byte(cp);
    } else if (cp < 2048u) {
        byte(192u + cp / 64u);
        byte(128u + cp % 64u);
    } else if (cp < 65536u) {
        byte(224u + cp / 4096u);
        byte(128u + cp / 64u % 64u);
        byte(128u + cp % 64u);
    } else {
        byte(240u + cp / 262144u);
        byte(128u + cp / 4096u % 64u);
        byte(128u + cp / 64u % 64u);
        byte(128u + cp % 64u);
    }
}

/* Concatenates without the temporaries a chain of + makes. */
std::string join(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (std::string_view part : parts)
        out.append(part);
    return out;
}

/* Parses text that is already known to be JSON, so it checks only what it needs to stay in bounds.
 */
struct Parser {
    const std::string& s;
    size_t i = 0;
    explicit Parser(const std::string& text) : s(text) {}

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }

    bool hex4(uint32_t* out) {
        if (i + 4 > s.size())
            return false;
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i];
            ++i;
            uint32_t digit = 0;
            if (c >= '0' && c <= '9')
                digit = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<uint32_t>(c - 'A' + 10);
            else
                return false;
            v = v * 16u + digit;
        }
        *out = v;
        return true;
    }

    bool string(std::string* out) {
        if (i >= s.size() || s[i] != '"')
            return false;
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"')
                return true;
            if (c != '\\') {
                *out += c;
                continue;
            }
            if (i >= s.size())
                return false;
            char e = s[i++];
            switch (e) {
            case 'b':
                *out += '\b';
                break;
            case 'f':
                *out += '\f';
                break;
            case 'n':
                *out += '\n';
                break;
            case 'r':
                *out += '\r';
                break;
            case 't':
                *out += '\t';
                break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(&cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' &&
                    s[i + 1] == 'u') {
                    size_t save = i;
                    i += 2;
                    uint32_t low = 0;
                    if (hex4(&low) && low >= 0xDC00 && low < 0xE000)
                        cp = 65536u + (cp - 55296u) * 1024u + (low - 56320u);
                    else
                        i = save;
                }
                append_utf8(*out, cp);
                break;
            }
            default: /* " \ / */
                *out += e;
            }
        }
        return false;
    }

    bool value(JsonValue* out, int depth) {
        if (depth > kMaxDepth)
            return false;
        ws();
        if (i >= s.size())
            return false;
        char c = s[i];
        if (c == '{') {
            ++i;
            out->type = JsonValue::Type::kObject;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            }
            for (;;) {
                ws();
                std::string key;
                if (!string(&key))
                    return false;
                ws();
                if (i >= s.size() || s[i] != ':')
                    return false;
                ++i;
                JsonValue member;
                if (!value(&member, depth + 1))
                    return false;
                out->members.push_back(JsonMember{std::move(key), std::move(member)});
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++i;
            out->type = JsonValue::Type::kArray;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            for (;;) {
                JsonValue item;
                if (!value(&item, depth + 1))
                    return false;
                out->items.push_back(std::move(item));
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out->type = JsonValue::Type::kString;
            return string(&out->text);
        }
        if (s.compare(i, 4, "true") == 0) {
            i += 4;
            out->type = JsonValue::Type::kBool;
            out->boolean = true;
            return true;
        }
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            out->type = JsonValue::Type::kBool;
            return true;
        }
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            out->type = JsonValue::Type::kNull;
            return true;
        }
        size_t start = i;
        bool integral = true;
        while (i < s.size() && (strchr("+-0123456789.eE", s[i]) != nullptr)) {
            if (s[i] == '.' || s[i] == 'e' || s[i] == 'E')
                integral = false;
            ++i;
        }
        if (i == start)
            return false;
        std::string token = s.substr(start, i - start);
        out->type = JsonValue::Type::kNumber;
        out->number = strtod(token.c_str(), nullptr);
        out->integral = integral;
        return true;
    }
};

using table::Field;
using table::Kind;
using table::Prim;
using table::TypeRef;

std::string describe(const TypeRef& t);

std::string describe_prim(Prim p) {
    switch (p) {
    case Prim::kString:
        return "a string";
    case Prim::kUrl:
        return "a URL string";
    case Prim::kBytes:
        return "a base64 string";
    case Prim::kInt:
        return "an integer";
    case Prim::kTimestamp:
        return "an integer number of milliseconds since 1970-01-01T00:00:00Z";
    case Prim::kDouble:
        return "a number";
    case Prim::kBool:
        return "true or false";
    case Prim::kJson:
        return "a JSON value";
    default:
        return "a value";
    }
}

std::string describe(const TypeRef& t) {
    std::string base;
    if (t.prim == Prim::kNamed) {
        const table::TypeDef& def = table::kTypes[t.type];
        base = def.kind == Kind::kEnum     ? join({"one of the ", def.name, " values"})
               : def.kind == Kind::kStruct ? join({"a ", def.name, " object"})
                                           : describe(def.alias);
    } else {
        base = describe_prim(t.prim);
    }
    return t.array ? join({"an array of ", base}) : base;
}

std::string check_value(const TypeRef& type, const JsonValue& v, const std::string& path,
                        bool element);

std::string mismatch(const TypeRef& type, const std::string& path) {
    return join({path, ": expected ", describe(type)});
}

std::string check_struct(const table::TypeDef& def, const JsonValue& v, const std::string& path) {
    if (v.type != JsonValue::Type::kObject)
        return join({path, ": expected a ", def.name, " object"});
    for (auto& [key, member] : v.members) {
        bool known = false;
        for (size_t k = 0; k < def.field_count; ++k)
            known = known || key == def.fields[k].name;
        if (!known)
            return join({path, ".", key, ": unknown field"});
    }
    for (size_t k = 0; k < def.field_count; ++k) {
        const Field& f = def.fields[k];
        const JsonValue* m = v.find(f.name);
        if (!m) {
            if (!f.type.optional)
                return join({path, ".", f.name, ": is required"});
            continue;
        }
        std::string problem = check_value(f.type, *m, join({path, ".", f.name}), false);
        if (!problem.empty())
            return problem;
    }
    return std::string();
}

std::string check_scalar(const TypeRef& type, const JsonValue& v, const std::string& path) {
    using T = JsonValue::Type;
    switch (type.prim) {
    case Prim::kString:
    case Prim::kUrl:
    case Prim::kBytes:
        return v.type == T::kString ? std::string() : mismatch(type, path);
    case Prim::kInt:
    case Prim::kTimestamp:
        return v.type == T::kNumber && v.integral && std::fabs(v.number) <= kMaxExactInteger
                   ? std::string()
                   : mismatch(type, path);
    case Prim::kDouble:
        return v.type == T::kNumber && std::isfinite(v.number) ? std::string()
                                                               : mismatch(type, path);
    case Prim::kBool:
        return v.type == T::kBool ? std::string() : mismatch(type, path);
    case Prim::kJson:
        return std::string();
    case Prim::kNamed: {
        const table::TypeDef& def = table::kTypes[type.type];
        if (def.kind == Kind::kStruct)
            return check_struct(def, v, path);
        if (def.kind == Kind::kEnum) {
            if (v.type == T::kString) {
                for (size_t k = 0; k < def.value_count; ++k) {
                    if (v.text == def.values[k])
                        return std::string();
                }
            }
            return mismatch(type, path);
        }
        TypeRef inner = def.alias;
        return check_value(inner, v, path, false);
    }
    case Prim::kVoid:
        break;
    }
    return std::string();
}

/* element: v is an element of an array, so an array type's [] has been consumed. */
std::string check_value(const TypeRef& type, const JsonValue& v, const std::string& path,
                        bool element) {
    if (v.type == JsonValue::Type::kNull && type.optional && !element)
        return std::string();
    if (type.array && !element) {
        if (v.type != JsonValue::Type::kArray)
            return mismatch(type, path);
        TypeRef item = type;
        item.array = false;
        item.optional = false;
        for (size_t k = 0; k < v.items.size(); ++k) {
            std::string problem =
                check_value(item, v.items[k], join({path, "[", std::to_string(k), "]"}), true);
            if (!problem.empty())
                return problem;
        }
        return std::string();
    }
    TypeRef scalar = type;
    scalar.array = false;
    return check_scalar(scalar, v, path);
}

} // namespace

const JsonValue* JsonValue::find(const std::string& key) const {
    for (auto& [k, v] : members) {
        if (k == key)
            return &v;
    }
    return nullptr;
}

bool json_parse(const std::string& text, JsonValue* out) {
    Parser p(text);
    *out = JsonValue();
    if (!p.value(out, 0))
        return false;
    p.ws();
    return p.i == text.size();
}

std::string json_serialize(const JsonValue& v) {
    using T = JsonValue::Type;
    switch (v.type) {
    case T::kNull:
        return "null";
    case T::kBool:
        return v.boolean ? "true" : "false";
    case T::kNumber: {
        char buf[40];
        if (v.integral && std::fabs(v.number) <= kMaxExactInteger)
            snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.number));
        else
            snprintf(buf, sizeof buf, "%.17g", v.number);
        return buf;
    }
    case T::kString:
        return "\"" + json_escape(v.text) + "\"";
    case T::kArray: {
        std::string out = "[";
        for (size_t i = 0; i < v.items.size(); ++i)
            out += (i ? "," : "") + json_serialize(v.items[i]);
        return out + "]";
    }
    case T::kObject: {
        std::string out = "{";
        for (size_t i = 0; i < v.members.size(); ++i) {
            out += (i ? ",\"" : "\"") + json_escape(v.members[i].first) +
                   "\":" + json_serialize(v.members[i].second);
        }
        return out + "}";
    }
    }
    return "null";
}

std::string check_arguments(const table::CommandDef& def, const JsonValue& arguments) {
    if (arguments.type != JsonValue::Type::kObject)
        return "arguments: expected an object";
    for (auto& [key, member] : arguments.members) {
        bool known = false;
        for (size_t k = 0; k < def.arg_count; ++k)
            known = known || key == def.args[k].name;
        if (!known)
            return join({"arguments.", key, ": unknown field"});
    }
    for (size_t k = 0; k < def.arg_count; ++k) {
        const Field& f = def.args[k];
        const JsonValue* m = arguments.find(f.name);
        if (!m) {
            if (!f.type.optional)
                return join({"arguments.", f.name, ": is required"});
            continue;
        }
        std::string problem = check_value(f.type, *m, join({"arguments.", f.name}), false);
        if (!problem.empty())
            return problem;
    }
    if ((def.flags & table::kPaginated) != 0) {
        const JsonValue* limit = arguments.find("limit");
        if (limit && limit->type == JsonValue::Type::kNumber && limit->number < 1)
            return "arguments.limit: must be at least 1";
    }
    return std::string();
}

} // namespace bdkcore
