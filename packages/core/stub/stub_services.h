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

} // namespace bdkstub

#endif /* BDK_STUB_SERVICES_H_ */
