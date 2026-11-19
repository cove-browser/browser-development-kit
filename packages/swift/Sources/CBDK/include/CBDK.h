/* The C module Swift imports: the public ABI headers, the generated ABI hash and the macOS
   application base class. All are symlinks to packages/core, so the Swift layer reads
   BDK_ABI_VERSION and BDK_ABI_HASH from the headers and never from a copy. */
#ifndef CBDK_H_
#define CBDK_H_

#include "bdk/bdk.h"
#include "bdk_abi_hash.h"
/* macOS: the application base class the runtime exports. Objective-C only, so it is invisible to C.
 */
#include "bdk_mac/bdk_application.h"

#endif /* CBDK_H_ */
