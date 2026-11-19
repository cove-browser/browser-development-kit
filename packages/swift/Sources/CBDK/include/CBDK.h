/* The C module Swift imports: the public ABI headers and the generated ABI hash. Both are symlinks
   to packages/core, so the Swift layer reads BDK_ABI_VERSION and BDK_ABI_HASH from the headers and
   never from a copy. */
#ifndef CBDK_H_
#define CBDK_H_

#include "bdk/bdk.h"
#include "bdk_abi_hash.h"

#endif /* CBDK_H_ */
