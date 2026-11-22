/* Automation: sessions, tasks, the wire protocol and the external endpoint (T-AUT-02, T-AUT-04).
   See automation_internal.h and packages/core/docs/conventions.md ("Automation").

   A session is a broker caller (CallerKind::kSession) with the capabilities and scope the
   application chose. Everything a session does arrives as frames on a Connection: from a script
   in the backend's automation host (a task's connection) or from an external tool over the local
   socket. Both speak the same protocol, so the checks are in one place: here, and in the broker. */
#include <algorithm>
#include <charconv>
#include <cstring>
#include <map>
#include <memory>

#include "automation_state.h"
#include "bdk_command_table.inc"
#include "broker_internal.h"
#include "core_internal.h"

namespace bdkcore {

/* ---- One protocol connection ---- */

class Connection : public std::enable_shared_from_this<Connection> {
  public:
    enum class Kind : uint8_t { kTask, kExternal };

    struct EventSub {
        std::string name;
        uint32_t id = 0;
        bdk_profile_t profile = 0;
        bdk_contents_t contents = 0;
    };

    Connection(Kind kind, std::weak_ptr<Runtime> rt) : kind_(kind), rt_(std::move(rt)) {}

    Kind kind() const {
        return kind_;
    }
    bool closed() const {
        return closed_;
    }
    const std::shared_ptr<Session>& session() const {
        return session_;
    }
    const std::map<int64_t, EventSub>& subs() const {
        return subs_;
    }

    void Bind(std::shared_ptr<Session> session) {
        session_ = std::move(session);
    }
    void SetTask(std::weak_ptr<AutomationTask> task) {
        task_ = std::move(task);
    }
    void SetTransport(std::function<bool(const std::string&)> send, std::function<void()> close) {
        send_ = std::move(send);
        close_ = std::move(close);
    }

    void Send(const std::string& frame);
    void Receive(const std::string& frame);
    /* Ends the connection: cancels its calls, ends its subscriptions and, when domain is not null,
       sends a `closed` frame first. */
    void Close(const char* domain, const char* code, const std::string& message);
    void CallFinished(int64_t id) {
        calls_.erase(id);
    }
    /* An event for this connection's subscriptions; the scope was checked. */
    void DeliverEvent(const std::string& name, uint32_t id, bdk_profile_t profile,
                      bdk_contents_t contents, const std::string& payload);

  private:
    void ProtocolError(const std::string& message) {
        Close(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS, message);
    }
    void Hello(const JsonValue& frame);
    void Call(const JsonValue& frame);
    void Subscribe(const JsonValue& frame);
    void TaskFrame(const std::string& type, const JsonValue& frame);
    void SendError(int64_t id, const char* domain, const char* code, const std::string& message);

    Kind kind_;
    std::weak_ptr<Runtime> rt_;
    std::shared_ptr<Session> session_;
    std::weak_ptr<AutomationTask> task_;
    std::function<bool(const std::string&)> send_;
    std::function<void()> close_;
    bool closed_ = false, hello_pending_ = false;
    std::map<int64_t, bdk_call_t> calls_;
    std::map<int64_t, EventSub> subs_;
};

namespace {

std::map<const Runtime*, std::unique_ptr<AutomationState>>& states() {
    static auto* m = new std::map<const Runtime*, std::unique_ptr<AutomationState>>;
    return *m;
}

std::string quoted(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

std::string error_json(const std::string& domain, const std::string& code,
                       const std::string& message) {
    return "{\"domain\":" + quoted(domain) + ",\"code\":" + quoted(code) +
           ",\"message\":" + quoted(message) + "}";
}

std::string base64(const std::string& in) {
    static const char* const kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto byte = [&](size_t i) { return uint32_t{static_cast<uint8_t>(in[i])}; };
    auto letter = [](uint32_t n, unsigned shift) { return kAlphabet[(n >> shift) & 63U]; };
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t n = (byte(i) << 16U) | (byte(i + 1) << 8U) | byte(i + 2);
        out += letter(n, 18);
        out += letter(n, 12);
        out += letter(n, 6);
        out += letter(n, 0);
    }
    if (i < in.size()) {
        uint32_t n = byte(i) << 16U;
        if (i + 1 < in.size())
            n |= byte(i + 1) << 8U;
        out += letter(n, 18);
        out += letter(n, 12);
        out += i + 1 < in.size() ? letter(n, 6) : '=';
        out += '=';
    }
    return out;
}

const JsonValue* field(const JsonValue& v, const char* key, JsonValue::Type type) {
    const JsonValue* f = v.find(key);
    return f && f->type == type ? f : nullptr;
}

std::string string_field(const JsonValue& v, const char* key) {
    const JsonValue* f = field(v, key, JsonValue::Type::kString);
    return f ? f->text : std::string();
}

/* A positive integer id, or 0. */
int64_t id_field(const JsonValue& v) {
    const JsonValue* f = field(v, "id", JsonValue::Type::kNumber);
    if (!f || !f->integral || f->number < 1 || f->number > 9007199254740992.0)
        return 0;
    return static_cast<int64_t>(f->number);
}

std::shared_ptr<Profile> open_profile(const Runtime& rt, const std::string& id) {
    for (const auto& p : rt.profiles) {
        if (p->open && p->id == id)
            return p;
    }
    return nullptr;
}

std::string session_info_json(const Session& s) {
    std::string caps = "[";
    for (size_t i = 0; i < s.caller->capabilities.size(); ++i)
        caps += (i ? "," : "") + quoted(s.caller->capabilities[i]);
    return "{\"name\":" + quoted(s.caller->name) + ",\"capabilities\":" + caps + "]}";
}

bool in_profiles(const Caller& caller, bdk_profile_t profile) {
    return caller.scope.all_profiles || caller.scope.profiles.count(profile) != 0;
}

/* ---- Tasks ---- */

void task_event(const std::shared_ptr<AutomationTask>& t, uint32_t kind,
                const std::string& detail) {
    auto subs = t->subs;
    bdk_task_t handle = t->handle;
    post([subs, kind, detail, handle] {
        for (auto& s : subs) {
            if (s->active && s->tcb)
                s->tcb(s->ctx, handle, kind, view(detail));
        }
        if (kind == BDK_TASK_EVENT_FINISHED) {
            for (auto& s : subs)
                deactivate(s);
        }
    });
}

void drop_from_session(Session& session, const AutomationTask* task) {
    auto& tasks = session.tasks;
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                               [task](const std::weak_ptr<AutomationTask>& w) {
                                   auto t = w.lock();
                                   return !t || t.get() == task;
                               }),
                tasks.end());
}

