/* Engine scenarios for the browser-owned surfaces of T-UIR-02 and T-UIR-06: permission prompts,
   JavaScript dialogs, file pickers, find in page, fullscreen and notifications. Each runs with no
   handler (the default presenter, scripted through bdk_testing_ui so no panel blocks the suite),
   with a handler that completes, defers (useDefault) or rejects, and with the surface disabled
   where the configuration allows it. A permission is granted only with a gesture, and its decision
   lands in the profile's site settings. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bdk_testing.h"
#include "test_support.h"

void bdk_harness_engine_show(bdk_contents_t contents, int visible);
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);
int bdk_harness_engine_answer_sheet(bdk_contents_t contents, int index);
extern const char* bdk_harness_engine_surfaces;

#define PAGE "https://bdk.test/ui"

typedef struct {
    int calls;
    bdk_request_t request;
    bdk_contents_t contents;
    char payload[2048];
    /* 0: leave the request open for the test to complete; else the response to give. */
    bdk_ui_response_t response;
    /* With complete set, completes with it (and the gesture kind of bdk_harness_engine_gesture,
       or -1 for none). */
    const char* complete;
    const char* complete_args;
    int gesture_kind;
} log_t;

static log_t g_log;
static int g_withdrawn;
static bdk_profile_t g_profile;

static bdk_ui_response_t record(void* ctx, const bdk_ui_request_info_t* info) {
    log_t* log = (log_t*)ctx;
    ++log->calls;
    log->request = info->request;
    log->contents = info->contents;
    snprintf(log->payload, sizeof log->payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    if (log->complete) {
        bdk_native_handle_t gesture;
        memset(&gesture, 0, sizeof gesture);
        if (log->gesture_kind >= 0)
            gesture = bdk_harness_engine_gesture(info->contents, log->gesture_kind);
        bdk_error_t err = 0;
        bdk_status_t s =
            bdk_ui_complete(info->request, S(log->complete),
                            S(log->complete_args ? log->complete_args : ""), gesture, &err);
        CHECK(s == BDK_OK);
        return BDK_UI_HANDLED;
    }
    return log->response ? log->response : BDK_UI_HANDLED;
}

static void on_withdrawn(void* ctx) {
    (void)ctx;
    ++g_withdrawn;
}

static void ui(const char* command, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(command), S(args), out, n) == BDK_OK);
}

static void script(const char* surface, const char* answer) {
    char args[512];
    snprintf(args, sizeof args, "{\"surface\":\"%s\",\"answer\":%s}", surface, answer);
    ui("present.script", args, NULL, 0);
}

static int count_of(const char* json, const char* key) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char* at = strstr(json, pattern);
    return at ? atoi(at + strlen(pattern)) : -1;
}

/* How many times the surface's default presenter was asked to show something. */
static int shown(const char* surface) {
    char args[128], out[4096];
    snprintf(args, sizeof args, "{\"surface\":\"%s\"}", surface);
    ui("present.record", args, out, sizeof out);
    return count_of(out, "shown");
}

static int dismissed(const char* surface) {
    char args[128], out[4096];
    snprintf(args, sizeof args, "{\"surface\":\"%s\"}", surface);
    ui("present.record", args, out, sizeof out);
    return count_of(out, "dismissed");
}

/* What the default presenter was last asked to show, as the JSON string it is inside the record. */
static void last_shown(const char* surface, char* out, size_t n) {
    char args[128];
    snprintf(args, sizeof args, "{\"surface\":\"%s\"}", surface);
    ui("present.record", args, out, n);
}

static void page_ui(bdk_contents_t c, const char* command, const char* extra, char* out, size_t n) {
    char args[512];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\"%s%s}", (unsigned long long)c,
             extra[0] ? "," : "", extra);
    ui(command, args, out, n);
}

static void pumps(int n) {
    for (int i = 0; i < n; ++i)
        t_pump();
}

static void run_script(bdk_contents_t c, const char* js, int gesture) {
    REQUIRE(bdk_testing_execute_script(c, S(js), (uint8_t)gesture) == BDK_OK);
    t_pump();
}

static void title_of(bdk_contents_t c, char* out, size_t n) {
    bdk_contents_state_t st;
    memset(&st, 0, sizeof st);
    st.struct_size = sizeof st;
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
    snprintf(out, n, "%.*s", (int)st.title.length, st.title.data ? st.title.data : "");
}

static int wait_for_title(bdk_contents_t c, const char* want) {
    char title[256];
    for (int i = 0; i < 60; ++i) {
        title_of(c, title, sizeof title);
        if (strcmp(title, want) == 0)
            return 1;
        t_pump();
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    return 0;
}

/* Starts a runtime (the surfaces of `surfaces_json`, if any, configured), a handler for `surface`
   (0: none) logging into g_log, and one page that has loaded PAGE and is shown and focused. */
static bdk_contents_t start_page(uint32_t surface, const char* surfaces_json) {
    memset(&g_log, 0, sizeof g_log);
    g_log.gesture_kind = -1;
    g_withdrawn = 0;
    bdk_harness_engine_surfaces = surfaces_json;
    t_start();
    ui("present.clear", "{}", NULL, 0);
    bdk_error_t err = 0;
    if (surface)
        REQUIRE(bdk_ui_set_handler(t_runtime, surface, record, &g_log, NULL, &err) == BDK_OK);
    t_freeze();
    g_profile = t_new_profile("UI");
    bdk_contents_t c = t_new_contents(g_profile);
    REQUIRE(bdk_contents_load(c, S(PAGE), &err) == BDK_OK);
    t_pump();
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    t_pump();
    return c;
}

static bdk_contents_t second_page(bdk_profile_t profile, const char* url) {
    bdk_contents_t c = t_new_contents(profile);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(url), &err) == BDK_OK);
    t_pump();
    bdk_harness_engine_show(c, 1);
    REQUIRE(bdk_contents_focus(c) == BDK_OK);
    t_pump();
    return c;
}

/* Hides the page and lets its renderer settle, so the next test does not start a process while
   this one's is still launching or going. */
static void finish(bdk_contents_t c) {
    bdk_harness_engine_show(c, 0);
    pumps(10);
}

/* ---- JavaScript dialogs ---- */

static void test_ui_a_prompt_with_no_handler_is_answered_by_the_default_presenter(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("javascript_dialogs", "{\"accept\":true,\"text\":\"hello\"}");
    run_script(c, "document.title=prompt('Your name?','dflt')", 0);
    CHECK(wait_for_title(c, "hello"));
    char out[4096];
    last_shown("javascript_dialogs", out, sizeof out);
    CHECK(count_of(out, "shown") == 1);
    /* The presenter was given what a handler would be: kind, origin, message, default text. */
    CHECK(strstr(out, "kind\\\":\\\"prompt") != NULL);
    CHECK(strstr(out, "https://bdk.test") != NULL);
    CHECK(strstr(out, "Your name?") != NULL);
    CHECK(strstr(out, "defaultPromptText\\\":\\\"dflt") != NULL);
    CHECK(strstr(out, "pageTitle") != NULL);
    finish(c);
}

static void test_ui_a_cancelled_confirm_returns_false(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("javascript_dialogs", "{\"accept\":false}");
    run_script(c, "document.title=confirm('sure?')?'yes':'no'", 0);
    CHECK(wait_for_title(c, "no"));
    finish(c);
}

