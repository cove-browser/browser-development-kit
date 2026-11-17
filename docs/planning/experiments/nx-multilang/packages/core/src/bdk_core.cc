#include "bdk_core.h"
extern "C" uint32_t bdk_abi_version(void) { return BDK_ABI_VERSION; }
extern "C" int32_t bdk_add(int32_t a, int32_t b) { return a + b; }
