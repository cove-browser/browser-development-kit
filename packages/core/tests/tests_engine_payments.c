/* Engine scenarios for Payment Request (ui-requests.md 5.22, T-UIR-14): a shop page asks to pay
   with a payment method whose payment handler, a service worker, Chromium installs just in time
   from the method's manifest, all on the loopback server. A payments request names the total and
   the app; a grant pays through the handler, which answers at once. Built only into the engine
   conformance app (BDK_HARNESS_ENGINE_TESTS). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdk_testing.h"
#include "engine_http_server.h"
#include "test_support.h"

void bdk_harness_engine_show(bdk_contents_t contents, int visible);
bdk_native_handle_t bdk_harness_engine_gesture(bdk_contents_t contents, int kind);

/* The payment method /pay points at its manifest, which names the handler's web app manifest.
   The handler is a service worker that opens its own page and pays, with a token, when the
   person presses Pay there: Chromium resolves a payment only after the person has used the
   handler's page (kPaymentRequestMandatoryPaymentAppUi). */
static const char kMethodHeaders[] =
    "Link: </pay/manifest.json>; rel=\"payment-method-manifest\"\r\n";
static const char kMethodManifest[] = "{\"default_applications\":[\"app.json\"]}";
static const char kAppManifest[] =
    "{\"name\":\"Test Pay\",\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"192x192\","
    "\"type\":\"image/png\"}],\"serviceworker\":{\"src\":\"sw.js\",\"scope\":\"/pay/\","
    "\"use_cache\":false}}";
static const char kHandler[] =
    "let pending;"
    "self.addEventListener('paymentrequest', e => {"
    " e.respondWith(new Promise(resolve => pending = {resolve, method:"
    " e.methodData[0].supportedMethods}));"
    " e.openWindow('/pay/confirm.html'); });"
    "self.addEventListener('message', m => { if (m.data === 'pay' && pending)"
    " pending.resolve({methodName: pending.method, details: {token: 'paid-ok'}}); });";
static const char kConfirm[] =
    "<html><head><title>confirm</title></head><body style=\"margin:0\">"
    "<button id=\"pay\" style=\"position:fixed;left:0;top:0;width:100%;height:100%\">Pay</button>"
    "<script>pay.onclick = () => navigator.serviceWorker.ready.then(r =>"
    " r.active.postMessage('pay'));</script></body></html>";
static const char kShop[] = "<html><head><title>shop</title></head><body>shop</body></html>";

static int start_server(void) {
    static unsigned char icon[1024];
    const size_t icon_length = t_http_test_icon(icon, sizeof icon);
    const t_http_route_t routes[] = {
        {"/shop.html", "text/html", kShop, sizeof kShop - 1, NULL},
        {"/pay", "text/html", kShop, sizeof kShop - 1, kMethodHeaders},
        {"/pay/manifest.json", "application/json", kMethodManifest, sizeof kMethodManifest - 1,
         NULL},
        {"/pay/app.json", "application/manifest+json", kAppManifest, sizeof kAppManifest - 1, NULL},
        {"/pay/sw.js", "text/javascript", kHandler, sizeof kHandler - 1, NULL},
        {"/pay/confirm.html", "text/html", kConfirm, sizeof kConfirm - 1, NULL},
        {"/icon.png", "image/png", icon, icon_length, NULL},
    };
    return t_http_start(routes, 7);
}

static bdk_contents_t g_page;
static bdk_contents_t g_handler_page; /* the payment handler's own page, once it opens */
static char g_window[4096];
static int g_calls;
static char g_payload[8192];
static const char* g_complete; /* "grant", "deny" or NULL to take the default */

static void control(const char* verb, const char* args, char* out, size_t n) {
    REQUIRE(bdk_testing_command(t_runtime, S(verb), S(args), out, n) == BDK_OK);
}

