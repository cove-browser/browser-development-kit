#ifndef BDK_CORE_H_
#define BDK_CORE_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BDK_ABI_VERSION 1
uint32_t bdk_abi_version(void);
/* Returns a + b; stands in for a real service call across the C ABI. */
int32_t bdk_add(int32_t a, int32_t b);
#ifdef __cplusplus
}
#endif
#endif