void task_finish(const std::shared_ptr<AutomationTask>& t, const std::string& detail) {
    if (t->finished)
        return;
    t->finished = true;
    t->finish_detail = detail;
    t->connection->Close(nullptr, nullptr, "");
    if (t->session)
        drop_from_session(*t->session, t.get());
    task_event(t, BDK_TASK_EVENT_FINISHED, detail);
    if (t->started) {
        auto rt = t->rt.lock();
        if (rt) {
            /* Holds the task, not just its port: releasing the task's handle may drop the last
               other reference first, and the port reads the task's id through a weak pointer. */
            post([rt, t] { backend().EndTask(*rt, *t->port); });
        }
    }
}

void task_fail(const std::shared_ptr<AutomationTask>& t, const char* domain, const char* code,
               const std::string& message) {
    task_finish(t, "{\"error\":" + error_json(domain, code, message) + "}");
}

/* Tells the script it is being torn down, then finishes. */
void task_abort(const std::shared_ptr<AutomationTask>& t, const char* domain, const char* code,
                const std::string& message) {
    if (t->finished)
        return;
    t->connection->Send("{\"type\":\"abort\",\"error\":" + error_json(domain, code, message) + "}");
    task_fail(t, domain, code, message);
}

/* ---- Calls made on a connection ---- */

struct WireCall {
    std::weak_ptr<Connection> connection;
    std::weak_ptr<Runtime> rt;
    std::weak_ptr<AutomationTask> task;
    std::string session, command, profile, contents;
    int64_t id = 0;
};

/* Records a call, answered or refused (domain and code empty when it succeeded). */
void audit(const WireCall& w, const std::string& domain, const std::string& code) {
    auto rt = w.rt.lock();
    if (!rt)
        return;
    std::string err;
    if (!domain.empty())
        err = "{\"domain\":" + quoted(domain) + ",\"code\":" + quoted(code) + "}";
    std::string payload = "{\"session\":" + quoted(w.session);
    auto task = w.task.lock();
    if (task)
        payload += ",\"task\":" + quoted(task->id);
    payload += ",\"command\":" + quoted(w.command);
    if (!w.profile.empty())
        payload += ",\"profile\":" + quoted(w.profile);
    if (!w.contents.empty())
        payload += ",\"contents\":" + quoted(w.contents);
    if (!err.empty())
        payload += ",\"error\":" + err;
    payload += '}';
    emit_builtin_event(rt, "automation.commandInvoked", 0, payload);
    if (task && !task->finished) {
        std::string detail = "{\"command\":" + quoted(w.command);
        if (!w.contents.empty())
            detail += ",\"contents\":" + quoted(w.contents);
        if (!w.profile.empty())
            detail += ",\"profile\":" + quoted(w.profile);
        if (!err.empty())
            detail += ",\"error\":" + err;
        task_event(task, BDK_TASK_EVENT_COMMAND, detail + "}");
    }
}

void on_wire_result(void* ctx, bdk_error_t error, bdk_string_t json, bdk_buffer_t binary) {
    auto* w = static_cast<WireCall*>(ctx);
    if (error)
        audit(*w, str(bdk_error_domain(error)), str(bdk_error_code(error)));
    else
        audit(*w, std::string(), std::string());
    auto c = w->connection.lock();
    if (!c || c->closed())
        return;
    c->CallFinished(w->id);
    std::string id = std::to_string(w->id);
    if (error) {
        c->Send("{\"type\":\"error\",\"id\":" + id + ",\"error\":" +
                error_json(str(bdk_error_domain(error)), str(bdk_error_code(error)),
                           str(bdk_error_message(error))) +
                "}");
        return;
    }
    std::string frame = "{\"type\":\"result\",\"id\":" + id +
                        ",\"value\":" + (json.length ? str(json) : std::string("null"));
    if (binary) {
        bdk_bytes_t b = bdk_buffer_bytes(binary);
        frame += ",\"binary\":\"" +
                 base64(std::string(reinterpret_cast<const char*>(b.data), b.length)) + "\"";
    }
    c->Send(frame + "}");
}

void on_wire_progress(void* ctx, double fraction, bdk_string_t message) {
    auto* w = static_cast<WireCall*>(ctx);
    auto c = w->connection.lock();
    if (!c || c->closed())
        return;
    char number[32];
    snprintf(number, sizeof number, "%.6g", fraction);
    c->Send("{\"type\":\"progress\",\"id\":" + std::to_string(w->id) + ",\"fraction\":" + number +
            ",\"message\":" + quoted(str(message)) + "}");
}

void on_wire_release(void* ctx) {
    delete static_cast<WireCall*>(ctx);
}

} // namespace

/* ---- Connection ---- */

void Connection::Send(const std::string& frame) {
    if (closed_ || !send_)
        return;
    if (frame.size() > kMaxFrameBytes || !send_(frame))
        Close(BDK_DOMAIN_AUTOMATION, BDK_CODE_OVERLOADED,
              "the other side does not read its frames fast enough");
}

void Connection::SendError(int64_t id, const char* domain, const char* code,
                           const std::string& message) {
    Send("{\"type\":\"error\",\"id\":" + std::to_string(id) +
         ",\"error\":" + error_json(domain, code, message) + "}");
}

void Connection::Close(const char* domain, const char* code, const std::string& message) {
    if (closed_)
        return;
    if (domain && send_ && !(code && std::strcmp(code, BDK_CODE_OVERLOADED) == 0))
        send_("{\"type\":\"closed\",\"error\":" + error_json(domain, code, message) + "}");
    closed_ = true;
    auto calls = std::move(calls_);
    calls_.clear();
    subs_.clear();
    for (auto& kv : calls)
        bdk_call_cancel(kv.second);
    if (close_) {
        auto close = std::move(close_);
        close_ = nullptr;
        close();
    }
    send_ = nullptr;
}

