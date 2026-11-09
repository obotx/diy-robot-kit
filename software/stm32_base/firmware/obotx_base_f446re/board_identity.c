#include "stm32f4xx_hal.h"
#include "base_boot.h"
#include <stdio.h>

extern const uint32_t _boot_journal_start[], _boot_journal_end[];

static int program_slot(size_t index)
{
    if (HAL_FLASH_Unlock() != HAL_OK) return 0;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                          FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    HAL_StatusTypeDef status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
        (uint32_t)&_boot_journal_start[index], 0U);
    HAL_FLASH_Lock();
    if (FLASH->ACR & FLASH_ACR_DCEN) {
        __HAL_FLASH_DATA_CACHE_DISABLE();
        __HAL_FLASH_DATA_CACHE_RESET();
        __HAL_FLASH_DATA_CACHE_ENABLE();
    }
    return status == HAL_OK;
}

int base_board_identity(char device_id[81], char boot_id[81])
{
    size_t words = ((uintptr_t)_boot_journal_end - (uintptr_t)_boot_journal_start) / 4U;
    uint32_t boot = base_boot_claim(_boot_journal_start, words, program_slot);
    if (!boot) return 0;
    snprintf(device_id, 81, "BASE-%08lx%08lx%08lx", (unsigned long)HAL_GetUIDw0(),
             (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw2());
    snprintf(boot_id, 81, "%s-%08lx", device_id, (unsigned long)boot);
    return 1;
}
