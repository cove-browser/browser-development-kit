/* The stub's automation host: scripts defined by the test as C callbacks (bdk_stub_automation.h).
   Frames cross on later tasks in both directions, as they would between processes. */
#include <map>
#include <string>

#include "../runtime/automation_internal.h"
#include "bdk_stub_automation.h"
#include "stub_services.h"

using namespace bdkcore;

namespace bdkstub {
namespace {

struct Script {
    bdk_stub_script_cb cb = nullptr;
    void* ctx = nullptr;
};

struct HostState {
    std::map<std::string, Script> scripts;
    std::map<uint64_t, std::weak_ptr<TaskPort>> running;
    uint64_t next = 1;
    uint32_t started = 0, ended = 0, scripts_changed = 0;
};

HostState& H() {
    static auto* h = new HostState;
    return *h;
}

} // namespace

void automation_reset() {
    H() = HostState();
}

void automation_start(const std::shared_ptr<TaskPort>& task) {
    HostState& h = H();
    auto it = h.scripts.find(task->script());
    if (it == h.scripts.end()) {
        std::weak_ptr<TaskPort> weak = task;
        std::string name = task->script();
        post([weak, name] {
            if (auto p = weak.lock())
                p->ScriptFailed("the stub has no script named '" + name + "'");
        });
        return;
    }
    ++h.started;
    uint64_t id = h.next++;
    h.running[id] = task;
    Script script = it->second;
    task->to_script = [script, id](const std::string& frame) {
        post([script, id, frame] {
            if (H().running.count(id))
                script.cb(script.ctx, id, view(frame));
        });
    };
}

void automation_end(TaskPort& task) {
    HostState& h = H();
    for (auto it = h.running.begin(); it != h.running.end(); ++it) {
        auto p = it->second.lock();
        if (!p || p.get() == &task) {
            ++h.ended;
            h.running.erase(it);
            return;
        }
    }
}

void automation_scripts_changed() {
    ++H().scripts_changed;
}

} // namespace bdkstub

extern "C" {

BDK_EXPORT bdk_status_t bdk_stub_script_define(bdk_runtime_t h, bdk_string_t name,
                                               bdk_stub_script_cb cb, void* ctx) {
    if (check_thread() != BDK_OK)
        return BDK_ERR_WRONG_THREAD;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    if (!cb || !string_ok(name))
        return BDK_ERR_INVALID_ARGUMENT;
    bdkstub::H().scripts[str(name)] = bdkstub::Script{cb, ctx};
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_script_send(uint64_t task, bdk_string_t frame) {
    if (check_thread() != BDK_OK)
        return BDK_ERR_WRONG_THREAD;
    if (!string_ok(frame))
        return BDK_ERR_INVALID_ARGUMENT;
    auto& running = bdkstub::H().running;
    auto it = running.find(task);
    if (it == running.end())
        return BDK_ERR_STALE_HANDLE;
    std::weak_ptr<TaskPort> weak = it->second;
    std::string f = str(frame);
    post([weak, f] {
        if (auto p = weak.lock())
            p->FromScript(f);
    });
    return BDK_OK;
}

BDK_EXPORT bdk_status_t bdk_stub_automation_host_crash(bdk_runtime_t h) {
    if (check_thread() != BDK_OK)
        return BDK_ERR_WRONG_THREAD;
    if (!runtime_of(h))
        return BDK_ERR_STALE_HANDLE;
    auto running = std::move(bdkstub::H().running);
    bdkstub::H().running.clear();
    for (auto& kv : running) {
        if (auto p = kv.second.lock())
            p->HostGone("the stub's automation host crashed");
    }
    return BDK_OK;
}

BDK_EXPORT uint32_t bdk_stub_automation_count(bdk_runtime_t, uint32_t which) {
    const bdkstub::HostState& h = bdkstub::H();
    switch (which) {
    case BDK_STUB_AUTOMATION_STARTED:
        return h.started;
    case BDK_STUB_AUTOMATION_ENDED:
        return h.ended;
    case BDK_STUB_AUTOMATION_SCRIPTS_CHANGED:
        return h.scripts_changed;
    default:
        return 0;
    }
}

} /* extern "C" */