static void test_ui_a_handler_completes_a_dialog(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_JAVASCRIPT_DIALOGS, NULL);
    g_log.complete = "accept";
    g_log.complete_args = "{\"text\":\"typed\",\"suppressFurther\":false}";
    run_script(c, "document.title=prompt('q','d')", 0);
    CHECK(wait_for_title(c, "typed"));
    CHECK(g_log.calls == 1);
    CHECK(strstr(g_log.payload, "\"kind\":\"prompt\"") != NULL);
    CHECK(strstr(g_log.payload, "\"origin\":\"https://bdk.test\"") != NULL);
    CHECK(strstr(g_log.payload, "\"message\":\"q\"") != NULL);
    CHECK(shown("javascript_dialogs") == 0);
    finish(c);
}

static void test_ui_a_handler_cancels_a_dialog(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_JAVASCRIPT_DIALOGS, NULL);
    g_log.complete = "deny";
    g_log.complete_args = "{\"suppressFurther\":false}";
    run_script(c, "document.title=confirm('q')?'yes':'no'", 0);
    CHECK(wait_for_title(c, "no"));
    finish(c);
}

static void test_ui_use_default_shows_the_default_dialog(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_JAVASCRIPT_DIALOGS, NULL);
    g_log.response = BDK_UI_USE_DEFAULT;
    script("javascript_dialogs", "{\"accept\":true}");
    run_script(c, "document.title=confirm('q')?'yes':'no'", 0);
    CHECK(wait_for_title(c, "yes"));
    CHECK(g_log.calls == 1);
    CHECK(shown("javascript_dialogs") == 1);
    finish(c);
}

static void test_ui_a_rejected_dialog_is_cancelled(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_JAVASCRIPT_DIALOGS, NULL);
    g_log.response = BDK_UI_REJECT;
    script("javascript_dialogs", "{\"accept\":true}");
    run_script(c, "document.title=confirm('q')?'yes':'no'", 0);
    CHECK(wait_for_title(c, "no"));
    CHECK(shown("javascript_dialogs") == 0);
    finish(c);
}

static void test_ui_preventing_further_dialogs_suppresses_the_next(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_JAVASCRIPT_DIALOGS, NULL);
    g_log.complete = "accept";
    g_log.complete_args = "{\"suppressFurther\":true}";
    run_script(c, "var a=confirm('1');var b=confirm('2');document.title=a+'/'+b", 0);
    CHECK(wait_for_title(c, "true/false"));
    CHECK(g_log.calls == 1);
    finish(c);
}

static void test_ui_navigating_away_withdraws_and_dismisses_a_dialog(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("javascript_dialogs", "{\"none\":true}");
    run_script(c, "confirm('waiting')", 0);
    pumps(3);
    CHECK(shown("javascript_dialogs") == 1);
    bdk_error_t err = 0;
    /* Another site: the blocked renderer cannot commit a navigation of its own site. */
    REQUIRE(bdk_contents_load(c, S("https://other.test/elsewhere"), &err) == BDK_OK);
    pumps(5);
    CHECK(dismissed("javascript_dialogs") == 1);
    finish(c);
}

static void test_ui_the_default_dialog_is_a_sheet_on_the_pages_window(void) {
    bdk_contents_t c = start_page(0, NULL);
    run_script(c, "document.title=confirm('real sheet')?'yes':'no'", 0);
    int answered = 0;
    for (int i = 0; i < 40 && !answered; ++i) {
        t_pump();
        answered = bdk_harness_engine_answer_sheet(c, 0);
    }
    CHECK(answered);
    CHECK(wait_for_title(c, "yes"));
    finish(c);
}

static void test_ui_beforeunload_asks_with_chromiums_text(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_JAVASCRIPT_DIALOGS, NULL);
    g_log.complete = "accept";
    g_log.complete_args = "{\"suppressFurther\":false}";
    bdk_error_t err = 0;
    REQUIRE(bdk_testing_script_navigation(t_runtime, S("https://bdk.test/after"), 0, S("After")) ==
            BDK_OK);
    /* A page the person has interacted with may ask before it is left. */
    run_script(c, "window.onbeforeunload=e=>{e.preventDefault();e.returnValue='mine'}", 1);
    REQUIRE(bdk_contents_load(c, S("https://bdk.test/after"), &err) == BDK_OK);
    CHECK(wait_for_title(c, "After"));
    CHECK(g_log.calls == 1);
    CHECK(strstr(g_log.payload, "\"kind\":\"beforeUnload\"") != NULL);
    CHECK(strstr(g_log.payload, "Changes you made may not be saved.") != NULL);
    CHECK(strstr(g_log.payload, "mine") == NULL);
    finish(c);
}

/* ---- file pickers ---- */

static const char* picker_script =
    "var i=document.createElement('input');i.type='file';"
    "i.onchange=()=>document.title='picked:'+Array.from(i.files).map(f=>f.name).join(',');"
    "i.oncancel=()=>document.title='cancelled';"
    "(document.body||document.documentElement).appendChild(i);i.click()";

static const char* picker_multiple_script =
    "var i=document.createElement('input');i.type='file';i.multiple=true;"
    "i.onchange=()=>document.title='picked:'+Array.from(i.files).map(f=>f.name).join(',');"
    "i.oncancel=()=>document.title='cancelled';"
    "(document.body||document.documentElement).appendChild(i);i.click()";

/* A file the page may be given; `name` receives its base name. */
static void make_file(char* path, size_t n, char* name, size_t name_n) {
    snprintf(path, n, "/tmp/bdk-pick-XXXXXX");
    int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    REQUIRE(write(fd, "x", 1) == 1);
    close(fd);
    snprintf(name, name_n, "%s", strrchr(path, '/') + 1);
}

static void test_ui_a_file_input_with_no_handler_uses_the_default_picker(void) {
    char path[64], name[64], answer[200], want[128];
    make_file(path, sizeof path, name, sizeof name);
    bdk_contents_t c = start_page(0, NULL);
    snprintf(answer, sizeof answer, "{\"paths\":[\"%s\"]}", path);
    script("file_pickers", answer);
    run_script(c, picker_script, 1);
    snprintf(want, sizeof want, "picked:%s", name);
    CHECK(wait_for_title(c, want));
    char out[4096];
    last_shown("file_pickers", out, sizeof out);
    CHECK(strstr(out, "mode\\\":\\\"open\\\"") != NULL);
    unlink(path);
    finish(c);
}

static void test_ui_cancelling_the_default_picker_cancels_the_input(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("file_pickers", "{\"paths\":[]}");
    run_script(c, picker_script, 1);
    CHECK(wait_for_title(c, "cancelled"));
    finish(c);
}

static void test_ui_a_handler_chooses_files(void) {
    char path[64], name[64], args[200], want[128];
    make_file(path, sizeof path, name, sizeof name);
    bdk_contents_t c = start_page(BDK_SURFACE_FILE_PICKERS, NULL);
    snprintf(args, sizeof args, "{\"paths\":[\"%s\"]}", path);
    g_log.complete = "accept";
    g_log.complete_args = args;
    run_script(c, picker_script, 1);
    snprintf(want, sizeof want, "picked:%s", name);
    CHECK(wait_for_title(c, want));
    CHECK(strstr(g_log.payload, "\"mode\":\"open\"") != NULL);
    CHECK(strstr(g_log.payload, "\"isFileSystemAccessAPI\":false") != NULL);
    unlink(path);
    finish(c);
}