void Connection::Receive(const std::string& text) {
    if (closed_)
        return;
    JsonValue frame;
    bool is_object = false;
    if (!valid_json(text, &is_object) || !is_object || !json_parse(text, &frame)) {
        ProtocolError("a frame is not a JSON object");
        return;
    }
    const std::string type = string_field(frame, "type");
    if (kind_ == Kind::kExternal && !session_) {
        if (type == "hello" && !hello_pending_) {
            Hello(frame);
            return;
        }
        ProtocolError(hello_pending_ ? "wait for the welcome before sending frames"
                                     : "the first frame must be a hello");
        return;
    }
    if (type == "call") {
        Call(frame);
    } else if (type == "cancel") {
        auto it = calls_.find(id_field(frame));
        if (it != calls_.end())
            bdk_call_cancel(it->second);
    } else if (type == "subscribe") {
        Subscribe(frame);
    } else if (type == "unsubscribe") {
        subs_.erase(id_field(frame));
    } else if (kind_ == Kind::kTask &&
               (type == "progress" || type == "log" || type == "done" || type == "failed")) {
        TaskFrame(type, frame);
    } else {
        ProtocolError("unknown frame type '" + type + "'");
    }
}

void Connection::Hello(const JsonValue& frame) {
    auto rt = rt_.lock();
    if (!rt)
        return;
    const JsonValue* protocol = field(frame, "protocol", JsonValue::Type::kNumber);
    const JsonValue* token = field(frame, "token", JsonValue::Type::kString);
    if (!protocol || protocol->number != kProtocolVersion || !token) {
        Close(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
              "a hello needs protocol 1 and a token");
        return;
    }
    AutomationState& st = automation_state(*rt);
    if (!st.hello_cb) {
        Close(BDK_DOMAIN_AUTOMATION, BDK_CODE_DISABLED, "the endpoint is closed");
        return;
    }
    hello_pending_ = true;
    auto hello = std::make_shared<struct Hello>();
    hello->connection = weak_from_this();
    bdk_context_t handle = table_make(T_HELLO, hello);
    const JsonValue* client = field(frame, "client", JsonValue::Type::kObject);
    std::string json = "{\"token\":" + quoted(token->text) +
                       ",\"client\":" + (client ? json_serialize(*client) : std::string("{}")) +
                       "}";
    auto cb = st.hello_cb;
    void* ctx = st.hello_ctx;
    post([rt, cb, ctx, handle, json] {
        if (rt->shutting || table_type(handle) != T_HELLO)
            return;
        AutomationState& s = automation_state(*rt);
        if (s.hello_cb == cb && s.hello_ctx == ctx)
            cb(ctx, handle, view(json));
    });
}

void Connection::Call(const JsonValue& frame) {
    auto rt = rt_.lock();
    if (!rt)
        return;
    const int64_t id = id_field(frame);
    const std::string command = string_field(frame, "command");
    if (id == 0 || command.empty()) {
        ProtocolError("a call needs a positive integer id and a command");
        return;
    }
    if (calls_.count(id) || subs_.count(id)) {
        ProtocolError("id " + std::to_string(id) + " is already in use");
        return;
    }
    auto w = std::make_unique<WireCall>();
    w->connection = weak_from_this();
    w->rt = rt;
    w->task = task_;
    w->session = session_->caller->name;
    w->command = command;
    w->profile = string_field(frame, "profile");
    w->contents = string_field(frame, "contents");
    w->id = id;
    /* Refused before the broker sees it, and audited like a broker refusal. */
    auto refuse = [&](const char* domain, const char* code, const std::string& message) {
        audit(*w, domain, code);
        SendError(id, domain, code, message);
    };
    const JsonValue* args = frame.find("args");
    if (args && args->type != JsonValue::Type::kObject && args->type != JsonValue::Type::kNull) {
        refuse(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS, "args must be an object");
        return;
    }
    bdk_invoke_options_t options;
    memset(&options, 0, sizeof options);
    options.struct_size = sizeof options;
    if (!w->profile.empty()) {
        auto p = open_profile(*rt, w->profile);
        if (!p) {
            refuse(BDK_DOMAIN_PROFILE, BDK_CODE_NOT_FOUND, "no open profile '" + w->profile + "'");
            return;
        }
        options.profile = p->handle;
    }
    if (!w->contents.empty()) {
        auto c = contents_from_id(w->contents);
        if (!c) {
            refuse(BDK_DOMAIN_CONTENTS, BDK_CODE_STALE, "no page '" + w->contents + "'");
            return;
        }
        options.contents = c->handle;
    }
    CommandRef ref;
    ref.name = command;
    std::string input =
        args && args->type == JsonValue::Type::kObject ? json_serialize(*args) : std::string("{}");
    bdk_call_t call = broker_submit(rt, session_->caller, ref, input, &options, false,
                                    on_wire_result, on_wire_progress, w.release(), on_wire_release);
    calls_[id] = call;
}

void Connection::Subscribe(const JsonValue& frame) {
    auto rt = rt_.lock();
    if (!rt)
        return;
    const int64_t id = id_field(frame);
    const std::string name = string_field(frame, "event");
    if (id == 0 || name.empty()) {
        ProtocolError("a subscribe needs a positive integer id and an event");
        return;
    }
    if (calls_.count(id) || subs_.count(id)) {
        ProtocolError("id " + std::to_string(id) + " is already in use");
        return;
    }
    EventSub sub;
    sub.name = name;
    std::string capability;
    for (size_t i = 0; i < table::kEventCount; ++i) {
        if (name == table::kEvents[i].name) {
            sub.id = table::kEvents[i].id;
            capability = table::kEvents[i].capability;
        }
    }
    if (sub.id == 0) {
        auto it = rt->events.find(name);
        if (it == rt->events.end()) {
            SendError(id, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                      "unknown event '" + name + "'");
            return;
        }
        sub.id = it->second;
        capability = name; /* an application event needs the capability named like it */
    }
    const Caller& caller = *session_->caller;
    if (!capability_granted(caller, capability)) {
        SendError(id, BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED,
                  "the session lacks the capability '" + capability + "'");
        return;
    }
    const std::string profile = string_field(frame, "profile");
    const std::string contents = string_field(frame, "contents");
    if (!profile.empty()) {
        auto p = open_profile(*rt, profile);
        if (!p || !in_profiles(caller, p->handle)) {
            SendError(id, BDK_DOMAIN_CAPABILITY, BDK_CODE_OUT_OF_SCOPE,
                      "the profile is outside the session's scope");
            return;
        }
        sub.profile = p->handle;
    }
    if (!contents.empty()) {
        auto c = contents_from_id(contents);
        if (!c || !caller_sees_contents(caller, *c)) {
            SendError(id, BDK_DOMAIN_CAPABILITY, BDK_CODE_OUT_OF_SCOPE,
                      "the page is outside the session's scope");
            return;
        }
        sub.contents = c->handle;
    }
    subs_[id] = sub;
    Send("{\"type\":\"result\",\"id\":" + std::to_string(id) + ",\"value\":null}");
}

