/* The stub's side of providers: the translation commands, the reputation check before a navigation
   commits, and geolocation watches, all through the core's provider calls (providers_internal.h).
   The stub has no engine of its own: the default translation provider is unavailable, as in the
   engine until T-PRV-02's engine lands, and the default reputation provider uses the core's lists.
 */
#ifndef BDK_STUB_PROVIDERS_H_
#define BDK_STUB_PROVIDERS_H_

#include <functional>
#include <memory>
#include <string>

#include "../runtime/broker_internal.h"

namespace bdkstub {

/* Forgets every page text, preference and watch. Called at runtime start. */
void providers_reset();

/* Answers a translation.* command and returns true; false for any other command. */
bool providers_execute(bdkcore::Runtime& rt, const std::shared_ptr<bdkcore::BuiltinCall>& call);

/* Asks the reputation provider about a navigation before it commits. go(true) lets it commit;
   go(false) means it was blocked (the warning was not proceeded past). Calls go at once when no
   check is needed, so a navigation with no list and no application provider keeps its timing. */
void providers_check_navigation(const std::shared_ptr<bdkcore::Contents>& c, const std::string& url,
                                const std::function<void(bool)>& go);

} // namespace bdkstub

#endif /* BDK_STUB_PROVIDERS_H_ */
