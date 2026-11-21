/* The stub's in-memory implementation of some built-in commands, and the state its test hooks
   control. Everything else answers broker.notImplemented, like the engine until Phase 2. */
#ifndef BDK_STUB_SERVICES_H_
#define BDK_STUB_SERVICES_H_

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "../runtime/broker_internal.h"

namespace bdkstub {

/* Forgets every stored value and resets the test controls. Called at runtime start. */
void services_reset();

/* Backend::ExecuteCommand for the stub. */
void services_execute(bdkcore::Runtime& rt, const std::shared_ptr<bdkcore::BuiltinCall>& call);
/* Backend::Authenticate for the stub: the answer the test chose, now, later or never. */
void services_authenticate(std::function<void(bool)> done);

/* Answers every held call with null, as a service finishing late would. */
void services_release_held();

/* The internal callers the test hooks created, by identifier (never 0). */
std::map<uint64_t, std::shared_ptr<bdkcore::Caller>>& callers();

/* Test controls (bdk_stub_broker.h). */
struct Controls {
    bool hold = false;      /* services never answer by themselves */
    int authentication = 0; /* 0 refuse, 1 accept, 2 wait for services_answer_authentication */
    uint32_t executed = 0;  /* calls that reached the service */
    uint32_t held = 0;      /* calls the service is holding */
    uint32_t stopped = 0;   /* calls the service was told to stop */
    uint32_t authentications = 0; /* times the operating system was asked */
};
Controls& controls();
/* Answers the authentication request that is waiting (mode 2). False if none is. */
bool services_answer_authentication(bool granted);

/* Extensions (stub_extensions.cc). */
void extensions_reset();
/* A package a source names, for sources the stub cannot read from disk: kind is an
   ExtensionSourceKind name, key the path (crx), the id (updateUrl) or "<store>/<id>" (store). */
void extensions_add_package(const std::string& kind, const std::string& key,
                            const std::string& manifest);
/* The extensions.* commands; false for any other command. */
bool extensions_execute(bdkcore::Runtime& rt, const std::shared_ptr<bdkcore::BuiltinCall>& call);
/* Backend::ExtensionActivateAction (command empty) and ExtensionPerformCommand. */
bdk_status_t extensions_activate(bdkcore::Runtime& rt,
                                 const std::shared_ptr<bdkcore::Contents>& contents,
                                 const std::string& id, const std::string& command,
                                 std::string* result, bdk_error_t* err);

/* Makes the request the one bdk_stub_send_ui_update and bdk_stub_last_ui_update address, and
   records the application's updates to it (stub_backend.cc). */
void track_request(const std::shared_ptr<bdkcore::Request>& request);

} // namespace bdkstub

#endif /* BDK_STUB_SERVICES_H_ */