void Connection::DeliverEvent(const std::string& name, uint32_t id, bdk_profile_t profile,
                              bdk_contents_t contents, const std::string& payload) {
    std::vector<int64_t> matches;
    for (const auto& kv : subs_) {
        const EventSub& s = kv.second;
        if (s.name != name && s.id != id)
            continue;
        if (s.profile && s.profile != profile)
            continue;
        if (s.contents && s.contents != contents)
            continue;
        matches.push_back(kv.first);
    }
    for (int64_t sub : matches) {
        Send("{\"type\":\"event\",\"id\":" + std::to_string(sub) + ",\"name\":" + quoted(name) +
             ",\"payload\":" + payload + "}");
    }
}

void Connection::TaskFrame(const std::string& type, const JsonValue& frame) {
    auto task = task_.lock();
    if (!task || task->finished)
        return;
    if (type == "progress") {
        const JsonValue* f = field(frame, "fraction", JsonValue::Type::kNumber);
        double fraction = f ? std::min(1.0, std::max(0.0, f->number)) : 0.0;
        char number[32];
        snprintf(number, sizeof number, "%.6g", fraction);
        task_event(task, BDK_TASK_EVENT_PROGRESS,
                   std::string("{\"fraction\":") + number +
                       ",\"message\":" + quoted(string_field(frame, "message")) + "}");
    } else if (type == "log") {
        std::string level = string_field(frame, "level");
        if (level != "debug" && level != "warn" && level != "error")
            level = "info";
        task_event(task, BDK_TASK_EVENT_LOG,
                   "{\"level\":" + quoted(level) +
                       ",\"message\":" + quoted(string_field(frame, "message")) + "}");
    } else if (type == "done") {
        const JsonValue* result = frame.find("result");
        std::string json = result ? json_serialize(*result) : std::string("null");
        if (json.size() > kMaxPayloadBytes) {
            task_fail(task, BDK_DOMAIN_BROKER, BDK_CODE_PAYLOAD_TOO_LARGE,
                      "the script's result is larger than " + std::to_string(kMaxPayloadBytes) +
                          " bytes");
            return;
        }
        task_finish(task, "{\"result\":" + json + "}");
    } else {
        const JsonValue* error = field(frame, "error", JsonValue::Type::kObject);
        std::string message = error ? string_field(*error, "message") : std::string();
        task_fail(task, BDK_DOMAIN_AUTOMATION, BDK_CODE_SCRIPT_FAILED,
                  message.empty() ? "the script failed" : message);
    }
}

/* ---- TaskPort ---- */

TaskPort::TaskPort(std::weak_ptr<AutomationTask> task) : task_(std::move(task)) {}
TaskPort::~TaskPort() = default;

const std::string& TaskPort::id() const {
    auto t = task_.lock();
    return t ? t->id : empty_;
}
const std::string& TaskPort::script() const {
    auto t = task_.lock();
    return t ? t->script : empty_;
}
const std::string& TaskPort::entry_json() const {
    auto t = task_.lock();
    return t ? t->entry_json : empty_;
}
const std::string& TaskPort::arguments_json() const {
    auto t = task_.lock();
    return t ? t->arguments_json : empty_;
}

void TaskPort::FromScript(const std::string& frame) {
    auto t = task_.lock();
    if (t && !t->finished)
        t->connection->Receive(frame);
}

void TaskPort::HostGone(const std::string& message) {
    if (auto t = task_.lock())
        task_fail(t, BDK_DOMAIN_AUTOMATION, BDK_CODE_HOST_GONE, message);
}

void TaskPort::ScriptFailed(const std::string& message) {
    if (auto t = task_.lock())
        task_fail(t, BDK_DOMAIN_AUTOMATION, BDK_CODE_SCRIPT_FAILED, message);
}

/* ---- Backend defaults ---- */

void Backend::StartTask(Runtime&, const std::shared_ptr<TaskPort>& task) {
    std::weak_ptr<TaskPort> weak = task;
    post([weak] {
        if (auto port = weak.lock())
            port->HostGone("this runtime has no automation host");
    });
}

void Backend::EndTask(Runtime&, TaskPort&) {}

void Backend::UserScriptsChanged(Runtime&, Profile&, const std::vector<UserScript>&) {}

void Backend::CallerRevoked(Runtime&, const Caller&) {}

/* ---- Shared with contents_commands.cc and the backends ---- */

AutomationState& automation_state(const Runtime& rt) {
    auto& m = states();
    auto it = m.find(&rt);
    if (it == m.end())
        it = m.emplace(&rt, std::make_unique<AutomationState>()).first;
    return *it->second;
}

std::string contents_id(bdk_contents_t contents) {
    return std::to_string(contents);
}

