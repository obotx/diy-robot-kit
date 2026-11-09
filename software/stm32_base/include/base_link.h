#ifndef BASE_LINK_H
#define BASE_LINK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "base_drive.h"

#define BASE_LINK_MAX_FRAME 2048U
#define BASE_LINK_TX_SIZE 768U
#define BASE_LINK_WATCHDOG_MS 250U

typedef struct {
    BaseDrive *drive; /* Optional: NULL retains the communication-only host fixture. */
    char device_id[81], boot_id[81], session[33];
    uint64_t seq;
    volatile bool session_valid, watchdog_armed, watchdog_expired;
    volatile uint32_t last_setpoint_ms;
    uint32_t frame_start_ms;
    size_t rx_len;
    bool dropping;
    const char *error;
    char rx[BASE_LINK_MAX_FRAME];
} BaseLink;

/* Locally generated, JSON-safe IDs only (not peer-supplied strings). */
void base_link_init(BaseLink *link, const char *device_id, const char *boot_id);
void base_link_abort(BaseLink *link, const char *reason);
/* ISR invalidates the session independently of the parser. Expiration is sticky
 * until HELLO. The board calls this every millisecond, including during TX. */
bool base_link_watchdog_isr(BaseLink *link, uint32_t now_ms);
void base_link_tick(BaseLink *link, uint32_t now_ms);
size_t base_link_feed(BaseLink *link, uint32_t now_ms, uint8_t byte,
                      char *reply, size_t capacity);
#endif