static void test_ui_a_handler_chooses_several_files_for_a_multiple_input(void) {
    char path1[64], name1[64], path2[64], name2[64], args[300];
    make_file(path1, sizeof path1, name1, sizeof name1);
    make_file(path2, sizeof path2, name2, sizeof name2);
    bdk_contents_t c = start_page(BDK_SURFACE_FILE_PICKERS, NULL);
    snprintf(args, sizeof args, "{\"paths\":[\"%s\",\"%s\"]}", path1, path2);
    g_log.complete = "accept";
    g_log.complete_args = args;
    run_script(c, picker_multiple_script, 1);
    char title[256];
    for (int i = 0; i < 40; ++i) {
        title_of(c, title, sizeof title);
        if (strncmp(title, "picked:", 7) == 0)
            break;
        t_pump();
    }
    CHECK(strstr(title, name1) != NULL && strstr(title, name2) != NULL);
    CHECK(strstr(g_log.payload, "\"mode\":\"openMultiple\"") != NULL);
    unlink(path1);
    unlink(path2);
    finish(c);
}

static void test_ui_a_handler_cancels_a_picker(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_FILE_PICKERS, NULL);
    g_log.complete = "deny";
    run_script(c, picker_script, 1);
    CHECK(wait_for_title(c, "cancelled"));
    finish(c);
}

static void test_ui_use_default_and_reject_for_a_picker(void) {
    char path[64], name[64], answer[200], want[128];
    make_file(path, sizeof path, name, sizeof name);
    bdk_contents_t c = start_page(BDK_SURFACE_FILE_PICKERS, NULL);
    g_log.response = BDK_UI_USE_DEFAULT;
    snprintf(answer, sizeof answer, "{\"paths\":[\"%s\"]}", path);
    script("file_pickers", answer);
    run_script(c, picker_script, 1);
    snprintf(want, sizeof want, "picked:%s", name);
    CHECK(wait_for_title(c, want));
    CHECK(shown("file_pickers") == 1);
    /* Rejected: the input is cancelled and nothing is shown. */
    run_script(c, "document.title='again'", 0);
    g_log.response = BDK_UI_REJECT;
    run_script(c, picker_script, 1);
    CHECK(wait_for_title(c, "cancelled"));
    CHECK(shown("file_pickers") == 1);
    unlink(path);
    finish(c);
}

static void test_ui_a_disabled_file_picker_does_nothing(void) {
    char path[64], name[64], answer[200];
    make_file(path, sizeof path, name, sizeof name);
    bdk_contents_t c = start_page(BDK_SURFACE_FILE_PICKERS, "{\"file_pickers\":\"disabled\"}");
    snprintf(answer, sizeof answer, "{\"paths\":[\"%s\"]}", path);
    script("file_pickers", answer);
    run_script(c, picker_script, 1);
    CHECK(wait_for_title(c, "cancelled"));
    CHECK(g_log.calls == 0);
    CHECK(shown("file_pickers") == 0);
    unlink(path);
    finish(c);
}

/* ---- File System Access pickers (T-UIR-13) ---- */

static const char* fsa_open_script =
    "showOpenFilePicker({types:[{description:'Text',accept:{'text/plain':['.txt']}}]})"
    ".then(h=>document.title='fsa:'+h[0].name,e=>document.title='fsa-error:'+e.name)";

static const char* fsa_directory_script =
    "showDirectoryPicker().then(h=>document.title='dir:'+h.name,"
    "e=>document.title='fsa-error:'+e.name)";

static const char* fsa_save_script =
    "showSaveFilePicker({suggestedName:'notes.txt'}).then(h=>document.title='saved:'+h.name,"
    "e=>document.title='fsa-error:'+e.name)";

static void test_ui_a_file_system_access_picker_with_no_handler_uses_the_default_picker(void) {
    char path[64], name[64], answer[200], want[128];
    make_file(path, sizeof path, name, sizeof name);
    bdk_contents_t c = start_page(0, NULL);
    snprintf(answer, sizeof answer, "{\"paths\":[\"%s\"]}", path);
    script("file_pickers", answer);
    run_script(c, fsa_open_script, 1);
    snprintf(want, sizeof want, "fsa:%s", name);
    CHECK(wait_for_title(c, want));
    char out[4096];
    last_shown("file_pickers", out, sizeof out);
    CHECK(strstr(out, "isFileSystemAccessAPI\\\":true") != NULL);
    unlink(path);
    finish(c);
}

static void test_ui_a_handler_answers_a_file_system_access_picker(void) {
    char path[64], name[64], args[200], want[128];
    make_file(path, sizeof path, name, sizeof name);
    bdk_contents_t c = start_page(BDK_SURFACE_FILE_PICKERS, NULL);
    snprintf(args, sizeof args, "{\"paths\":[\"%s\"]}", path);
    g_log.complete = "accept";
    g_log.complete_args = args;
    run_script(c, fsa_open_script, 1);
    snprintf(want, sizeof want, "fsa:%s", name);
    CHECK(wait_for_title(c, want));
    CHECK(strstr(g_log.payload, "\"mode\":\"open\"") != NULL);
    CHECK(strstr(g_log.payload, "\"isFileSystemAccessAPI\":true") != NULL);
    CHECK(strstr(g_log.payload, "\".txt\"") != NULL);
    CHECK(strstr(g_log.payload, "\"origin\":\"https://bdk.test\"") != NULL);
    unlink(path);
    finish(c);
}

static void test_ui_a_handler_answers_the_directory_and_save_pickers(void) {
    char dir[64] = "/tmp/bdk-pick-dir-XXXXXX", args[200], want[128];
    REQUIRE(mkdtemp(dir) != NULL);
    bdk_contents_t c = start_page(BDK_SURFACE_FILE_PICKERS, NULL);
    /* Chromium then asks whether the site may view the folder: a tab-modal model BDK shows as a
       sheet of browser_dialogs. */
    script("browser_dialogs", "{\"answer\":\"accept\"}");
    snprintf(args, sizeof args, "{\"paths\":[\"%s\"]}", dir);
    g_log.complete = "accept";
    g_log.complete_args = args;
    run_script(c, fsa_directory_script, 1);
    snprintf(want, sizeof want, "dir:%s", strrchr(dir, '/') + 1);
    CHECK(wait_for_title(c, want));
    CHECK(strstr(g_log.payload, "\"mode\":\"directory\"") != NULL);
    char file[128];
    snprintf(file, sizeof file, "%s/notes.txt", dir);
    snprintf(args, sizeof args, "{\"paths\":[\"%s\"]}", file);
    run_script(c, fsa_save_script, 1);
    CHECK(wait_for_title(c, "saved:notes.txt"));
    CHECK(strstr(g_log.payload, "\"mode\":\"save\"") != NULL);
    CHECK(strstr(g_log.payload, "\"suggestedName\":\"notes.txt\"") != NULL);
    unlink(file);
    rmdir(dir);
    finish(c);
}

static void test_ui_a_cancelled_file_system_access_picker_rejects(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_FILE_PICKERS, NULL);
    g_log.complete = "deny";
    run_script(c, fsa_open_script, 1);
    CHECK(wait_for_title(c, "fsa-error:AbortError"));
    finish(c);
}

static void test_ui_a_disabled_file_system_access_picker_rejects(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_FILE_PICKERS, "{\"file_pickers\":\"disabled\"}");
    run_script(c, fsa_open_script, 1);
    CHECK(wait_for_title(c, "fsa-error:AbortError"));
    CHECK(g_log.calls == 0);
    CHECK(shown("file_pickers") == 0);
    finish(c);
}