std::shared_ptr<Contents> contents_from_id(const std::string& id) {
    if (id.empty() || id.size() > 20 ||
        !std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return nullptr;
    /* from_chars, not stoull: twenty digits can exceed a handle, and stoull throws on that. */
    bdk_contents_t handle = 0;
    const auto parsed = std::from_chars(id.data(), id.data() + id.size(), handle);
    if (parsed.ec != std::errc() || parsed.ptr != id.data() + id.size())
        return nullptr;
    auto c = contents_of(handle);
    return c && !c->dead ? c : nullptr;
}

bool devtools_protocol_enabled(const Runtime& rt) {
    return automation_state(rt).protocol;
}

const std::vector<UserScript>& user_scripts(const Runtime& rt, const Profile& profile) {
    return automation_state(rt).user_scripts[profile.id];
}

void user_scripts_changed(Runtime& rt, Profile& profile) {
    backend().UserScriptsChanged(rt, profile, automation_state(rt).user_scripts[profile.id]);
}

bool caller_sees_contents(const Caller& caller, const Contents& contents) {
    if (caller.kind == CallerKind::kHost)
        return true;
    if (!contents.profile || !in_profiles(caller, contents.profile->handle))
        return false;
    return caller.scope.all_contents || caller.scope.contents.count(contents.handle) != 0;
}

std::shared_ptr<Session> session_of(const Runtime& rt, const Caller& caller) {
    for (auto& s : automation_state(rt).sessions) {
        if (s->caller.get() == &caller)
            return s;
    }
    return nullptr;
}

std::string contents_info_json(const Runtime& rt, const Contents& c) {
    ContentsState st;
    backend().State(c, &st);
    auto it = automation_state(rt).pages.find(c.handle);
    bool crashed = it != automation_state(rt).pages.end() && it->second.crashed;
    return "{\"id\":" + quoted(contents_id(c.handle)) + ",\"url\":" + quoted(st.url) +
           ",\"title\":" + quoted(st.title) +
           ",\"profile\":" + quoted(c.profile ? c.profile->id : std::string()) +
           ",\"visible\":" + (st.visibility == BDK_VISIBILITY_VISIBLE ? "true" : "false") +
           ",\"loading\":" + (st.loading ? "true" : "false") +
           ",\"crashed\":" + (crashed ? "true" : "false") + "}";
}

void automation_page_created(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Contents>& c,
                             const Caller& caller) {
    AutomationState& st = automation_state(*rt);
    std::string creator = "{\"kind\":\"swift\"}";
    if (auto session = session_of(*rt, caller)) {
        if (session->pages != Session::Pages::kAll)
            session->caller->scope.contents.insert(c->handle);
        st.pages[c->handle].created_by = session->caller->name;
        creator = "{\"kind\":\"session\",\"session\":" + quoted(session->caller->name) + "}";
    }
    emit_builtin_event_for(rt, "contents.created", c->profile ? c->profile->handle : 0, c->handle,
                           "{\"contents\":" + contents_info_json(*rt, *c) +
                               ",\"createdBy\":" + creator + "}");
}

void add_contents_waiter(const Runtime& rt, std::shared_ptr<ContentsWaiter> waiter) {
    auto& waiters = automation_state(rt).waiters;
    waiters.erase(std::remove_if(waiters.begin(), waiters.end(),
                                 [](const std::shared_ptr<ContentsWaiter>& w) { return w->done; }),
                  waiters.end());
    waiters.push_back(std::move(waiter));
}

/* ---- Hooks called by the rest of the core ---- */

bdk_status_t automation_configure(Runtime& rt, const std::string& resolved,
                                  bdk_error_t* out_error) {
    AutomationState& st = automation_state(rt);
    JsonValue root;
    if (!json_parse(resolved, &root))
        return BDK_OK; /* bdk_runtime_start has already refused text that is not JSON */
    if (const JsonValue* a = field(root, "automation", JsonValue::Type::kObject)) {
        const JsonValue* enabled = a->find("enabled");
        if (enabled && enabled->type != JsonValue::Type::kBool)
            return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                        "automation.enabled must be a boolean");
        st.enabled = enabled && enabled->boolean;
        if (const JsonValue* scripts = a->find("scripts")) {
            if (scripts->type != JsonValue::Type::kObject)
                return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                            "automation.scripts must be an object");
            for (const auto& m : scripts->members) {
                if (m.second.type != JsonValue::Type::kObject)
                    return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                                "automation.scripts." + m.first + " must be an object");
                st.scripts[m.first] = json_serialize(m.second);
            }
        }
        if (const JsonValue* ext = field(*a, "external", JsonValue::Type::kObject)) {
            const JsonValue* on = ext->find("enabled");
            st.external_enabled = on && on->type == JsonValue::Type::kBool && on->boolean;
        }
    }
    if (const JsonValue* d = field(root, "devtools", JsonValue::Type::kObject)) {
        const JsonValue* protocol = d->find("protocol");
        if (protocol && protocol->type != JsonValue::Type::kBool)
            return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                        "devtools.protocol must be a boolean");
        st.protocol = !protocol || protocol->boolean;
        st.frontend_disabled = string_field(*d, "frontend") == "disabled";
    }
    return BDK_OK;
}

namespace {

void revoke_session(const std::shared_ptr<Runtime>& rt, const std::shared_ptr<Session>& s,
                    bool announce) {
    if (s->revoked)
        return;
    s->revoked = true;
    broker_revoke_caller(s->caller);
    backend().CallerRevoked(*rt, *s->caller);
    auto tasks = s->tasks;
    for (auto& w : tasks) {
        if (auto t = w.lock())
            task_abort(t, BDK_DOMAIN_AUTOMATION, BDK_CODE_SESSION_REVOKED,
                       "the session was revoked");
    }
    auto connections = s->connections;
    for (auto& w : connections) {
        if (auto c = w.lock())
            c->Close(BDK_DOMAIN_AUTOMATION, BDK_CODE_SESSION_REVOKED, "the session was revoked");
    }
    AutomationState& st = automation_state(*rt);
    for (auto& kv : st.user_scripts) {
        auto& list = kv.second;
        size_t before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const UserScript& u) { return u.owner == s->id; }),
                   list.end());
        if (list.size() != before && !rt->shutting) {
            if (auto p = open_profile(*rt, kv.first))
                user_scripts_changed(*rt, *p);
        }
    }
    if (announce)
        emit_builtin_event(rt, "automation.sessionRevoked", 0,
                           "{\"session\":" + session_info_json(*s) + "}");
    st.sessions.erase(std::remove(st.sessions.begin(), st.sessions.end(), s), st.sessions.end());
    table_free(s->handle);
}

