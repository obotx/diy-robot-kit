#include "main.h"
#include "arm_board_link.h"
#include <stdio.h>

extern const unsigned char _boot_journal_start;
extern const unsigned char _boot_journal_end;

int arm_board_identity(char device_id[81], char boot_id[81])
{
    /* Append-only boot journal in reserved sector 7. Never erase automatically.
     * Interrupted writes consume a slot; no reply is sent before read-back.
     */
    const volatile uint32_t *begin = (const volatile uint32_t *)&_boot_journal_start;
    const volatile uint32_t *end = (const volatile uint32_t *)&_boot_journal_end;
    const volatile uint32_t *slot = begin;
    for (const volatile uint32_t *p = begin; p < end; ++p) {
        if (*p != UINT32_MAX) slot = p + 1;
    }
    if (slot >= end || HAL_FLASH_Unlock() != HAL_OK) return 0;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                          FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    HAL_StatusTypeDef status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, (uint32_t)slot, 0U);
    HAL_FLASH_Lock();
    if (FLASH->ACR & FLASH_ACR_DCEN) {
        __HAL_FLASH_DATA_CACHE_DISABLE();
        __HAL_FLASH_DATA_CACHE_RESET();
        __HAL_FLASH_DATA_CACHE_ENABLE();
    }
    if (status != HAL_OK || *slot != 0U) return 0;
    snprintf(device_id, 81, "ARM-%08lx%08lx%08lx", (unsigned long)HAL_GetUIDw0(),
             (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw2());
    snprintf(boot_id, 81, "%s-%08lx", device_id, (unsigned long)(slot - begin + 1));
    return 1;
}