/* ---- permissions ---- */

static const char* ask_notifications = "Notification.requestPermission().then(p=>document.title=p)";

static const char* setting_of(bdk_contents_t c, const char* kind, char* out, size_t n) {
    char extra[200];
    snprintf(extra, sizeof extra, "\"origin\":\"https://bdk.test\",\"kind\":\"%s\"", kind);
    page_ui(c, "permission.get", extra, out, n);
    return out;
}

static int setting_is(bdk_contents_t c, const char* kind, const char* value) {
    char out[128], want[64];
    setting_of(c, kind, out, sizeof out);
    snprintf(want, sizeof want, "\"value\":\"%s\"", value);
    return strstr(out, want) != NULL;
}

static void test_ui_allowing_a_permission_in_the_default_prompt_grants_and_persists(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("permissions", "{\"choice\":\"allow\"}");
    run_script(c, ask_notifications, 1);
    CHECK(wait_for_title(c, "granted"));
    CHECK(setting_is(c, "notifications", "allow"));
    char out[4096];
    last_shown("permissions", out, sizeof out);
    CHECK(count_of(out, "shown") == 1);
    /* The origin and the wording a handler would get. */
    CHECK(strstr(out, "https://bdk.test") != NULL);
    CHECK(strstr(out, "kinds\\\":[\\\"notifications\\\"]") != NULL);
    CHECK(strstr(out, "wants to") != NULL);
    /* Another page of the profile is not asked again. */
    bdk_contents_t d = second_page(g_profile, "https://bdk.test/other");
    run_script(d, "document.title=Notification.permission", 0);
    CHECK(wait_for_title(d, "granted"));
    CHECK(shown("permissions") == 1);
    finish(c);
    finish(d);
}

static void test_ui_blocking_a_permission_in_the_default_prompt_denies_and_persists(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("permissions", "{\"choice\":\"block\"}");
    run_script(c, ask_notifications, 1);
    CHECK(wait_for_title(c, "denied"));
    CHECK(setting_is(c, "notifications", "block"));
    finish(c);
}

static void test_ui_dismissing_the_default_prompt_decides_nothing(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("permissions", "{\"choice\":\"dismiss\"}");
    run_script(c, ask_notifications, 1);
    CHECK(wait_for_title(c, "default"));
    CHECK(setting_is(c, "notifications", "ask"));
    finish(c);
}

static void test_ui_a_handler_grants_with_a_gesture(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_PERMISSIONS, NULL);
    g_log.complete = "grant";
    g_log.complete_args = "{\"kinds\":[\"notifications\"],\"duration\":\"persistent\"}";
    g_log.gesture_kind = 0;
    run_script(c, ask_notifications, 1);
    CHECK(wait_for_title(c, "granted"));
    CHECK(setting_is(c, "notifications", "allow"));
    CHECK(strstr(g_log.payload, "\"origin\":\"https://bdk.test\"") != NULL);
    CHECK(strstr(g_log.payload, "\"kinds\":[\"notifications\"]") != NULL);
    CHECK(strstr(g_log.payload, "\"text\":{") != NULL);
    finish(c);
}

static void test_ui_a_key_down_also_confirms_a_grant(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_PERMISSIONS, NULL);
    g_log.complete = "grant";
    g_log.complete_args = "{\"kinds\":[\"notifications\"],\"duration\":\"persistent\"}";
    g_log.gesture_kind = 1;
    run_script(c, ask_notifications, 1);
    CHECK(wait_for_title(c, "granted"));
    finish(c);
}

static void test_ui_the_core_refuses_a_grant_with_no_gesture(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_PERMISSIONS, NULL);
    run_script(c, ask_notifications, 1);
    REQUIRE(g_log.calls == 1);
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_log.request, S("grant"), S("{\"kinds\":[\"notifications\"]}"), none,
                          &err) != BDK_OK);
    CHECK(t_error_is(err, "ui", "gestureRequired"));
    /* Still open: nothing was granted. */
    CHECK(setting_is(c, "notifications", "ask"));
    bdk_native_handle_t no_event;
    memset(&no_event, 0, sizeof no_event);
    REQUIRE(bdk_ui_complete(g_log.request, S("deny"), S("{\"persist\":true}"), no_event, &err) ==
            BDK_OK);
    CHECK(wait_for_title(c, "denied"));
    finish(c);
}

static void grant_with_a_gesture_that_is_not_one(int kind) {
    bdk_contents_t c = start_page(BDK_SURFACE_PERMISSIONS, NULL);
    g_log.complete = "grant";
    g_log.complete_args = "{\"kinds\":[\"notifications\"],\"duration\":\"persistent\"}";
    g_log.gesture_kind = kind;
    run_script(c, ask_notifications, 1);
    /* The core accepted an event; the engine did not accept it as a person's: no grant. */
    CHECK(wait_for_title(c, "default"));
    CHECK(setting_is(c, "notifications", "ask"));
    finish(c);
}

static void test_ui_a_stale_event_does_not_grant(void) {
    grant_with_a_gesture_that_is_not_one(2);
}
static void test_ui_a_mouse_down_does_not_grant(void) {
    grant_with_a_gesture_that_is_not_one(3);
}
static void test_ui_an_event_for_no_window_of_the_application_does_not_grant(void) {
    grant_with_a_gesture_that_is_not_one(4);
}

static void test_ui_a_grant_cannot_widen_what_was_asked(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_PERMISSIONS, NULL);
    run_script(c, ask_notifications, 1);
    REQUIRE(g_log.calls == 1);
    bdk_native_handle_t gesture = bdk_harness_engine_gesture(c, 0);
    bdk_error_t err = 0;
    CHECK(bdk_ui_complete(g_log.request, S("grant"),
                          S("{\"kinds\":[\"notifications\",\"camera\"]}"), gesture,
                          &err) != BDK_OK);
    CHECK(t_error_is(err, "ui", "widensRequest"));
    CHECK(setting_is(c, "notifications", "ask"));
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    REQUIRE(bdk_ui_complete(g_log.request, S("deny"), S("{\"persist\":false}"), none, &err) ==
            BDK_OK);
    finish(c);
}

static void test_ui_use_default_for_a_permission_shows_the_default_prompt(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_PERMISSIONS, NULL);
    g_log.response = BDK_UI_USE_DEFAULT;
    script("permissions", "{\"choice\":\"allow\"}");
    run_script(c, ask_notifications, 1);
    CHECK(wait_for_title(c, "granted"));
    CHECK(g_log.calls == 1);
    CHECK(shown("permissions") == 1);
    finish(c);
}

static void test_ui_a_rejected_permission_is_not_granted(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_PERMISSIONS, NULL);
    g_log.response = BDK_UI_REJECT;
    script("permissions", "{\"choice\":\"allow\"}");
    run_script(c, ask_notifications, 1);
    CHECK(wait_for_title(c, "default"));
    CHECK(setting_is(c, "notifications", "ask"));
    CHECK(shown("permissions") == 0);
    finish(c);
}

static void test_ui_a_handler_denies_a_permission_for_good(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_PERMISSIONS, NULL);
    g_log.complete = "deny";
    g_log.complete_args = "{\"persist\":true}";
    run_script(c, ask_notifications, 1);
    CHECK(wait_for_title(c, "denied"));
    CHECK(setting_is(c, "notifications", "block"));
    finish(c);
}

