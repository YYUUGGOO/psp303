#ifndef PSPSYNC_H
#define PSPSYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PSPSYNC_PROFILE_OFF = 0,
    PSPSYNC_PROFILE_POCKET_OPERATOR,
    PSPSYNC_PROFILE_VOLCA,
    PSPSYNC_PROFILE_COUNT
} PSPSyncProfile;

typedef struct {
    uint32_t sample_rate;
    uint32_t bpm_milli;
    uint16_t ticks_per_quarter;
    int16_t pulse_level;
} PSPSyncConfig;

typedef struct {
    int16_t pulse;
    uint8_t tick;
} PSPSyncFrame;

typedef struct {
    PSPSyncConfig config;
    PSPSyncProfile profile;
    uint64_t phase_q32;
    uint64_t increment_q32;
    uint32_t pulse_samples;
    uint32_t pulse_remaining;
    uint32_t tick_count;
    uint16_t pulse_divider;
    uint8_t running;
    uint8_t pending_tick;
} PSPSyncClock;

void pspSyncInit(PSPSyncClock *clock, const PSPSyncConfig *config);
void pspSyncSetTempo(PSPSyncClock *clock, uint32_t bpm_milli);
void pspSyncSetProfile(PSPSyncClock *clock, PSPSyncProfile profile);
void pspSyncStart(PSPSyncClock *clock, int reset_phase);
void pspSyncStop(PSPSyncClock *clock);
PSPSyncFrame pspSyncNext(PSPSyncClock *clock);
const char *pspSyncProfileName(PSPSyncProfile profile);

#ifdef __cplusplus
}
#endif

#endif
