#ifndef BASE_BOOT_H
#define BASE_BOOT_H
#include <stddef.h>
#include <stdint.h>
/* Append-only journal. Callback must program zero and invalidate flash cache.
 * Zero means failure: firmware must not open the serial endpoint. */
uint32_t base_boot_claim(const volatile uint32_t *journal, size_t words,
                        int (*program)(size_t index));
#endif