static void test_ui_a_page_that_goes_withdraws_its_permission_request(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_PERMISSIONS, NULL);
    run_script(c, ask_notifications, 1);
    REQUIRE(g_log.calls == 1);
    bdk_ui_set_withdrawn_handler(g_log.request, on_withdrawn, NULL, NULL);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("https://bdk.test/elsewhere"), &err) == BDK_OK);
    pumps(5);
    CHECK(g_withdrawn == 1);
    finish(c);
}

static void test_ui_the_default_prompt_is_taken_down_when_withdrawn(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("permissions", "{\"none\":true}");
    run_script(c, ask_notifications, 1);
    pumps(3);
    CHECK(shown("permissions") == 1);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("https://bdk.test/elsewhere"), &err) == BDK_OK);
    pumps(5);
    CHECK(dismissed("permissions") == 1);
    finish(c);
}

static const char* ask_geolocation =
    "navigator.geolocation.getCurrentPosition(()=>{},()=>{document.title='geo-error'})";

static void test_ui_permissions_are_kept_per_profile_and_a_private_one_stays_separate(void) {
    bdk_contents_t c = start_page(0, NULL);
    script("permissions", "{\"choice\":\"allow\"}");
    run_script(c, ask_geolocation, 1);
    pumps(5);
    CHECK(shown("permissions") == 1);
    CHECK(setting_is(c, "geolocation", "allow"));
    char out[4096];
    last_shown("permissions", out, sizeof out);
    CHECK(strstr(out, "kinds\\\":[\\\"geolocation\\\"]") != NULL);

    bdk_profile_t priv = 0;
    bdk_error_t err = 0;
    REQUIRE(bdk_profile_open_private(t_runtime, &priv, &err) == BDK_OK);
    bdk_contents_t p = second_page(priv, PAGE);
    /* What the persistent profile granted is not the private one's. */
    CHECK(setting_is(p, "geolocation", "ask"));
    script("permissions", "{\"choice\":\"block\"}");
    run_script(p, ask_geolocation, 1);
    pumps(5);
    CHECK(shown("permissions") == 1);
    CHECK(setting_is(p, "geolocation", "block"));
    /* And what the private one decided does not reach the persistent one. */
    CHECK(setting_is(c, "geolocation", "allow"));
    finish(c);
    finish(p);
}

/* ---- find in page ---- */

static void find_text_page(bdk_contents_t c) {
    run_script(c, "document.body.innerText='alpha beta alpha gamma alpha'", 0);
}

static void find_result(bdk_contents_t c, int* ordinal, int* count, int* presented) {
    char out[256];
    page_ui(c, "find.result", "", out, sizeof out);
    *ordinal = count_of(out, "ordinal");
    *count = count_of(out, "count");
    *presented = strstr(out, "\"presented\":true") != NULL;
}

static int wait_for_find(bdk_contents_t c, int want_ordinal, int want_count) {
    int ordinal = 0, count = 0, presented = 0;
    for (int i = 0; i < 60; ++i) {
        find_result(c, &ordinal, &count, &presented);
        if (ordinal == want_ordinal && count == want_count)
            return 1;
        t_pump();
    }
    printf("    find result is %d of %d, not %d of %d\n", ordinal, count, want_ordinal, want_count);
    return 0;
}

static void test_ui_find_with_no_handler_shows_the_default_find_bar_and_searches(void) {
    bdk_contents_t c = start_page(0, NULL);
    find_text_page(c);
    script("find", "{}");
    page_ui(c, "find.present", "", NULL, 0);
    pumps(3);
    CHECK(shown("find") == 1);
    page_ui(c, "find.start", "\"text\":\"alpha\"", NULL, 0);
    CHECK(wait_for_find(c, 1, 3));
    page_ui(c, "find.next", "\"forward\":true", NULL, 0);
    CHECK(wait_for_find(c, 2, 3));
    page_ui(c, "find.next", "\"forward\":false", NULL, 0);
    CHECK(wait_for_find(c, 1, 3));
    page_ui(c, "find.stop", "", NULL, 0);
    int ordinal, count, presented;
    find_result(c, &ordinal, &count, &presented);
    CHECK(count == 0);
    finish(c);
}

static void test_ui_the_find_request_offers_the_last_search_as_initial_text(void) {
    bdk_contents_t c = start_page(0, NULL);
    find_text_page(c);
    script("find", "{}");
    page_ui(c, "find.start", "\"text\":\"gamma\"", NULL, 0);
    CHECK(wait_for_find(c, 1, 1));
    page_ui(c, "find.present", "", NULL, 0);
    pumps(3);
    char out[4096];
    last_shown("find", out, sizeof out);
    CHECK(strstr(out, "initialText\\\":\\\"gamma") != NULL);
    finish(c);
}

static void test_ui_a_find_handler_shows_its_own_bar_and_closes_it(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_FIND, NULL);
    find_text_page(c);
    script("find", "{}");
    page_ui(c, "find.present", "", NULL, 0);
    pumps(3);
    CHECK(g_log.calls == 1);
    CHECK(shown("find") == 0);
    int ordinal, count, presented;
    find_result(c, &ordinal, &count, &presented);
    CHECK(presented);
    /* Asking again while it is open raises nothing. */
    page_ui(c, "find.present", "", NULL, 0);
    pumps(3);
    CHECK(g_log.calls == 1);
    page_ui(c, "find.start", "\"text\":\"alpha\"", NULL, 0);
    CHECK(wait_for_find(c, 1, 3));
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    bdk_error_t err = 0;
    REQUIRE(bdk_ui_complete(g_log.request, S("accept"), S("{\"action\":\"close\"}"), none, &err) ==
            BDK_OK);
    pumps(3);
    find_result(c, &ordinal, &count, &presented);
    CHECK(!presented);
    CHECK(count == 0);
    finish(c);
}

static void test_ui_use_default_for_find_shows_the_default_bar(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_FIND, NULL);
    g_log.response = BDK_UI_USE_DEFAULT;
    script("find", "{}");
    page_ui(c, "find.present", "", NULL, 0);
    pumps(3);
    CHECK(g_log.calls == 1);
    CHECK(shown("find") == 1);
    finish(c);
}

static void test_ui_a_rejected_find_shows_nothing(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_FIND, NULL);
    g_log.response = BDK_UI_REJECT;
    script("find", "{}");
    page_ui(c, "find.present", "", NULL, 0);
    pumps(3);
    CHECK(shown("find") == 0);
    int ordinal, count, presented;
    find_result(c, &ordinal, &count, &presented);
    CHECK(!presented);
    finish(c);
}

static void test_ui_navigating_away_closes_the_find_ui(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_FIND, NULL);
    page_ui(c, "find.present", "", NULL, 0);
    pumps(3);
    REQUIRE(g_log.calls == 1);
    bdk_ui_set_withdrawn_handler(g_log.request, on_withdrawn, NULL, NULL);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S("https://bdk.test/elsewhere"), &err) == BDK_OK);
    pumps(5);
    CHECK(g_withdrawn == 1);
    int ordinal, count, presented;
    find_result(c, &ordinal, &count, &presented);
    CHECK(!presented);
    finish(c);
}

/* ---- find in page started by the application (T-SWF-07) ---- */

typedef struct {
    int results;
    char last[512];
} find_events_t;

static find_events_t g_find;