void close_endpoint(AutomationState& st, const char* why) {
    auto connections = std::move(st.external);
    st.external.clear();
    for (auto& kv : connections)
        kv.second->Close(BDK_DOMAIN_AUTOMATION, BDK_CODE_DISABLED, why);
    st.server.reset();
    release_later(st.hello_ctx, st.hello_rel);
    st.hello_cb = nullptr;
    st.hello_ctx = nullptr;
    st.hello_rel = nullptr;
}

} // namespace

void automation_shutdown(const std::shared_ptr<Runtime>& rt) {
    AutomationState& st = automation_state(*rt);
    auto sessions = st.sessions;
    for (auto& s : sessions)
        revoke_session(rt, s, false);
    close_endpoint(st, "the runtime shut down");
    for (auto& w : st.waiters)
        w->done = true;
    st.waiters.clear();
    states().erase(rt.get());
}

void automation_deliver_event(const std::shared_ptr<Runtime>& rt, const std::string& name,
                              uint32_t id, bdk_profile_t profile, bdk_contents_t contents,
                              const std::string& payload, const Caller* only) {
    auto it = states().find(rt.get());
    if (it == states().end() || rt->shutting)
        return;
    auto sessions = it->second->sessions;
    for (auto& s : sessions) {
        const Caller& caller = *s->caller;
        if (s->revoked || (only && only != &caller))
            continue;
        if (profile && !in_profiles(caller, profile))
            continue;
        if (contents) {
            auto page = contents_of(contents);
            if (page ? !caller_sees_contents(caller, *page)
                     : !caller.scope.all_contents && !caller.scope.contents.count(contents))
                continue;
        }
        std::vector<std::shared_ptr<Connection>> targets;
        for (auto& w : s->connections) {
            if (auto c = w.lock())
                targets.push_back(c);
        }
        for (auto& w : s->tasks) {
            auto t = w.lock();
            if (t && !t->finished)
                targets.push_back(t->connection);
        }
        for (auto& c : targets) {
            if (!c->closed())
                c->DeliverEvent(name, id, profile, contents, payload);
        }
    }
}

void automation_forget_contents(const std::shared_ptr<Runtime>& rt, bdk_contents_t contents) {
    auto it = states().find(rt.get());
    if (it == states().end())
        return;
    AutomationState& st = *it->second;
    for (auto& s : st.sessions)
        s->caller->scope.contents.erase(contents);
    st.pages.erase(contents);
    auto waiters = st.waiters;
    for (auto& w : waiters) {
        if (!w->done && w->contents == contents) {
            w->done = true;
            if (w->on_gone)
                w->on_gone();
        }
    }
}

void automation_contents_event(const std::shared_ptr<Contents>& c, uint32_t kind,
                               const std::string& url, const std::string& detail, uint32_t flags) {
    auto rt = c->rt.lock();
    if (!rt || rt->shutting)
        return;
    auto it = states().find(rt.get());
    if (it == states().end())
        return;
    AutomationState& st = *it->second;
    const bdk_profile_t profile = c->profile ? c->profile->handle : 0;
    const std::string id = quoted(contents_id(c->handle));
    const bool main = (flags & BDK_EVENT_FLAG_MAIN_FRAME) != 0;
    switch (kind) {
    case BDK_CONTENTS_EVENT_NAVIGATION_STARTED:
        if (main && !(flags & BDK_EVENT_FLAG_SAME_DOCUMENT))
            emit_builtin_event_for(rt, "contents.loadingChanged", profile, c->handle,
                                   "{\"contents\":" + id + ",\"loading\":true}");
        break;
    case BDK_CONTENTS_EVENT_NAVIGATION_COMMITTED:
        if (main) {
            st.pages[c->handle].crashed = false;
            emit_builtin_event_for(
                rt, "contents.navigated", profile, c->handle,
                "{\"contents\":" + id + ",\"url\":" + quoted(url) + ",\"sameDocument\":" +
                    ((flags & BDK_EVENT_FLAG_SAME_DOCUMENT) ? "true" : "false") +
                    ",\"errorPage\":" + ((flags & BDK_EVENT_FLAG_ERROR_PAGE) ? "true" : "false") +
                    "}");
        }
        break;
    case BDK_CONTENTS_EVENT_LOAD_FINISHED:
    case BDK_CONTENTS_EVENT_NAVIGATION_FAILED:
        if (main)
            emit_builtin_event_for(rt, "contents.loadingChanged", profile, c->handle,
                                   "{\"contents\":" + id + ",\"loading\":false}");
        break;
    case BDK_CONTENTS_EVENT_TITLE_CHANGED: {
        ContentsState state;
        backend().State(*c, &state);
        emit_builtin_event_for(rt, "contents.titleChanged", profile, c->handle,
                               "{\"contents\":" + id + ",\"title\":" + quoted(state.title) + "}");
        break;
    }
    case BDK_CONTENTS_EVENT_CLOSED:
        emit_builtin_event_for(rt, "contents.closed", profile, c->handle,
                               "{\"contents\":" + id + "}");
        break;
    case BDK_CONTENTS_EVENT_RENDERER_GONE:
        st.pages[c->handle].crashed = true;
        emit_builtin_event_for(rt, "contents.crashed", profile, c->handle,
                               "{\"contents\":" + id + "}");
        break;
    default:
        break;
    }
    auto waiters = st.waiters;
    for (auto& w : waiters) {
        if (w->done || w->contents != c->handle)
            continue;
        if (kind == BDK_CONTENTS_EVENT_CLOSED) {
            w->done = true;
            if (w->on_gone)
                w->on_gone();
        } else if (w->on_event && w->on_event(kind, url, detail, flags)) {
            w->done = true;
        }
    }
}

void automation_request_raised(const std::shared_ptr<Runtime>& rt, const Request& request) {
    if (states().find(rt.get()) == states().end())
        return;
    emit_builtin_event_for(rt, "contents.dialogOpened", request.profile, request.contents,
                           "{\"contents\":" + quoted(contents_id(request.contents)) +
                               ",\"surface\":" + quoted(ui_surface_name(request.surface)) + "}");
}

