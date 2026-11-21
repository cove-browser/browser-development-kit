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

/* What the page hooks (bdk_stub_page_bind, bdk_stub_page_navigate) need of a stub page
   (stub_backend.cc). */
/* The internal URL the page committed last, as Chromium's frame would report it. */
std::string page_committed_url(const bdkcore::Contents& contents);
/* Keeps a caller bound for the page, to revoke it when the page navigates or closes. */
void page_keep_caller(bdkcore::Contents& contents, std::shared_ptr<bdkcore::Caller> caller);
/* A navigation the page starts itself: trusted only when it shows an internal page. */
void page_navigate(const std::shared_ptr<bdkcore::Contents>& contents, const std::string& url);
/* Chromium terminates a renderer that asks for an interface it may not have. */
void page_renderer_gone(const std::shared_ptr<bdkcore::Contents>& contents, const char* reason);

} // namespace bdkstub

#endif /* BDK_STUB_SERVICES_H_ */
