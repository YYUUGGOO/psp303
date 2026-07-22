#include "pspsync.h"

#include <stddef.h>
#include <string.h>

#define PHASE_ONE (1ULL << 32)

static void update_increment(PSPSyncClock *clock)
{
    uint64_t numerator;
    uint64_t denominator;

    if (clock->config.sample_rate == 0 || clock->config.ticks_per_quarter == 0) {
        clock->increment_q32 = 0;
        return;
    }

    numerator = (uint64_t)clock->config.bpm_milli
              * (uint64_t)clock->config.ticks_per_quarter
              * PHASE_ONE;
    denominator = 60000ULL * (uint64_t)clock->config.sample_rate;
    clock->increment_q32 = numerator / denominator;
}

void pspSyncInit(PSPSyncClock *clock, const PSPSyncConfig *config)
{
    if (clock == NULL || config == NULL) return;
    memset(clock, 0, sizeof(*clock));
    clock->config = *config;
    if (clock->config.sample_rate == 0) clock->config.sample_rate = 44100;
    if (clock->config.bpm_milli == 0) clock->config.bpm_milli = 120000;
    if (clock->config.ticks_per_quarter == 0) clock->config.ticks_per_quarter = 4;
    if (clock->config.pulse_level == 0) clock->config.pulse_level = 29490;
    update_increment(clock);
    pspSyncSetProfile(clock, PSPSYNC_PROFILE_OFF);
}

void pspSyncSetTempo(PSPSyncClock *clock, uint32_t bpm_milli)
{
    if (clock == NULL || bpm_milli == 0 || clock->config.bpm_milli == bpm_milli) return;
    clock->config.bpm_milli = bpm_milli;
    update_increment(clock);
}

void pspSyncSetProfile(PSPSyncClock *clock, PSPSyncProfile profile)
{
    if (clock == NULL) return;
    if (profile < PSPSYNC_PROFILE_OFF || profile >= PSPSYNC_PROFILE_COUNT) {
        profile = PSPSYNC_PROFILE_OFF;
    }

    clock->profile = profile;
    clock->pulse_remaining = 0;
    switch (profile) {
        case PSPSYNC_PROFILE_POCKET_OPERATOR:
            /* PO sync is an eighth-note click: every second 16th tick. */
            clock->pulse_divider = 2;
            clock->pulse_samples = (clock->config.sample_rate * 15U) / 1000U;
            break;
        case PSPSYNC_PROFILE_VOLCA:
            /* Korg specifies a 15 ms pulse at the start of every step. */
            clock->pulse_divider = 1;
            clock->pulse_samples = (clock->config.sample_rate * 15U) / 1000U;
            break;
        case PSPSYNC_PROFILE_OFF:
        default:
            clock->pulse_divider = 0;
            clock->pulse_samples = 0;
            break;
    }
}

void pspSyncStart(PSPSyncClock *clock, int reset_phase)
{
    if (clock == NULL) return;
    clock->running = 1;
    if (reset_phase) {
        clock->phase_q32 = 0;
        clock->tick_count = 0;
        clock->pulse_remaining = 0;
        clock->pending_tick = 1;
    }
}

void pspSyncStop(PSPSyncClock *clock)
{
    if (clock == NULL) return;
    clock->running = 0;
    clock->pending_tick = 0;
    clock->pulse_remaining = 0;
}

PSPSyncFrame pspSyncNext(PSPSyncClock *clock)
{
    PSPSyncFrame frame = {0, 0};
    int emit_tick = 0;

    if (clock == NULL || !clock->running) return frame;

    if (clock->pending_tick) {
        clock->pending_tick = 0;
        emit_tick = 1;
    } else {
        clock->phase_q32 += clock->increment_q32;
        if (clock->phase_q32 >= PHASE_ONE) {
            clock->phase_q32 -= PHASE_ONE;
            emit_tick = 1;
        }
    }

    if (emit_tick) {
        frame.tick = 1;
        if (clock->pulse_divider != 0 && (clock->tick_count % clock->pulse_divider) == 0) {
            clock->pulse_remaining = clock->pulse_samples;
        }
        ++clock->tick_count;
    }

    if (clock->pulse_remaining > 0) {
        frame.pulse = clock->config.pulse_level;
        --clock->pulse_remaining;
    }
    return frame;
}

const char *pspSyncProfileName(PSPSyncProfile profile)
{
    switch (profile) {
        case PSPSYNC_PROFILE_POCKET_OPERATOR: return "PO";
        case PSPSYNC_PROFILE_VOLCA: return "VOLCA";
        case PSPSYNC_PROFILE_OFF:
        default: return "OFF";
    }
}
