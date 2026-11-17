/* BDK C ABI: providers. See c-abi.md section 9. c-abi.md lists the callback names of each service's
   vtable but not their signatures, so the vtables are declared as a size-prefixed struct with no
   callbacks yet; each service's callbacks are appended when its contract task lands (T-PRV-01).
   PROVISIONAL: the runtime parameter is added to match every other registration call (c-abi.md
   writes bdk_provider_register(service, vtable, ctx)). */
#ifndef BDK_PROVIDER_H_
#define BDK_PROVIDER_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bdk_provider_vtable {
    size_t struct_size;
    /* Translation: supported_pairs, prepare, translate, detect. Reputation: check_url,
       check_download, update. Geolocation: start, stop. Credentials (Added, S14): list, get_secret,
       save, remove. Leak check (Added, S16): check_credential. Signatures not yet specified. */
} bdk_provider_vtable_t;

/* service is the service name, such as "translation". Registration only before the freeze. */
BDK_EXPORT bdk_status_t bdk_provider_register(bdk_runtime_t, bdk_string_t service,
                                              const bdk_provider_vtable_t*, void* ctx,
                                              bdk_release_cb, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_provider_set_key(bdk_runtime_t, bdk_string_t service, bdk_string_t key,
                                             bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_PROVIDER_H_ */