static bdk_ui_response_t on_payment(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    ++g_calls;
    snprintf(g_payload, sizeof g_payload, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    if (!g_complete)
        return BDK_UI_USE_DEFAULT;
    bdk_error_t err = 0;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    const int grant = strcmp(g_complete, "grant") == 0;
    CHECK(bdk_ui_complete(info->request, S(g_complete), S(grant ? "{\"app\":\"app0\"}" : "{}"),
                          grant ? bdk_harness_engine_gesture(g_page, 0) : none, &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

/* The new-window request of the handler's page: accepted, as an application places it. */
static bdk_ui_response_t on_window(void* ctx, const bdk_ui_request_info_t* info) {
    (void)ctx;
    snprintf(g_window, sizeof g_window, "%.*s", (int)info->payload_json.length,
             info->payload_json.data ? info->payload_json.data : "");
    const char* handle = strstr(g_window, "\"newContents\":\"");
    if (handle)
        g_handler_page = (bdk_contents_t)strtoull(handle + 15, NULL, 10);
    bdk_error_t err = 0;
    bdk_native_handle_t none;
    memset(&none, 0, sizeof none);
    CHECK(bdk_ui_complete(info->request, S("accept"), S(""), none, &err) == BDK_OK);
    return BDK_UI_HANDLED;
}

static int wait_for_title(bdk_contents_t c, const char* want) {
    char title[256] = "";
    for (int i = 0; i < 400; ++i) {
        bdk_contents_state_t st;
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        bdk_error_t err = 0;
        REQUIRE(bdk_contents_state(c, &st, &err) == BDK_OK);
        snprintf(title, sizeof title, "%.*s", (int)st.title.length,
                 st.title.data ? st.title.data : "");
        if (strcmp(title, want) == 0)
            return 1;
        t_pump();
        bdk_testing_run_for(0.05);
    }
    printf("    title is \"%s\", not \"%s\"\n", title, want);
    /* A rejected show() keeps its message: shown as the title, for the log. */
    if (bdk_testing_execute_script(c, S("if (window.why) document.title = window.why"), 0) ==
        BDK_OK) {
        t_pump();
        bdk_testing_run_for(0.2);
        bdk_contents_state_t st;
        memset(&st, 0, sizeof st);
        st.struct_size = sizeof st;
        bdk_error_t err = 0;
        if (bdk_contents_state(c, &st, &err) == BDK_OK)
            printf("    the page says: %.*s\n", (int)st.title.length,
                   st.title.data ? st.title.data : "");
    }
    return 0;
}

/* A runtime with `extra` in its configuration, a payments handler unless `handler` is 0, and the
   shop page, shown. */
static bdk_contents_t start(const char* extra, int handler) {
    g_calls = 0;
    g_payload[0] = '\0';
    g_complete = NULL;
    g_handler_page = 0;
    g_window[0] = '\0';
    t_config_extra = extra;
    t_start();
    t_config_extra = NULL;
    control("present.clear", "{}", NULL, 0);
    bdk_error_t err = 0;
    if (handler)
        REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_PAYMENTS, on_payment, NULL, NULL, &err) ==
                BDK_OK);
    REQUIRE(bdk_ui_set_handler(t_runtime, BDK_SURFACE_NEW_WINDOWS, on_window, NULL, NULL, &err) ==
            BDK_OK);
    t_freeze();
    REQUIRE(start_server());
    g_page = t_new_contents(t_new_profile("Shop"));
    bdk_harness_engine_show(g_page, 1);
    REQUIRE(bdk_contents_focus(g_page) == BDK_OK);
    t_pump();
    REQUIRE(bdk_contents_load(g_page, S(t_http_url("/shop.html")), &err) == BDK_OK);
    REQUIRE(wait_for_title(g_page, "shop"));
    return g_page;
}

static void finish(bdk_contents_t c) {
    control("present.clear", "{}", NULL, 0);
    bdk_harness_engine_show(c, 0);
    for (int i = 0; i < 10; ++i)
        t_pump();
    t_http_stop();
}

/* show() for a dollar of tea, with `options` (the PaymentOptions literal), with a gesture: the
   page's title becomes "paid <token>" or the error's name. */
static void ask_to_pay(bdk_contents_t c, const char* options) {
    char js[1024];
    snprintf(js, sizeof js,
             "new PaymentRequest([{supportedMethods: location.origin + '/pay'}],"
             " {total: {label: 'Total', amount: {currency: 'USD', value: '1.00'}},"
             "  displayItems: [{label: 'Tea', amount: {currency: 'USD', value: '1.00'}}]}, %s)"
             ".show().then(r => r.complete('success').then(() =>"
             " document.title = 'paid ' + r.details.token),"
             " e => { window.why = e.message; document.title = e.name; })",
             options);
    REQUIRE(bdk_testing_execute_script(c, S(js), 1) == BDK_OK);
    t_pump();
}

/* The handler's page opens as a new window of kind paymentHandler; the person presses its Pay. */
static void press_pay_in_the_handler(void) {
    for (int i = 0; i < 400 && !g_handler_page; ++i) {
        t_pump();
        bdk_testing_run_for(0.05);
    }
    REQUIRE(g_handler_page != 0);
    CHECK(strstr(g_window, "\"kind\":\"paymentHandler\"") != NULL);
    bdk_harness_engine_show(g_handler_page, 1);
    REQUIRE(wait_for_title(g_handler_page, "confirm"));
    REQUIRE(bdk_testing_click(g_handler_page, 40, 40, 0) == BDK_OK);
    t_pump();
}

/* ---- Scenarios ---- */

/* The request names who asks, the total, the items and the handler; a grant with the gesture
   pays through it, in the handler's own page. */
static void test_a_handler_pays_with_the_app(void) {
    bdk_contents_t c = start(NULL, 1);
    g_complete = "grant";
    ask_to_pay(c, "{}");
    press_pay_in_the_handler();
    CHECK(wait_for_title(c, "paid paid-ok"));
    CHECK(g_calls == 1);
    char origin[96];
    snprintf(origin, sizeof origin, "\"origin\":\"%s\"", t_http_origin());
    CHECK(strstr(g_payload, origin) != NULL);
    CHECK(strstr(g_payload, "\"kind\":\"paymentRequest\"") != NULL);
    CHECK(strstr(g_payload, "\"value\":\"1.00\"") != NULL);
    CHECK(strstr(g_payload, "\"label\":\"Tea\"") != NULL);
    CHECK(strstr(g_payload, "\"name\":\"Test Pay\"") != NULL);
    finish(c);
}

/* A deny cancels: show() rejects with AbortError. */
static void test_a_deny_cancels(void) {
    bdk_contents_t c = start(NULL, 1);
    g_complete = "deny";
    ask_to_pay(c, "{}");
    CHECK(wait_for_title(c, "AbortError"));
    CHECK(g_calls == 1);
    finish(c);
}

/* With no handler BDK's sheet asks; its "Pay with" button pays. */
static void test_the_default_sheet_pays(void) {
    bdk_contents_t c = start(NULL, 0);
    control("present.script", "{\"surface\":\"payments\",\"answer\":{\"answer\":\"app0\"}}", NULL,
            0);
    ask_to_pay(c, "{}");
    press_pay_in_the_handler();
    CHECK(wait_for_title(c, "paid paid-ok"));
    char out[2048];
    control("present.record", "{\"surface\":\"payments\"}", out, sizeof out);
    CHECK(strstr(out, "\"shown\":1") != NULL);
    finish(c);
}

/* A request for shipping or payer details is refused before anyone is asked: BDK has no address
   or contact picker. */
static void test_shipping_or_payer_details_are_refused(void) {
    bdk_contents_t c = start(NULL, 1);
    ask_to_pay(c, "{requestShipping: true}");
    CHECK(wait_for_title(c, "NotSupportedError"));
    bdk_error_t err = 0;
    REQUIRE(bdk_contents_load(c, S(t_http_url("/shop.html")), &err) == BDK_OK);
    CHECK(wait_for_title(c, "shop"));
    ask_to_pay(c, "{requestPayerEmail: true}");
    CHECK(wait_for_title(c, "NotSupportedError"));
    CHECK(g_calls == 0);
    finish(c);
}

#define DISABLED_LAUNCH "engine_launch_payments_disabled_pages_have_no_payment_request"
#define DISABLED_CONFIG "\"surfaces\":{\"payments\":\"disabled\"}"

static int g_disabled_prepared;

/* Called by the harness before its first start: Chromium reads whether pages have Payment
   Request as it starts, so this scenario runs alone (scripts/test-engine.sh). */
void bdk_engine_payments_launch_prepare(const char* name) {
    if (strcmp(name, DISABLED_LAUNCH) != 0)
        return;
    t_config_extra = DISABLED_CONFIG;
    g_disabled_prepared = 1;
}

/* payments = "disabled": pages have no PaymentRequest. */
static void test_disabled_pages_have_no_payment_request(void) {
    if (!g_disabled_prepared) {
        printf("    nothing prepared: run alone (scripts/test-engine.sh does)\n");
        return;
    }
    bdk_contents_t c = start(DISABLED_CONFIG, 1);
    REQUIRE(bdk_testing_execute_script(c, S("document.title = typeof PaymentRequest"), 1) ==
            BDK_OK);
    CHECK(wait_for_title(c, "undefined"));
    finish(c);
}

const t_test_t t_engine_payment_tests[] = {
    {"engine_payments_a_handler_pays_with_the_app", test_a_handler_pays_with_the_app},
    {"engine_payments_a_deny_cancels", test_a_deny_cancels},
    {"engine_payments_the_default_sheet_pays", test_the_default_sheet_pays},
    {"engine_payments_shipping_or_payer_details_are_refused",
     test_shipping_or_payer_details_are_refused},
    {DISABLED_LAUNCH, test_disabled_pages_have_no_payment_request},
    {NULL, NULL},
};
