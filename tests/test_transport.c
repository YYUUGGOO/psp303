#include "pattern.h"
#include "transport.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

static void test_defaults_clamps_and_start_modes(void)
{
    Pattern pattern;
    TransportConfig config;
    Transport transport;

    pattern_init(&pattern);
    transport_defaults(&config);
    CHECK(config.bpm == TRANSPORT_DEFAULT_BPM);
    CHECK(config.sample_rate == TRANSPORT_DEFAULT_SAMPLE_RATE);
    config.bpm = 1U;
    config.sample_rate = 1U;
    config.ppqn = 0U;
    config.swing = 100U;
    transport_init(&transport, &config);
    CHECK(transport.bpm == TRANSPORT_MIN_BPM);
    CHECK(transport.sample_rate == TRANSPORT_MIN_SAMPLE_RATE);
    CHECK(transport.ppqn == TRANSPORT_DEFAULT_PPQN);
    CHECK(transport.swing == 75U);
    transport_set_bpm(&transport, 500U);
    transport_set_sample_rate(&transport, 999999U);
    transport_set_ppqn(&transport, 999U);
    transport_set_swing(&transport, 0U);
    CHECK(transport.bpm == TRANSPORT_MAX_BPM);
    CHECK(transport.sample_rate == TRANSPORT_MAX_SAMPLE_RATE);
    CHECK(transport.ppqn == TRANSPORT_MAX_PPQN);
    CHECK(transport.swing == 50U);

    transport_start(&transport, &pattern, 1U);
    CHECK(transport.running != 0U);
    CHECK(transport_pending_events(&transport) != 0U);
    transport_stop(&transport);
    CHECK(transport.running == 0U);
    transport_continue(&transport, &pattern);
    CHECK(transport.running != 0U);
}

static void test_swing_and_fractional_clock(void)
{
    Pattern pattern;
    TransportConfig config;
    Transport transport;
    TransportEvent event;
    uint32_t ticks;

    pattern_init(&pattern);
    pattern.length = 4U;
    memset(pattern.steps, 0, sizeof(pattern.steps));
    pattern.steps[0].probability = 0U;
    pattern.steps[1].probability = 0U;
    transport_defaults(&config);
    config.source = TRANSPORT_SOURCE_MIDI;
    config.swing = 75U;
    config.rng_seed = 1U;
    transport_init(&transport, &config);
    transport_start(&transport, &pattern, 1U);
    CHECK(transport_poll_event(&transport, &event) == 1);
    CHECK(event.type == TRANSPORT_EVENT_STEP && event.step_index == 0U);
    for (ticks = 0U; ticks < 8U; ++ticks) {
        (void)transport_midi_clock_tick(&transport, &pattern);
        CHECK(transport_pending_events(&transport) == 0U);
    }
    (void)transport_midi_clock_tick(&transport, &pattern);
    CHECK(transport_poll_event(&transport, &event) == 1);
    CHECK(event.type == TRANSPORT_EVENT_STEP && event.step_index == 1U);
    for (ticks = 0U; ticks < 9U; ++ticks) {
        (void)transport_midi_clock_tick(&transport, &pattern);
    }
    CHECK(transport_poll_event(&transport, &event) == 1);
    CHECK(event.step_index == 2U);

    config.source = TRANSPORT_SOURCE_INTERNAL;
    config.sample_rate = 44100U;
    config.bpm = 123U;
    config.swing = 50U;
    transport_init(&transport, &config);
    CHECK((transport.phase_increment_q32 & UINT32_MAX) != 0U);
    transport_start(&transport, &pattern, 1U);
    CHECK(transport.timeline_q32 == 0U);
    CHECK(transport_advance_samples(&transport, &pattern, 100U) == 0U);
    CHECK(transport_advance_samples(&transport, &pattern, 20000U) > 0U);
    CHECK(transport.timeline_q32 > 0U);
    CHECK(transport.phase_q32 < TRANSPORT_Q32_ONE);
}

static void test_clock_transport_and_queue_overflow(void)
{
    Pattern pattern;
    TransportConfig config;
    Transport transport;
    unsigned int i;

    pattern_init(&pattern);
    memset(pattern.steps, 0, sizeof(pattern.steps));
    pattern.steps[0].active = 1U;
    pattern.steps[0].probability = 100U;
    pattern.steps[0].ratchet_count = 4U;
    pattern.steps[0].gate = 100U;
    transport_defaults(&config);
    config.source = TRANSPORT_SOURCE_MIDI;
    config.rng_seed = 1U;
    transport_init(&transport, &config);
    transport_start(&transport, &pattern, 1U);
    for (i = 0U; i < 1000U; ++i) {
        (void)transport_midi_clock_tick(&transport, &pattern);
    }
    CHECK(transport_pending_events(&transport) <= TRANSPORT_MAX_EVENTS);
    CHECK(transport_dropped_events(&transport) > 0U);
    transport_midi_stop(&transport);
    CHECK(transport.running == 0U);
    CHECK(transport_midi_clock_tick(&transport, &pattern) == 0U);
}

int main(void)
{
    test_defaults_clamps_and_start_modes();
    test_swing_and_fractional_clock();
    test_clock_transport_and_queue_overflow();
    if (failures != 0U) {
        fprintf(stderr, "%u transport test(s) failed\n", failures);
        return 1;
    }
    puts("transport tests passed");
    return 0;
}
