/* Helpers the engine's extension scenarios share (tests_engine_ext_panels.c,
   tests_engine_ext_probe.c): test commands, loading an unpacked fixture of
   tests/extensions/fixtures (BDK_EXTENSION_FIXTURES) without the install prompt, and running script
   in its service worker. Built only into the engine conformance app. */
#ifndef BDK_TEST_ENGINE_EXT_SUPPORT_H_
#define BDK_TEST_ENGINE_EXT_SUPPORT_H_

#include <stddef.h>

#include "bdk/bdk.h"

/* bdk_testing_command, REQUIREd to succeed; out may be NULL. */
void ext_cmd(const char* name, const char* args, char* out, size_t n);
/* Pumps n times. */
void ext_pumps(int n);
/* The page's title. */
void ext_title(bdk_contents_t c, char* out, size_t n);
/* Removes the backslashes before quotes, so JSON carried inside a JSON string can be searched. */
void ext_unescape(char* text);
int ext_has(const char* text, const char* fragment);
/* Loads the fixture `name` in the page's profile and waits until its worker can answer. */
void ext_load(bdk_contents_t c, const char* name, char* id, size_t n);
/* Runs script in the extension's service worker; out is the raw result, quotes unescaped. */
void ext_eval(bdk_contents_t c, const char* ext, const char* script, char* out, size_t n);
/* Runs a promise-returning expression in the worker; out is what it resolved to, as JSON. */
void ext_ask(bdk_contents_t c, const char* ext, const char* promise_expr, char* out, size_t n);

#endif /* BDK_TEST_ENGINE_EXT_SUPPORT_H_ */
