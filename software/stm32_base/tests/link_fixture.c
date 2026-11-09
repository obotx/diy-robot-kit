/* Host-only harness; @ commands never exist in the firmware UART endpoint. */
#include "base_link.h"
#include "base_boot.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static uint32_t journal[16];
static int write_mode;
static int program(size_t i)
{
    if (write_mode == 1) return 0;
    journal[i] = write_mode == 2 ? 0xffffff00U : 0U;
    return 1;
}
static void test_boot_journal(void)
{
    memset(journal, 0xff, sizeof(journal));
    assert(base_boot_claim(journal, 16, program) == 1);
    assert(base_boot_claim(journal, 16, program) == 2);
    write_mode = 1;
    assert(base_boot_claim(journal, 16, program) == 0);
    write_mode = 2;
    assert(base_boot_claim(journal, 16, program) == 0);
    write_mode = 0;
    assert(base_boot_claim(journal, 16, program) == 4);
    journal[8] = 0xffff0000U; /* Torn write after a gap is consumed too. */
    assert(base_boot_claim(journal, 16, program) == 10);
    for (unsigned n = 11; n <= 16; ++n) assert(base_boot_claim(journal, 16, program) == n);
    assert(base_boot_claim(journal, 16, program) == 0);
    memset(journal, 0xff, sizeof(journal));
}
static void reset(BaseLink *link)
{
    char boot[81];
    uint32_t n = base_boot_claim(journal, 16, program);
    assert(n);
    snprintf(boot, sizeof(boot), "BASE-TEST-%08lx", (unsigned long)n);
    base_link_init(link, "BASE-TEST", boot);
}
int main(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    test_boot_journal();
    BaseDrive drive;
    BaseLink link;
    reset(&link);
    uint32_t now = 0;
    char reply[BASE_LINK_TX_SIZE], control[120];
    bool at_start = true, responded = false;
    size_t capacity = sizeof(reply);
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int c; (c = getchar()) != EOF;) {
        if (at_start && c == '@') {
            if (!fgets(control, sizeof(control), stdin)) break;
            if (strncmp(control, "bench", 5) == 0) {
                BaseDriveConfig config = {0};
                base_drive_init(&drive, &config);
                link.drive = &drive;
            } else if (strncmp(control, "drive", 5) == 0) {
                BaseDriveConfig config = {true, .152f, .4f, .4f, 2244, 1, .25f, .2f, .5f,
                    {1,1,1,1}, {1,1,1,1}};
                base_drive_init(&drive, &config);
                link.drive = &drive;
            } else if (strncmp(control, "time ", 5) == 0) {
                now = (uint32_t)strtoull(control + 5, NULL, 10);
                base_link_tick(&link, now);
            } else if (strncmp(control, "isr ", 4) == 0) {
                now = (uint32_t)strtoull(control + 4, NULL, 10);
                base_link_watchdog_isr(&link, now);
            } else if (strncmp(control, "reset", 5) == 0) {
                reset(&link);
                now = 0;
            } else if (strncmp(control, "partial", 7) == 0) {
                base_link_feed(&link, now, '{', reply, capacity);
            } else if (strncmp(control, "tiny", 4) == 0) capacity = 1;
            printf("{\"session_valid\":%s,\"expired\":%s,\"armed\":%s,"
                   "\"last_ms\":%lu,\"dropping\":%s,\"rx_len\":%lu}\n",
                link.session_valid ? "true" : "false", link.watchdog_expired ? "true" : "false",
                link.watchdog_armed ? "true" : "false", (unsigned long)link.last_setpoint_ms,
                link.dropping ? "true" : "false", (unsigned long)link.rx_len);
            continue;
        }
        size_t n = base_link_feed(&link, now, (uint8_t)c, reply, capacity);
        if (n) { fwrite(reply, 1, n, stdout); responded = true; }
        at_start = c == '\n';
        if (at_start) {
            if (!responded) puts("NONE");
            responded = false;
        }
    }
    return 0;
}
