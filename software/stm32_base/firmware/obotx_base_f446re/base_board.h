#ifndef BASE_BOARD_H
#define BASE_BOARD_H
#include <stdbool.h>
#include <stdint.h>
void base_board_init(void);
void base_board_off(void);
void base_board_counts(int32_t delta[4]);
/* Caller holds interrupt mask and checks watchdog/faults before applying. */
void base_board_apply(const float pwm[4]);
#endif
