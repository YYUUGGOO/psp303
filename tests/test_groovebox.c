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

static void test_length_and_directions(void)
{
    Pattern pattern;
    int pingpong_direction = 1;
    uint32_t random_state = 1U;

    pattern_init(&pattern);
    pattern.length = 2U;
    pattern_clamp(&pattern);
    CHECK(pattern.length == PSP303_MIN_STEPS);
    pattern.length = 200U;
    pattern_clamp(&pattern);
    CHECK(pattern.length == PSP303_MAX_STEPS);

    pattern.length = 4U;
    pattern.direction = PATTERN_FORWARD;
    CHECK(pattern_next_index(&pattern, -1, &random_state) == 0);
    CHECK(pattern_next_index(&pattern, 0, &random_state) == 1);
    CHECK(pattern_next_index(&pattern, 3, &random_state) == 0);

    pattern.direction = PATTERN_REVERSE;
    CHECK(pattern_next_index(&pattern, -1, &random_state) == 3);
    CHECK(pattern_next_index(&pattern, 0, &random_state) == 3);
    CHECK(pattern_next_index(&pattern, 3, &random_state) == 2);
    CHECK(pattern_previous_index(&pattern, 0) == 3);

    pattern.direction = PATTERN_PING_PONG;
    pingpong_direction = 1;
    CHECK(pattern_next_index_with_direction(&pattern, -1, &pingpong_direction, &random_state) == 0);
    CHECK(pattern_next_index_with_direction(&pattern, 0, &pingpong_direction, &random_state) == 1);
    CHECK(pattern_next_index_with_direction(&pattern, 1, &pingpong_direction, &random_state) == 2);
    CHECK(pattern_next_index_with_direction(&pattern, 3, &pingpong_direction, &random_state) == 2);
    CHECK(pingpong_direction == -1);
    CHECK(pattern_next_index_with_direction(&pattern, 2, &pingpong_direction, &random_state) == 1);
    CHECK(pattern_next_index_with_direction(&pattern, 0, &pingpong_direction, &random_state) == 1);
    CHECK(pingpong_direction == 1);

    pattern.direction = PATTERN_RANDOM;
    random_state = 0U;
    CHECK(pattern_next_index(&pattern, 0, &random_state) < 4);
}

static void test_probability_edges(void)
{
    Pattern pattern;

    pattern_init(&pattern);
    pattern.steps[0].probability = 0U;
    pattern.steps[1].probability = 100U;
    pattern.steps[2].probability = 255U;
    pattern_clamp(&pattern);
    CHECK(pattern.steps[0].probability == 0U);
    CHECK(pattern.steps[1].probability == 100U);
    CHECK(pattern.steps[2].probability == 100U);
    pattern.steps[0].gate = 0U;
    pattern.steps[1].gate = 100U;
    CHECK(pattern.steps[0].gate == 0U);
    CHECK(pattern.steps[1].gate == 100U);
}

static void test_ratchet_and_gate_timing(void)
{
    Pattern pattern;
    TransportConfig config;
    Transport transport;
    TransportEvent event;
    unsigned int note_on_count = 0U;
    unsigned int note_off_count = 0U;
    uint64_t first_on = 0U;
    uint64_t first_off = 0U;

    pattern_init(&pattern);
    pattern.length = 3U;
    memset(pattern.steps, 0, sizeof(pattern.steps));
    pattern.steps[0].note = 48U;
    pattern.steps[0].active = 1U;
    pattern.steps[0].probability = 100U;
    pattern.steps[0].ratchet_count = 4U;
    pattern.steps[0].gate = 50U;
    transport_defaults(&config);
    config.source = TRANSPORT_SOURCE_MIDI;
    config.rng_seed = 1U;
    transport_init(&transport, &config);
    transport_start(&transport, &pattern, 1U);
    {
        unsigned int tick;
        for (tick = 0U; tick <= 11U; ++tick) {
            while (transport_poll_event(&transport, &event) != 0) {
                if (event.type == TRANSPORT_EVENT_NOTE_ON) {
                    ++note_on_count;
                    if (note_on_count == 1U) first_on = event.time_q32;
                    CHECK(event.note == 48U);
                    CHECK(event.ratchet_count == 4U);
                } else if (event.type == TRANSPORT_EVENT_NOTE_OFF) {
                    ++note_off_count;
                    if (note_off_count == 1U) first_off = event.time_q32;
                }
            }
            if (tick < 11U) (void)transport_midi_clock_tick(&transport, &pattern);
        }
    }
    CHECK(note_on_count == 4U);
    CHECK(note_off_count == 4U);
    CHECK(first_off > first_on);
    CHECK(transport_pending_events(&transport) == 0U);

    pattern.steps[0].probability = 0U;
    transport_start(&transport, &pattern, 1U);
    note_on_count = 0U;
    while (transport_poll_event(&transport, &event) != 0) {
        if (event.type == TRANSPORT_EVENT_NOTE_ON) ++note_on_count;
    }
    CHECK(note_on_count == 0U);
}

int main(void)
{
    test_length_and_directions();
    test_probability_edges();
    test_ratchet_and_gate_timing();
    if (failures != 0U) {
        fprintf(stderr, "%u groovebox test(s) failed\n", failures);
        return 1;
    }
    puts("groovebox tests passed");
    return 0;
}
