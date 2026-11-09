#include "base_boot.h"

uint32_t base_boot_claim(const volatile uint32_t *journal, size_t words,
                        int (*program)(size_t index))
{
    size_t slot = 0;
    /* Any partially programmed word consumes a slot, including gaps before it. */
    for (size_t i = 0; i < words; ++i) {
        if (journal[i] != UINT32_MAX) slot = i + 1;
    }
    if (slot >= words || !program(slot) || journal[slot] != 0U) return 0;
    return (uint32_t)(slot + 1);
}
