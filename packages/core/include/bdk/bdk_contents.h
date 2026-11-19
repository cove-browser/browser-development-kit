/* BDK C ABI: WebContents lifecycle, navigation, state and events. See c-abi.md sections 5.2
   and 5.3. Signatures are PROVISIONAL where c-abi.md only names a function (it gives the event
   struct and the subscribe functions in full). Named in c-abi.md but not declared, because nothing
   states their shape: bdk_contents_prune, _perform_command, _can_perform_command, _find_start,
   _find_next, _find_stop, _set_host_context (conditional on decision D1). */
#ifndef BDK_CONTENTS_H_
#define BDK_CONTENTS_H_

#include "bdk_base.h"
#include "bdk_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bdk_contents_options {
    size_t struct_size;
    bdk_profile_t profile;    /* required */
    bdk_string_t initial_url; /* empty: about:blank */
} bdk_contents_options_t;

/* Created in the given profile. Released by bdk_contents_destroy (or bdk_release). Requires the
   registrations to be frozen (BDK_ERR_NOT_STARTED before). */
BDK_EXPORT bdk_status_t bdk_contents_create(bdk_runtime_t, const bdk_contents_options_t*,
                                            bdk_contents_t* out, bdk_error_t*);
/* Asks the page to close. Emits BDK_CONTENTS_EVENT_CLOSED. The handle stays valid until destroyed.
 */
BDK_EXPORT bdk_status_t bdk_contents_close(bdk_contents_t, bdk_error_t*);
/* Releases the handle and destroys the page. */
BDK_EXPORT bdk_status_t bdk_contents_destroy(bdk_contents_t);

/* Navigation. Each starts a navigation and returns; progress arrives as events. */
BDK_EXPORT bdk_status_t bdk_contents_load(bdk_contents_t, bdk_string_t url, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_contents_reload(bdk_contents_t, uint8_t bypass_cache, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_contents_stop(bdk_contents_t, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_contents_go_to_offset(bdk_contents_t, int32_t offset, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_contents_go_to_index(bdk_contents_t, int32_t index, bdk_error_t*);
/* JSON {"current":int,"entries":[{"url","title"}]} in a buffer the caller releases. */
BDK_EXPORT bdk_status_t bdk_contents_entries(bdk_contents_t, bdk_buffer_t* out_json, bdk_error_t*);

/* Filled by bdk_contents_state. The caller sets struct_size to the size it was built with; the
   callee writes only those bytes. url and title are borrowed: valid until the next
   bdk_contents_state call on the same contents or until it is destroyed. */
typedef struct bdk_contents_state {
    size_t struct_size;
    bdk_string_t url;
    bdk_string_t title;
    uint8_t loading;
    double progress;     /* 0.0 to 1.0 */
    uint32_t security;   /* BDK_SECURITY_* */
    uint32_t audio;      /* BDK_AUDIO_* flags */
    uint32_t capture;    /* capture flags; none are defined yet, always 0 */
    uint32_t visibility; /* BDK_VISIBILITY_* */
    uint8_t focused;
    double zoom; /* 1.0 is 100 percent */
} bdk_contents_state_t;

BDK_EXPORT bdk_status_t bdk_contents_state(bdk_contents_t, bdk_contents_state_t* inout,
                                           bdk_error_t*);

/* Serialised navigation state, as a buffer the caller releases. Restoring it replaces the entries.
   A buffer from a newer format fails with contents.stateTooNew. */
BDK_EXPORT bdk_status_t bdk_contents_session_state(bdk_contents_t, bdk_buffer_t* out, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_contents_restore(bdk_contents_t, bdk_bytes_t state, bdk_error_t*);

BDK_EXPORT bdk_status_t bdk_contents_set_muted(bdk_contents_t, uint8_t muted, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_contents_set_zoom(bdk_contents_t, double zoom, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_contents_set_user_agent(bdk_contents_t, bdk_string_t user_agent,
                                                    bdk_error_t*);

/* Added (S15). The handle stays valid across a discard. */
BDK_EXPORT bdk_status_t bdk_contents_discard(bdk_contents_t, bdk_error_t*);
BDK_EXPORT bdk_status_t bdk_contents_set_auto_discardable(bdk_contents_t, uint8_t discardable,
                                                          bdk_error_t*);

/* Events (section 5.3). The struct and every string in it are valid until the callback returns. */
typedef struct bdk_contents_event {
    size_t struct_size;
    uint32_t kind; /* BDK_CONTENTS_EVENT_* from bdk_ids.h */
    bdk_contents_t contents;
    bdk_string_t url;
    bdk_string_t detail_json; /* fields specific to the kind */
    int32_t net_error;
    uint32_t flags; /* BDK_EVENT_FLAG_*: main frame, same document, user initiated, error page */
} bdk_contents_event_t;

typedef void (*bdk_contents_event_cb)(void* context, const bdk_contents_event_t*);

/* Events are delivered in order, on the UI thread, never synchronously from a call that causes
   them. Released by bdk_unsubscribe or bdk_release; the release callback follows the last event. */
BDK_EXPORT bdk_subscription_t bdk_contents_subscribe(bdk_contents_t, bdk_contents_event_cb,
                                                     void* ctx, bdk_release_cb);

#ifdef __cplusplus
}
#endif

#endif /* BDK_CONTENTS_H_ */
