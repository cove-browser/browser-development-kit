/* What the web app tests need from the runtime under test beyond tests/harness.h: manifests for
   pages it cannot fetch, and acting as a page that asks to be installed or sets its badge. The stub
   offers them as test hooks (stub/bdk_stub.h); a runtime without them answers 0 from
   bdk_harness_web_app_hooks and the tests that need them are skipped (the engine's scenarios
   against a fixture app are T-PWA-04's). Plain C. */
#ifndef BDK_TEST_HARNESS_WEB_APPS_H_
#define BDK_TEST_HARNESS_WEB_APPS_H_

#include "bdk/bdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if the runtime has these hooks and the stub's in-memory registry, 0 if not. */
int bdk_harness_web_app_hooks(void);

/* The manifest a page at document_url links. */
bdk_status_t bdk_harness_web_app_manifest(bdk_runtime_t, const char* document_url,
                                          const char* manifest_json);
/* Acts as the page calling prompt() on its beforeinstallprompt event. */
bdk_status_t bdk_harness_web_app_prompt(bdk_contents_t, uint8_t user_gesture, bdk_error_t*);
/* Acts as the page calling navigator.setAppBadge ("null" clears). */
bdk_status_t bdk_harness_web_app_badge(bdk_contents_t, const char* badge_json, bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_TEST_HARNESS_WEB_APPS_H_ */