static void on_find_event(void* ctx, const bdk_contents_event_t* event) {
    find_events_t* events = (find_events_t*)ctx;
    if (event->kind != BDK_CONTENTS_EVENT_FIND_RESULT)
        return;
    ++events->results;
    snprintf(events->last, sizeof events->last, "%.*s", (int)event->detail_json.length,
             event->detail_json.data ? event->detail_json.data : "");
}

static void watch_find(bdk_contents_t c) {
    memset(&g_find, 0, sizeof g_find);
    REQUIRE(bdk_contents_subscribe(c, on_find_event, &g_find, NULL) != 0);
}

/* Waits for a final result with the given count and active match (-1: any active match). */
static int wait_for_find_event(int want_count, int want_active) {
    for (int i = 0; i < 80; ++i) {
        if (g_find.results > 0 && strstr(g_find.last, "\"final\":true") != NULL &&
            count_of(g_find.last, "matchCount") == want_count &&
            (want_active < 0 ? count_of(g_find.last, "activeIndex") > 0
                             : count_of(g_find.last, "activeIndex") == want_active))
            return 1;
        t_pump();
    }
    printf("    last find result is %s, not %d matches with %d active\n", g_find.last, want_count,
           want_active);
    return 0;
}

static void test_ui_the_application_finds_matches_moves_the_active_one_and_stops(void) {
    bdk_contents_t c = start_page(0, NULL);
    find_text_page(c);
    watch_find(c);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_find_start(c, S("alpha"), NULL, &err) == BDK_OK);
    CHECK(wait_for_find_event(3, 1));
    CHECK(strstr(g_find.last, "\"rect\":{") != NULL);
    REQUIRE(bdk_contents_find_next(c, &err) == BDK_OK);
    CHECK(wait_for_find_event(3, 2));
    REQUIRE(bdk_contents_find_next(c, &err) == BDK_OK);
    CHECK(wait_for_find_event(3, 3));
    REQUIRE(bdk_contents_find_previous(c, &err) == BDK_OK);
    CHECK(wait_for_find_event(3, 2));
    /* Case matters only when asked. */
    REQUIRE(bdk_contents_find_start(c, S("ALPHA"), NULL, &err) == BDK_OK);
    CHECK(wait_for_find_event(3, -1));
    bdk_find_options_t exact;
    memset(&exact, 0, sizeof exact);
    exact.struct_size = sizeof exact;
    exact.match_case = 1;
    REQUIRE(bdk_contents_find_start(c, S("ALPHA"), &exact, &err) == BDK_OK);
    CHECK(wait_for_find_event(0, 0));
    REQUIRE(bdk_contents_find_start(c, S("gamma"), NULL, &err) == BDK_OK);
    CHECK(wait_for_find_event(1, 1));
    REQUIRE(bdk_contents_find_stop(c, 1, &err) == BDK_OK);
    pumps(5);
    const int after_stop = g_find.results;
    REQUIRE(bdk_contents_find_next(c, &err) == BDK_OK); /* no search: nothing happens */
    pumps(10);
    CHECK(g_find.results == after_stop);
    int ordinal, count, presented;
    find_result(c, &ordinal, &count, &presented);
    CHECK(count == 0);
    finish(c);
}

static void test_ui_the_application_presents_find_for_a_handler(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_FIND, NULL);
    find_text_page(c);
    watch_find(c);
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_find_start(c, S("beta"), NULL, &err) == BDK_OK);
    CHECK(wait_for_find_event(1, 1));
    REQUIRE(bdk_contents_find_present(c, &err) == BDK_OK);
    pumps(5);
    REQUIRE(g_log.calls == 1);
    CHECK(strstr(g_log.payload, "initialText\":\"beta") != NULL);
    CHECK(shown("find") == 0);
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    REQUIRE(bdk_ui_complete(g_log.request, S("accept"), S("{\"action\":\"close\"}"), none, &err) ==
            BDK_OK);
    pumps(3);
    int ordinal, count, presented;
    find_result(c, &ordinal, &count, &presented);
    CHECK(!presented);
    finish(c);
}

/* ---- fullscreen ---- */

static const char* fullscreen_script =
    "document.onfullscreenchange=()=>{document.title=document.fullscreenElement?'in':'out'};"
    "document.documentElement.requestFullscreen().catch(()=>{document.title='rejected'})";

static void test_ui_fullscreen_with_no_handler_uses_the_default_presenter(void) {
    bdk_contents_t c = start_page(0, NULL);
    run_script(c, fullscreen_script, 1);
    CHECK(wait_for_title(c, "in"));
    run_script(c, "document.exitFullscreen()", 1);
    CHECK(wait_for_title(c, "out"));
    finish(c);
}

static void test_ui_a_disabled_fullscreen_surface_rejects_the_api(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_FULLSCREEN, "{\"fullscreen\":\"disabled\"}");
    run_script(c, fullscreen_script, 1);
    CHECK(wait_for_title(c, "rejected"));
    CHECK(g_log.calls == 0);
    finish(c);
}

/* ---- exclusive access (5.12, T-UIR-15) ---- */

static const char* pointer_lock_script =
    "document.onpointerlockchange=()=>{document.title=document.pointerLockElement?'locked':"
    "'unlocked'};document.onpointerlockerror=()=>{document.title='refused'};"
    "document.body.style.height='400px';document.body.requestPointerLock()";

int bdk_harness_engine_window_is_key(bdk_contents_t contents);

/* Pointer lock and the keyboard reach only the key window (RenderWidgetHostViewMac): with
   BDK_ENGINE_SILENT the harness is never active, so these scenarios say so and stop. */
static int needs_key_window(bdk_contents_t c) {
    /* BDK_TEST_WAIT_FOR_KEY: someone clicks the harness window to make it key (macOS does not
       let an application bring itself forward). */
    const int wait = getenv("BDK_TEST_WAIT_FOR_KEY") != NULL;
    if (wait)
        printf("    waiting for the harness window to be clicked\n");
    for (int i = 0; wait && i < 800 && !bdk_harness_engine_window_is_key(c); ++i) {
        t_pump();
        bdk_testing_run_for(0.05);
    }
    if (bdk_harness_engine_window_is_key(c))
        return 1;
    printf("    skipped: the harness window is not the key window\n");
    finish(c);
    return 0;
}

/* Sends Escape through the page's render widget host, as a key press reaches it. */
static void press_escape(bdk_contents_t c) {
    char args[96];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"key\":\"Escape\",\"type\":\"down\"}",
             (unsigned long long)c);
    ui("input.key", args, NULL, 0);
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"key\":\"Escape\",\"type\":\"up\"}",
             (unsigned long long)c);
    ui("input.key", args, NULL, 0);
    t_pump();
}

/* A pointer lock with the gesture locks and raises the notice with Chromium's instruction, which
   the handler acknowledges; Escape unlocks it, as BDK keeps Escape for itself. */
static void test_ui_a_pointer_lock_raises_the_notice_and_escape_unlocks(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_EXCLUSIVE_ACCESS, NULL);
    if (!needs_key_window(c))
        return;
    g_log.complete = "accept";
    run_script(c, pointer_lock_script, 1);
    CHECK(wait_for_title(c, "locked"));
    CHECK(g_log.calls == 1);
    CHECK(strstr(g_log.payload, "\"kind\":\"pointerLock\"") != NULL);
    CHECK(strstr(g_log.payload, "\"origin\":\"https://bdk.test\"") != NULL);
    CHECK(strstr(g_log.payload, "Esc") != NULL);
    press_escape(c);
    CHECK(wait_for_title(c, "unlocked"));
    finish(c);
}

