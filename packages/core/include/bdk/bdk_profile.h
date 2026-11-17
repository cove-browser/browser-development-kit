/* BDK C ABI: profile lifecycle. See c-abi.md section 5.1.
   c-abi.md gives a family table without signatures. The signatures below are PROVISIONAL: they
   follow the conventions of section 2 and are what the stub implements. Not yet declared because
   the document does not say enough: none. */
#ifndef BDK_PROFILE_H_
#define BDK_PROFILE_H_

#include "bdk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Asynchronous. The result JSON is an array of profile info objects (see bdk_profile_info). */
BDK_EXPORT bdk_call_t bdk_profiles_list(bdk_runtime_t, bdk_result_cb, void* ctx, bdk_release_cb);

/* Asynchronous. Takes a display name. The result JSON is {"id":"<identifier>"}. Fails with
 * profile.nameTaken. */
BDK_EXPORT bdk_call_t bdk_profile_create(bdk_runtime_t, bdk_string_t display_name, bdk_result_cb,
                                         void* ctx, bdk_release_cb);

/* Asynchronous. Takes an identifier and never creates (profile.notFound). A profile has at most one
   open handle; opening it again fails with profile.inUse. *out_profile is written before this
   returns so the binding can hold the handle; it is usable once the result callback has delivered
   success, and is released by bdk_profile_close. If the call fails the handle is released before
   the callback runs and *out_profile reads 0. A call that returns 0 was rejected synchronously: no
   callback follows. */
BDK_EXPORT bdk_call_t bdk_profile_open(bdk_runtime_t, bdk_string_t profile_id,
                                       bdk_profile_t* out_profile, bdk_result_cb, void* ctx,
                                       bdk_release_cb);

/* Opens an ephemeral profile: no disk state, identifier prefixed "ephemeral:", discarded on close.
   Released by bdk_profile_close. */
BDK_EXPORT bdk_status_t bdk_profile_open_private(bdk_runtime_t, bdk_profile_t* out_profile,
                                                 bdk_error_t*);

/* Closes the profile and its contents, and releases the handle. */
BDK_EXPORT bdk_status_t bdk_profile_close(bdk_profile_t, bdk_error_t*);

BDK_EXPORT bdk_status_t bdk_profile_rename(bdk_profile_t, bdk_string_t display_name, bdk_error_t*);

/* Asynchronous. Takes an identifier. Fails with profile.inUse while the profile is open. */
BDK_EXPORT bdk_call_t bdk_profile_delete(bdk_runtime_t, bdk_string_t profile_id, bdk_result_cb,
                                         void* ctx, bdk_release_cb);

/* Writes a JSON object {"id","name","kind":"persistent"|"ephemeral","parent":string|null} into a
   buffer the caller releases with bdk_release. */
BDK_EXPORT bdk_status_t bdk_profile_info(bdk_profile_t, bdk_buffer_t* out_json, bdk_error_t*);

#ifdef __cplusplus
}
#endif

#endif /* BDK_PROFILE_H_ */
