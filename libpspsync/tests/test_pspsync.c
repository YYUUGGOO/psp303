#include "pspsync.h"

#include <assert.h>
#include <stdio.h>

static void test_profile(PSPSyncProfile profile, int expected_pulse_samples)
{
    PSPSyncClock clock;
    PSPSyncConfig config = {44100, 128000, 4, 29490};
    int ticks = 0;
    int pulse_samples = 0;
    int i;

    pspSyncInit(&clock, &config);
    pspSyncSetProfile(&clock, profile);
    pspSyncStart(&clock, 1);

    for (i = 0; i < 44100; ++i) {
        PSPSyncFrame frame = pspSyncNext(&clock);
        ticks += frame.tick;
        if (frame.pulse != 0) ++pulse_samples;
    }

    assert(ticks == 9);
    assert(pulse_samples == expected_pulse_samples);
    pspSyncStop(&clock);
    assert(pspSyncNext(&clock).tick == 0);
}

int main(void)
{
    test_profile(PSPSYNC_PROFILE_OFF, 0);
    test_profile(PSPSYNC_PROFILE_POCKET_OPERATOR, 5 * 661);
    test_profile(PSPSYNC_PROFILE_VOLCA, 9 * 661);
    puts("libpspsync tests passed");
    return 0;
}
