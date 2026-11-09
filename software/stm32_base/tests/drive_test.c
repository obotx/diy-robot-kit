#include "base_drive.h"
#include "base_drive_config.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void)
{
    BaseDrive d;
    BaseDriveConfig bench = BASE_DRIVE_CONFIG;
    bench.commissioned = false;
    base_drive_init(&d, &bench);
    assert(!base_drive_ready(&d));
    assert(base_drive_service(&d, 0, .1f, 0));
    assert(d.pwm[0] == .1f && d.pwm[1] == 0 && d.pwm[2] == 0 && d.pwm[3] == 0);
    base_drive_stop(&d);
    assert(!base_drive_service(&d, 1, .16f, 0));
    assert(base_drive_service(&d, 1, .1f, 0));
    int32_t bench_counts[4] = {0, 7, 0, 0};
    base_drive_tick(&d, bench_counts, 10);
    assert(d.enabled && d.service && !base_drive_ready(&d));
    assert(d.encoder_counts[1] == 7 && d.pwm[1] == .1f);
    assert(d.pwm[0] == 0 && d.pwm[2] == 0 && d.pwm[3] == 0);
    assert(!base_drive_service(&d, 2, .1f, 10));
    base_drive_stop(&d);
    assert(!d.service && !d.enabled && d.pwm[1] == 0);
    assert(base_drive_service(&d, 3, -.1f, 20));
    assert(!base_drive_service(&d, 3, -.1f, 3020));
    assert(d.fault && !d.enabled);
    BaseDriveConfig c = {true, .152f, .4f, .4f, 2244, 1, .25f, .2f, .5f,
        {1,1,1,1}, {1,1,1,1}};
    base_drive_init(&d, &c);
    double v[3] = {0,.2,0}, a[2] = {.3,.6};
    int32_t counts[4] = {0};
    base_drive_command(&d, true, v, a);
    base_drive_tick(&d, counts, 10);
    assert(fabsf(d.velocity[1] - .003f) < .00001f);
    assert(d.pwm[0] < 0 && d.pwm[1] > 0 && d.pwm[2] > 0 && d.pwm[3] < 0);
    for (int n=0; n<250 && !d.fault; ++n) base_drive_tick(&d, counts, 10);
    assert(d.fault && !d.enabled);
    for (int i=0; i<4; ++i) assert(d.pwm[i] == 0);
    base_drive_init(&d, &c);
    base_drive_command(&d, true, v, a);
    base_drive_fault(&d, "test_fault"); base_drive_tick(&d, counts, 10);
    assert(!d.enabled && d.fault);
    base_drive_init(&d, &c);
    base_drive_command(&d, true, v, a); base_drive_tick(&d, counts, 31);
    assert(!d.enabled && d.fault);
    base_drive_init(&d, &BASE_DRIVE_CONFIG);
    assert(base_drive_ready(&d));
    double cruise[3] = {.05, 0, 0};
    int32_t feedback[4] = {4, -4, 4, -4};
    for (int n = 0; n < 600; ++n) {
        base_drive_command(&d, true, cruise, a);
        base_drive_tick(&d, feedback, 10);
        assert(d.enabled && !d.fault);
    }
    assert(fabsf(d.velocity[0] - .05f) < .00001f);
    double zero[3] = {0};
    base_drive_command(&d, true, zero, a);
    base_drive_tick(&d, feedback, 10);
    assert(d.velocity[0] > 0 && d.velocity[0] < .05f);
    for (int n = 0; n < 100; ++n) base_drive_tick(&d, feedback, 10);
    for (int i = 0; i < 4; ++i) assert(d.pwm[i] == 0 && d.measured[i] > 0);
    c.kp = 1; c.ki = 1; c.pwm_limit = .85f;
    double start[3] = {.2, 0, 0}, fast[2] = {10, 2};
    base_drive_init(&d, &c);
    for (int n = 0; n < 70; ++n) {
        base_drive_command(&d, true, start, fast);
        base_drive_tick(&d, counts, 10);
        assert(!d.fault); /* Repeated commands neither fault early nor renew grace. */
    }
    for (int n = 0; n < 50 && !d.fault; ++n) base_drive_tick(&d, counts, 10);
    assert(d.fault && !d.enabled);
    base_drive_init(&d, &c);
    base_drive_command(&d, true, start, fast);
    int32_t moving[4] = {1, 1, 1, 1};
    base_drive_tick(&d, moving, 10);
    for (int n = 0; n < 49; ++n) base_drive_tick(&d, counts, 10);
    assert(!d.fault);
    base_drive_tick(&d, counts, 10);
    assert(d.fault && !d.enabled);
    puts("drive: calibration, continuous control, ramps, encoder signs, faults OK");
}