/* Without a gesture the page is refused, and no notice is raised. */
static void test_ui_a_pointer_lock_without_a_gesture_is_refused(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_EXCLUSIVE_ACCESS, NULL);
    g_log.complete = "accept";
    run_script(c, pointer_lock_script, 0);
    CHECK(wait_for_title(c, "refused"));
    CHECK(g_log.calls == 0);
    finish(c);
}

/* With no handler the default presenter shows the instruction. */
static void test_ui_the_default_presenter_shows_the_exit_instruction(void) {
    bdk_contents_t c = start_page(0, NULL);
    if (!needs_key_window(c))
        return;
    script("exclusive_access", "{\"none\":true}");
    run_script(c, pointer_lock_script, 1);
    CHECK(wait_for_title(c, "locked"));
    for (int i = 0; i < 20 && shown("exclusive_access") == 0; ++i)
        t_pump();
    CHECK(shown("exclusive_access") == 1);
    run_script(c, "document.exitPointerLock()", 0);
    CHECK(wait_for_title(c, "unlocked"));
    finish(c);
}

/* A keyboard lock of a page in fullscreen raises the notice; one that includes Escape exits
   fullscreen when Escape is held. */
static void test_ui_a_keyboard_lock_in_fullscreen_raises_the_notice(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_EXCLUSIVE_ACCESS, NULL);
    if (!needs_key_window(c))
        return;
    g_log.complete = "accept";
    run_script(c, fullscreen_script, 1);
    REQUIRE(wait_for_title(c, "in"));
    run_script(c,
               "navigator.keyboard.lock(['Escape']).then(()=>{document.title='keys'},"
               "e=>{document.title='keys '+e.name})",
               1);
    CHECK(wait_for_title(c, "keys"));
    for (int i = 0; i < 20 && g_log.calls == 0; ++i)
        t_pump();
    CHECK(strstr(g_log.payload, "\"kind\":\"keyboardLock\"") != NULL);
    char args[96];
    snprintf(args, sizeof args, "{\"contents\":\"%llu\",\"key\":\"Escape\",\"type\":\"down\"}",
             (unsigned long long)c);
    ui("input.key", args, NULL, 0);
    CHECK(wait_for_title(c, "out"));
    finish(c);
}

/* ---- notifications ---- */

static const char* show_notification =
    "var n=new Notification('Hello',{body:'World'});"
    "n.onclick=()=>document.title='clicked';n.onclose=()=>document.title='closed'";

/* Grants the page notifications through the default permission prompt, then shows one. */
static bdk_contents_t notification_page(uint32_t surface, const char* config) {
    bdk_contents_t c = start_page(surface, config);
    script("permissions", "{\"choice\":\"allow\"}");
    run_script(c, ask_notifications, 1);
    REQUIRE(wait_for_title(c, "granted"));
    return c;
}

static void test_ui_a_notification_with_no_handler_is_shown_by_the_default_presenter(void) {
    bdk_contents_t c = notification_page(0, NULL);
    script("notifications", "{\"event\":\"clicked\"}");
    run_script(c, show_notification, 0);
    CHECK(wait_for_title(c, "clicked"));
    char out[4096];
    last_shown("notifications", out, sizeof out);
    CHECK(count_of(out, "shown") == 1);
    CHECK(strstr(out, "Hello") != NULL);
    CHECK(strstr(out, "World") != NULL);
    CHECK(strstr(out, "https://bdk.test") != NULL);
    finish(c);
}

static void test_ui_closing_the_default_notification_fires_onclose(void) {
    bdk_contents_t c = notification_page(0, NULL);
    script("notifications", "{\"event\":\"closed\"}");
    run_script(c, show_notification, 0);
    CHECK(wait_for_title(c, "closed"));
    finish(c);
}

static void test_ui_a_handler_reports_a_notification_clicked(void) {
    bdk_contents_t c = notification_page(BDK_SURFACE_NOTIFICATIONS, NULL);
    g_log.complete = "accept";
    g_log.complete_args = "{\"event\":\"clicked\"}";
    run_script(c, show_notification, 0);
    CHECK(wait_for_title(c, "clicked"));
    CHECK(g_log.calls == 1);
    CHECK(strstr(g_log.payload, "\"title\":\"Hello\"") != NULL);
    CHECK(strstr(g_log.payload, "\"body\":\"World\"") != NULL);
    CHECK(strstr(g_log.payload, "\"origin\":\"https://bdk.test\"") != NULL);
    CHECK(strstr(g_log.payload, "\"requireInteraction\":false") != NULL);
    CHECK(shown("notifications") == 0);
    finish(c);
}

static void test_ui_a_handler_reports_a_notification_closed(void) {
    bdk_contents_t c = notification_page(BDK_SURFACE_NOTIFICATIONS, NULL);
    g_log.complete = "deny";
    g_log.complete_args = "{\"event\":\"closed\",\"byUser\":true}";
    run_script(c, show_notification, 0);
    CHECK(wait_for_title(c, "closed"));
    finish(c);
}

static void test_ui_use_default_for_a_notification_shows_the_default(void) {
    bdk_contents_t c = notification_page(BDK_SURFACE_NOTIFICATIONS, NULL);
    g_log.response = BDK_UI_USE_DEFAULT;
    script("notifications", "{\"event\":\"clicked\"}");
    run_script(c, show_notification, 0);
    CHECK(wait_for_title(c, "clicked"));
    CHECK(shown("notifications") == 1);
    finish(c);
}

static void test_ui_a_rejected_notification_is_closed_without_being_shown(void) {
    bdk_contents_t c = notification_page(BDK_SURFACE_NOTIFICATIONS, NULL);
    g_log.response = BDK_UI_REJECT;
    run_script(c, show_notification, 0);
    CHECK(wait_for_title(c, "closed"));
    CHECK(shown("notifications") == 0);
    finish(c);
}

static void test_ui_disabled_notifications_report_permission_denied(void) {
    bdk_contents_t c = start_page(BDK_SURFACE_NOTIFICATIONS, "{\"notifications\":\"disabled\"}");
    script("permissions", "{\"choice\":\"allow\"}");
    run_script(c, ask_notifications, 1);
    CHECK(wait_for_title(c, "denied"));
    CHECK(g_log.calls == 0);
    CHECK(shown("permissions") == 0);
    finish(c);
}

