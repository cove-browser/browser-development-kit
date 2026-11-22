/* BDK C ABI: providers. See c-abi.md section 9 and packages/core/docs/conventions.md ("Providers").
   An application provider (`provider = "app"` in bdk.toml) is a vtable of operations. BDK calls an
   operation with a call context and a JSON input; the application answers later, once, with
   bdk_provider_resolve or bdk_provider_reject, as an application command does. The input and output
   of each operation are the contract (T-PRV-01), checked by the core whichever runtime runs it.
   PROVISIONAL: the runtime parameter of bdk_provider_register is added to match every other
   registration call, and c-abi.md's geolocation `stop` is `cancel`, which every operation shares.
 */
#ifndef BDK_PROVIDER_H_
#define BDK_PROVIDER_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One operation. input_json is borrowed until the callback returns; call stays valid until the
   provider answers it. Called on the UI thread, never synchronously from the call that caused it.
 */
typedef void (*bdk_provider_op_cb)(void* context, bdk_context_t call, bdk_string_t input_json);

/* Operations, by service. A required operation must be set; an optional one may be null.
   Translation:
     supported_pairs (required)  {}                                   ->
   {"pairs":[{"from","to","installed"}]} prepare                      {"from","to"} -> null; reports
   progress translate (required)         {"from","to","texts":[...],"url"}    -> {"texts":[...]},
   same count detect                       {"sample"}                            ->
   {"language","reliable"} or null remove_model                 {"from","to"} -> null Reputation:
     check_url (required)         {"url","kind":"navigation"|"subresource"} -> {"verdict"}
     check_download               {"urls":[...],"fileName"}             -> {"verdict"}
     update                       {}                                    -> null
   Geolocation:
     start (required)             {"highAccuracy":bool}  reports positions with bdk_provider_report
                                  until cancelled; resolving ends the watch
   Verdicts: "safe", "unknown", "malware", "phishing", "unwantedSoftware", "dangerousDownload". A
   position: {"latitude","longitude","accuracy", optional "altitude","altitudeAccuracy","heading",
   "speed", "timestamp" in milliseconds}, or {"error":"permissionDenied"|"positionUnavailable",
   "message"}. */
typedef struct bdk_provider_vtable {
    size_t struct_size;
    /* {"id","name","privacy":"onDevice"|"remote","host","policyUrl","protection"}; a remote
       provider names its host and an https policy URL. Borrowed for the register call. */
    bdk_string_t descriptor_json;
    /* Called when BDK no longer wants the answer of a call (the page navigated, the translation
       was reverted, the watch stopped, the runtime is shutting down). The provider still answers
       the call, and the answer is ignored. May be null. */
    bdk_provider_op_cb cancel;
    bdk_provider_op_cb supported_pairs, prepare, translate, detect, remove_model;
    bdk_provider_op_cb check_url, check_download, update;
    bdk_provider_op_cb start;
} bdk_provider_vtable_t;

/* Answers a call. The first answer wins; later ones, and answers after a cancel, are ignored. The
   context is stale afterwards. UI thread. */
BDK_EXPORT void bdk_provider_resolve(bdk_context_t call, bdk_string_t output_json);
BDK_EXPORT void bdk_provider_reject(bdk_context_t call, bdk_string_t domain, bdk_string_t code,
                                    bdk_string_t message);
/* prepare: how far a download has got, 0 to 1. Ignored for other operations. */
BDK_EXPORT void bdk_provider_progress(bdk_context_t call, double fraction);
/* start: one position or position error, as JSON. Ignored for other operations. */
BDK_EXPORT void bdk_provider_report(bdk_context_t call, bdk_string_t json);
BDK_EXPORT uint8_t bdk_provider_is_cancelled(bdk_context_t call);

/* service is "translation", "reputation" or "geolocation", configured with provider = "app" in the
   resolved configuration (otherwise config.providerNotApp). Registration only before the freeze;
   a service configured "app" with no provider fails the freeze with config.providerMissing. An
   invalid descriptor is config.descriptorInvalid; a missing required operation, an unknown service
   or a null vtable is BDK_ERR_INVALID_ARGUMENT. */
BDK_EXPORT bdk_status_t bdk_provider_register(bdk_runtime_t, bdk_string_t service,
                                              const bdk_provider_vtable_t*, void* ctx,
                                              bdk_release_cb, bdk_error_t*);
/* The key of a named provider BDK ships that needs one. No provider in this runtime takes a key
   this way: the Google key is google_api_key in bdk_start_config_t, because Chromium reads it
   before it starts. Fails with config.keyUnused. */
BDK_EXPORT bdk_status_t bdk_provider_set_key(bdk_runtime_t, bdk_string_t service, bdk_string_t key,
                                             bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_PROVIDER_H_ */
