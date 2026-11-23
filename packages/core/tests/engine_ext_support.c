/* Helpers the engine's extension scenarios share (engine_ext_support.h). */
#include "engine_ext_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdk_testing.h"
#include "test_support.h"

void ext_cmd(const char* name, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(name), S(args), out, n) == BDK_OK);
}

void ext_pumps(int n) {
    for (int i = 0; i < n; ++i)
        t_pump();
}

void ext_title(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    snprintf(out, n, "%.*s", (int)st.title.length, st.title.data ? st.title.data : "");
}

/* The JSON text with the backslashes before quotes removed, so a result that carries JSON inside a
   JSON string can be searched for plain fragments. */
void ext_unescape(char* text) {
    char* w = text;
    for (const char* r = text; *r; ++r) {
        if (r[0] == '\\' && r[1] == '"')
            continue;
        *w++ = *r;
    }
    *w = 0;
}

int ext_has(const char* text, const char* fragment) {
    return strstr(text, fragment) != NULL;
}

void ext_load(bdk_contents_t c, const char* name, char* id, size_t n) {
    char args[512], out[512];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"path\":\"%s/%s\"}", (unsigned long long)c,
             BDK_EXTENSION_FIXTURES, name);
    ext_cmd("extensions.load", args, NULL, 0);
    id[0] = 0;
    for (int i = 0; i < 400 && !id[0]; ++i) {
        t_pump();
        ext_cmd("extensions.loaded", "{}", out, sizeof out);
        if (ext_has(out, "\"done\":true")) {
            if (!t_json_string(out, "id", id, n) || !id[0]) {
                printf("    extension %s did not load: %s\n", name, out);
                REQUIRE(0);
            }
        }
    }
    REQUIRE(id[0]);
    /* Its service worker starts, and so listens for what the test asks. */
    ext_pumps(20);
}

/* Runs `script` in the extension's service worker and waits for its value (the raw result
   object); `out` has its quotes unescaped. */
void ext_eval(bdk_contents_t c, const char* ext, const char* script, char* out, size_t n) {
    char args[1536];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"extension\":\"%s\",\"script\":\"%s\"}",
             (unsigned long long)c, ext, script);
    ext_cmd("extensions.eval", args, NULL, 0);
    for (int i = 0; i < 100; ++i) {
        ext_cmd("extensions.eval.result", "{}", out, n);
        if (ext_has(out, "\"done\":true")) {
            ext_unescape(out);
            return;
        }
        t_pump();
    }
    REQUIRE(0);
}

/* Runs a promise-returning expression in the service worker and returns what it resolved to, as
   JSON text with quotes unescaped ("null" for a promise that resolves with no value, as
   windows.remove does). */
void ext_ask(bdk_contents_t c, const char* ext, const char* promise_expr, char* out, size_t n) {
    char script[1024];
    snprintf(script, sizeof script,
             "globalThis.out=undefined;(%s).then(v=>{globalThis.out=v===undefined?'null':JSON."
             "stringify(v)});1",
             promise_expr);
    ext_eval(c, ext, script, out, n);
    for (int i = 0; i < 100; ++i) {
        ext_eval(c, ext, "globalThis.out===undefined?'':globalThis.out", out, n);
        if (!ext_has(out, "\"value\":\"\""))
            return;
        t_pump();
    }
    REQUIRE(0);
}