const t_test_t t_engine_ui_tests[] = {
    {"engine_ui_a_prompt_with_no_handler_is_answered_by_the_default_presenter",
     test_ui_a_prompt_with_no_handler_is_answered_by_the_default_presenter},
    {"engine_ui_a_cancelled_confirm_returns_false", test_ui_a_cancelled_confirm_returns_false},
    {"engine_ui_a_handler_completes_a_dialog", test_ui_a_handler_completes_a_dialog},
    {"engine_ui_a_handler_cancels_a_dialog", test_ui_a_handler_cancels_a_dialog},
    {"engine_ui_use_default_shows_the_default_dialog",
     test_ui_use_default_shows_the_default_dialog},
    {"engine_ui_a_rejected_dialog_is_cancelled", test_ui_a_rejected_dialog_is_cancelled},
    {"engine_ui_preventing_further_dialogs_suppresses_the_next",
     test_ui_preventing_further_dialogs_suppresses_the_next},
    {"engine_ui_navigating_away_withdraws_and_dismisses_a_dialog",
     test_ui_navigating_away_withdraws_and_dismisses_a_dialog},
    {"engine_ui_the_default_dialog_is_a_sheet_on_the_pages_window",
     test_ui_the_default_dialog_is_a_sheet_on_the_pages_window},
    {"engine_ui_beforeunload_asks_with_chromiums_text",
     test_ui_beforeunload_asks_with_chromiums_text},
    {"engine_ui_a_file_input_with_no_handler_uses_the_default_picker",
     test_ui_a_file_input_with_no_handler_uses_the_default_picker},
    {"engine_ui_cancelling_the_default_picker_cancels_the_input",
     test_ui_cancelling_the_default_picker_cancels_the_input},
    {"engine_ui_a_handler_chooses_files", test_ui_a_handler_chooses_files},
    {"engine_ui_a_handler_chooses_several_files_for_a_multiple_input",
     test_ui_a_handler_chooses_several_files_for_a_multiple_input},
    {"engine_ui_a_handler_cancels_a_picker", test_ui_a_handler_cancels_a_picker},
    {"engine_ui_use_default_and_reject_for_a_picker", test_ui_use_default_and_reject_for_a_picker},
    {"engine_ui_a_disabled_file_picker_does_nothing", test_ui_a_disabled_file_picker_does_nothing},
    {"engine_ui_a_file_system_access_picker_with_no_handler_uses_the_default_picker",
     test_ui_a_file_system_access_picker_with_no_handler_uses_the_default_picker},
    {"engine_ui_a_handler_answers_a_file_system_access_picker",
     test_ui_a_handler_answers_a_file_system_access_picker},
    {"engine_ui_a_handler_answers_the_directory_and_save_pickers",
     test_ui_a_handler_answers_the_directory_and_save_pickers},
    {"engine_ui_a_cancelled_file_system_access_picker_rejects",
     test_ui_a_cancelled_file_system_access_picker_rejects},
    {"engine_ui_a_disabled_file_system_access_picker_rejects",
     test_ui_a_disabled_file_system_access_picker_rejects},
    {"engine_ui_allowing_a_permission_in_the_default_prompt_grants_and_persists",
     test_ui_allowing_a_permission_in_the_default_prompt_grants_and_persists},
    {"engine_ui_blocking_a_permission_in_the_default_prompt_denies_and_persists",
     test_ui_blocking_a_permission_in_the_default_prompt_denies_and_persists},
    {"engine_ui_dismissing_the_default_prompt_decides_nothing",
     test_ui_dismissing_the_default_prompt_decides_nothing},
    {"engine_ui_a_handler_grants_with_a_gesture", test_ui_a_handler_grants_with_a_gesture},
    {"engine_ui_a_key_down_also_confirms_a_grant", test_ui_a_key_down_also_confirms_a_grant},
    {"engine_ui_the_core_refuses_a_grant_with_no_gesture",
     test_ui_the_core_refuses_a_grant_with_no_gesture},
    {"engine_ui_a_stale_event_does_not_grant", test_ui_a_stale_event_does_not_grant},
    {"engine_ui_a_mouse_down_does_not_grant", test_ui_a_mouse_down_does_not_grant},
    {"engine_ui_an_event_for_no_window_of_the_application_does_not_grant",
     test_ui_an_event_for_no_window_of_the_application_does_not_grant},
    {"engine_ui_a_grant_cannot_widen_what_was_asked", test_ui_a_grant_cannot_widen_what_was_asked},
    {"engine_ui_use_default_for_a_permission_shows_the_default_prompt",
     test_ui_use_default_for_a_permission_shows_the_default_prompt},
    {"engine_ui_a_rejected_permission_is_not_granted",
     test_ui_a_rejected_permission_is_not_granted},
    {"engine_ui_a_handler_denies_a_permission_for_good",
     test_ui_a_handler_denies_a_permission_for_good},
    {"engine_ui_a_page_that_goes_withdraws_its_permission_request",
     test_ui_a_page_that_goes_withdraws_its_permission_request},
    {"engine_ui_the_default_prompt_is_taken_down_when_withdrawn",
     test_ui_the_default_prompt_is_taken_down_when_withdrawn},
    {"engine_ui_a_pointer_lock_raises_the_notice_and_escape_unlocks",
     test_ui_a_pointer_lock_raises_the_notice_and_escape_unlocks},
    {"engine_ui_a_pointer_lock_without_a_gesture_is_refused",
     test_ui_a_pointer_lock_without_a_gesture_is_refused},
    {"engine_ui_the_default_presenter_shows_the_exit_instruction",
     test_ui_the_default_presenter_shows_the_exit_instruction},
    {"engine_ui_a_keyboard_lock_in_fullscreen_raises_the_notice",
     test_ui_a_keyboard_lock_in_fullscreen_raises_the_notice},
    {"engine_ui_permissions_are_kept_per_profile_and_a_private_one_stays_separate",
     test_ui_permissions_are_kept_per_profile_and_a_private_one_stays_separate},
    {"engine_ui_find_with_no_handler_shows_the_default_find_bar_and_searches",
     test_ui_find_with_no_handler_shows_the_default_find_bar_and_searches},
    {"engine_ui_the_application_finds_matches_moves_the_active_one_and_stops",
     test_ui_the_application_finds_matches_moves_the_active_one_and_stops},
    {"engine_ui_the_application_presents_find_for_a_handler",
     test_ui_the_application_presents_find_for_a_handler},
    {"engine_ui_the_find_request_offers_the_last_search_as_initial_text",
     test_ui_the_find_request_offers_the_last_search_as_initial_text},
    {"engine_ui_a_find_handler_shows_its_own_bar_and_closes_it",
     test_ui_a_find_handler_shows_its_own_bar_and_closes_it},
    {"engine_ui_use_default_for_find_shows_the_default_bar",
     test_ui_use_default_for_find_shows_the_default_bar},
    {"engine_ui_a_rejected_find_shows_nothing", test_ui_a_rejected_find_shows_nothing},
    {"engine_ui_navigating_away_closes_the_find_ui", test_ui_navigating_away_closes_the_find_ui},
    {"engine_ui_fullscreen_with_no_handler_uses_the_default_presenter",
     test_ui_fullscreen_with_no_handler_uses_the_default_presenter},
    {"engine_ui_a_disabled_fullscreen_surface_rejects_the_api",
     test_ui_a_disabled_fullscreen_surface_rejects_the_api},
    {"engine_ui_a_notification_with_no_handler_is_shown_by_the_default_presenter",
     test_ui_a_notification_with_no_handler_is_shown_by_the_default_presenter},
    {"engine_ui_closing_the_default_notification_fires_onclose",
     test_ui_closing_the_default_notification_fires_onclose},
    {"engine_ui_a_handler_reports_a_notification_clicked",
     test_ui_a_handler_reports_a_notification_clicked},
    {"engine_ui_a_handler_reports_a_notification_closed",
     test_ui_a_handler_reports_a_notification_closed},
    {"engine_ui_use_default_for_a_notification_shows_the_default",
     test_ui_use_default_for_a_notification_shows_the_default},
    {"engine_ui_a_rejected_notification_is_closed_without_being_shown",
     test_ui_a_rejected_notification_is_closed_without_being_shown},
    {"engine_ui_disabled_notifications_report_permission_denied",
     test_ui_disabled_notifications_report_permission_denied},
    {NULL, NULL},
};