bdk_status_t task_release(bdk_task_t h) {
    auto t = get<AutomationTask>(h, T_TASK);
    if (!t)
        return BDK_ERR_STALE_HANDLE;
    if (!t->finished)
        task_abort(t, BDK_DOMAIN_BROKER, BDK_CODE_CANCELLED, "the task's handle was released");
    table_free(h);
    return BDK_OK;
}

} // namespace bdkcore

/* ---- bdk_automation.h ---- */

using namespace bdkcore;

extern "C" {

BDK_EXPORT bdk_status_t bdk_session_create(bdk_runtime_t h, bdk_string_t options,
                                           bdk_session_t* out, bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    bool is_object = false;
    if (!out || !string_ok(options) || !valid_json(str(options), &is_object) || !is_object)
        return BDK_ERR_INVALID_ARGUMENT;
    *out = 0;
    JsonValue o;
    json_parse(str(options), &o);
    auto bad = [&](const std::string& why) {
        return fail(out_error, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                    "session options: " + why);
    };
    auto caller = std::make_shared<Caller>();
    caller->kind = CallerKind::kSession;
    caller->name = string_field(o, "name");
    if (caller->name.empty())
        return bad("name must be a non-empty string");
    if (const JsonValue* caps = o.find("capabilities")) {
        if (caps->type != JsonValue::Type::kArray)
            return bad("capabilities must be an array of strings");
        for (const JsonValue& c : caps->items) {
            if (c.type != JsonValue::Type::kString)
                return bad("capabilities must be an array of strings");
            caller->capabilities.push_back(c.text);
        }
    }
    std::string denied = grant_error(CallerKind::kSession, caller->capabilities);
    if (!denied.empty())
        return fail(out_error, BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED, denied);
    auto session = std::make_shared<Session>();
    session->rt = rt;
    session->caller = caller;
    session->id = "session-" + std::to_string(automation_state(*rt).next_session++);
    caller->scope.all_profiles = true;
    if (const JsonValue* scope = o.find("scope")) {
        if (scope->type != JsonValue::Type::kObject)
            return bad("scope must be an object");
        if (const JsonValue* profiles = scope->find("profiles")) {
            if (profiles->type == JsonValue::Type::kArray) {
                caller->scope.all_profiles = false;
                for (const JsonValue& p : profiles->items) {
                    if (p.type != JsonValue::Type::kString)
                        return bad("scope.profiles must be \"all\" or an array of profile ids");
                    auto profile = open_profile(*rt, p.text);
                    if (!profile)
                        return fail(out_error, BDK_DOMAIN_PROFILE, BDK_CODE_NOT_FOUND,
                                    "no open profile '" + p.text + "'");
                    caller->scope.profiles.insert(profile->handle);
                }
            } else if (!(profiles->type == JsonValue::Type::kString && profiles->text == "all")) {
                return bad("scope.profiles must be \"all\" or an array of profile ids");
            }
        }
        if (const JsonValue* pages = scope->find("contents")) {
            const std::string mode = pages->type == JsonValue::Type::kString ? pages->text : "";
            if (mode == "all")
                session->pages = Session::Pages::kAll;
            else if (mode == "explicit")
                session->pages = Session::Pages::kExplicit;
            else if (mode != "created")
                return bad("scope.contents must be \"all\", \"created\" or \"explicit\"");
        }
    }
    caller->scope.all_contents = session->pages == Session::Pages::kAll;
    session->handle = table_make(T_SESSION, session);
    automation_state(*rt).sessions.push_back(session);
    emit_builtin_event(rt, "automation.sessionCreated", 0,
                       "{\"session\":" + session_info_json(*session) + "}");
    *out = session->handle;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_session_revoke(bdk_session_t h, bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto s = get<Session>(h, T_SESSION);
    if (!s)
        return BDK_ERR_STALE_HANDLE;
    auto rt = s->rt.lock();
    if (!rt) {
        table_free(h);
        return BDK_OK;
    }
    revoke_session(rt, s, true);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_session_grant_contents(bdk_session_t h, bdk_contents_t contents,
                                                   bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto s = get<Session>(h, T_SESSION);
    auto c = contents_of(contents);
    if (!s || !c)
        return BDK_ERR_STALE_HANDLE;
    if (c->profile && !in_profiles(*s->caller, c->profile->handle))
        return fail(out_error, BDK_DOMAIN_CAPABILITY, BDK_CODE_OUT_OF_SCOPE,
                    "the page is in a profile outside the session's scope");
    s->caller->scope.contents.insert(contents);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_session_run(bdk_session_t h, bdk_string_t script,
                                        bdk_string_t arguments, bdk_task_t* out,
                                        bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto s = get<Session>(h, T_SESSION);
    if (!s)
        return BDK_ERR_STALE_HANDLE;
    auto rt = s->rt.lock();
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    if (!out || !string_ok(script) || !string_ok(arguments))
        return BDK_ERR_INVALID_ARGUMENT;
    *out = 0;
    std::string args = str(arguments);
    if (args.empty())
        args = "null";
    if (!valid_json(args))
        return fail(out_error, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                    "the arguments are not valid JSON");
    AutomationState& st = automation_state(*rt);
    if (!st.enabled)
        return fail(out_error, BDK_DOMAIN_AUTOMATION, BDK_CODE_DISABLED,
                    "[automation] enabled is false in bdk.toml");
    const std::string name = str(script);
    auto entry = st.scripts.find(name);
    if (entry == st.scripts.end())
        return fail(out_error, BDK_DOMAIN_AUTOMATION, BDK_CODE_SCRIPT_NOT_FOUND,
                    "no bundled script '" + name + "' in the resolved configuration");

    auto task = std::make_shared<AutomationTask>();
    task->id = "task-" + std::to_string(st.next_task++);
    task->script = name;
    task->entry_json = entry->second;
    task->arguments_json = args;
    task->rt = rt;
    task->session = s;
    task->port = std::make_shared<TaskPort>(task);
    task->connection = std::make_shared<Connection>(Connection::Kind::kTask, rt);
    task->connection->Bind(s);
    task->connection->SetTask(task);
    std::weak_ptr<TaskPort> port = task->port;
    task->connection->SetTransport(
        [port](const std::string& frame) {
            auto p = port.lock();
            if (p && p->to_script)
                p->to_script(frame);
            return true;
        },
        nullptr);
    task->handle = table_make(T_TASK, task);
    s->tasks.push_back(task);
    std::string start = "{\"type\":\"start\",\"protocol\":" + std::to_string(kProtocolVersion) +
                        ",\"task\":" + quoted(task->id) + ",\"script\":" + quoted(name) +
                        ",\"arguments\":" + args + ",\"session\":" + session_info_json(*s) + "}";
    post([rt, task, start] {
        if (task->finished || rt->shutting)
            return;
        task->started = true;
        backend().StartTask(*rt, task->port);
        if (!task->finished)
            task->connection->Send(start);
    });
    *out = task->handle;
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_task_cancel(bdk_task_t h, bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto t = get<AutomationTask>(h, T_TASK);
    if (!t)
        return BDK_ERR_STALE_HANDLE;
    task_abort(t, BDK_DOMAIN_BROKER, BDK_CODE_CANCELLED, "the application cancelled the task");
    return BDK_OK;
}

BDK_EXPORT bdk_subscription_t bdk_task_subscribe(bdk_task_t h, bdk_task_event_cb cb, void* ctx,
                                                 bdk_release_cb rel) {
    if (check_thread() != BDK_OK || !cb)
        return 0;
    auto t = get<AutomationTask>(h, T_TASK);
    if (!t)
        return 0;
    auto s = std::make_shared<Sub>();
    s->tcb = cb;
    s->ctx = ctx;
    s->rel = rel;
    s->handle = table_make(T_SUB, s);
    if (t->finished) {
        std::string detail = t->finish_detail;
        post([s, h, detail] {
            if (s->active)
                s->tcb(s->ctx, h, BDK_TASK_EVENT_FINISHED, view(detail));
            deactivate(s);
        });
    } else {
        t->subs.push_back(s);
    }
    return s->handle;
}

BDK_EXPORT bdk_status_t bdk_automation_endpoint_open(bdk_runtime_t h, bdk_string_t options,
                                                     bdk_automation_hello_cb cb, void* ctx,
                                                     bdk_release_cb rel,
                                                     bdk_buffer_t* out_socket_path,
                                                     bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    bool is_object = false;
    if (!cb || !out_socket_path || !string_ok(options))
        return BDK_ERR_INVALID_ARGUMENT;
    std::string o = str(options);
    if (o.empty())
        o = "{}";
    if (!valid_json(o, &is_object) || !is_object)
        return BDK_ERR_INVALID_ARGUMENT;
    *out_socket_path = 0;
    AutomationState& st = automation_state(*rt);
    if (!st.external_enabled)
        return fail(out_error, BDK_DOMAIN_AUTOMATION, BDK_CODE_DISABLED,
                    "[automation.external] enabled is false in bdk.toml");
    if (st.server)
        return fail(out_error, BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS,
                    "the endpoint is already open");
    JsonValue parsed;
    json_parse(o, &parsed);
    const std::string directory = string_field(parsed, "directory");

    std::weak_ptr<Runtime> weak = rt;
    LocalServer::Events events;
    events.on_connect = [weak](uint64_t id) {
        auto r = weak.lock();
        if (!r)
            return;
        AutomationState& s = automation_state(*r);
        auto c = std::make_shared<Connection>(Connection::Kind::kExternal, r);
        LocalServer* server = s.server.get();
        c->SetTransport([server, id](const std::string& frame) { return server->Send(id, frame); },
                        [server, id] { server->Disconnect(id); });
        s.external[id] = c;
    };
    events.on_frame = [weak](uint64_t id, const std::string& frame) {
        auto r = weak.lock();
        if (!r)
            return;
        AutomationState& s = automation_state(*r);
        auto it = s.external.find(id);
        if (it != s.external.end())
            it->second->Receive(frame);
    };
    events.on_disconnect = [weak](uint64_t id) {
        auto r = weak.lock();
        if (!r)
            return;
        AutomationState& s = automation_state(*r);
        auto it = s.external.find(id);
        if (it == s.external.end())
            return;
        auto c = it->second;
        s.external.erase(it);
        c->Close(nullptr, nullptr, "");
    };
    std::string path, error;
    st.server = LocalServer::Create(directory, std::move(events), &path, &error);
    if (!st.server)
        return fail(out_error, BDK_DOMAIN_PLATFORM, "failed", error);
    st.hello_cb = cb;
    st.hello_ctx = ctx;
    st.hello_rel = rel;
    *out_socket_path = make_buffer(path);
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_automation_endpoint_close(bdk_runtime_t h, bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto rt = runtime_of(h);
    if (!rt)
        return BDK_ERR_STALE_HANDLE;
    close_endpoint(automation_state(*rt), "the application closed the endpoint");
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_automation_hello_accept(bdk_context_t h, bdk_session_t session,
                                                    bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto hello = get<Hello>(h, T_HELLO);
    if (!hello)
        return BDK_ERR_STALE_HANDLE;
    auto s = get<Session>(session, T_SESSION);
    if (!s || s->revoked)
        return BDK_ERR_STALE_HANDLE;
    table_free(h);
    auto c = hello->connection.lock();
    if (!c || c->closed())
        return BDK_OK; /* the client went away; nothing to welcome */
    c->Bind(s);
    s->connections.push_back(c);
    c->Send("{\"type\":\"welcome\",\"protocol\":" + std::to_string(kProtocolVersion) +
            ",\"session\":" + session_info_json(*s) + "}");
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_automation_hello_refuse(bdk_context_t h, bdk_string_t message,
                                                    bdk_error_t* out_error) {
    clear_out(out_error);
    if (bdk_status_t s = check_thread())
        return s;
    auto hello = get<Hello>(h, T_HELLO);
    if (!hello)
        return BDK_ERR_STALE_HANDLE;
    if (!string_ok(message))
        return BDK_ERR_INVALID_ARGUMENT;
    table_free(h);
    if (auto c = hello->connection.lock()) {
        std::string m = str(message);
        c->Close(BDK_DOMAIN_CAPABILITY, BDK_CODE_DENIED,
                 m.empty() ? "the application refused the connection" : m);
    }
    return BDK_OK;
}

} /* extern "C" */
