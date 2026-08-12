#include "midi.h"

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

static void test_queue_fifo_and_overflow(void)
{
    MidiEventQueue queue;
    MidiEvent input;
    MidiEvent output;
    unsigned int i;

    midi_event_queue_init(&queue);
    memset(&input, 0, sizeof(input));
    for (i = 0U; i < MIDI_EVENT_QUEUE_CAPACITY; ++i) {
        input.data1 = (uint8_t)i;
        CHECK(midi_event_queue_push(&queue, &input) == 1);
    }
    CHECK(midi_event_queue_push(&queue, &input) == 0);
    CHECK(midi_event_queue_dropped(&queue) == 1U);
    CHECK(midi_event_queue_count(&queue) == MIDI_EVENT_QUEUE_CAPACITY);
    for (i = 0U; i < MIDI_EVENT_QUEUE_CAPACITY; ++i) {
        CHECK(midi_event_queue_pop(&queue, &output) == 1);
        CHECK(output.data1 == (uint8_t)i);
    }
    CHECK(midi_event_queue_pop(&queue, &output) == 0);
    midi_event_queue_reset(&queue);
    CHECK(midi_event_queue_count(&queue) == 0U);
    CHECK(midi_event_queue_dropped(&queue) == 0U);
}

static void test_cc_defaults_learn_and_lookup(void)
{
    MidiCcMap map;
    MidiCcBinding binding;
    uint8_t target = MIDI_CC_TARGET_NONE;

    midi_cc_map_init(&map);
    CHECK(midi_cc_map_get(&map, MIDI_CC_TARGET_CUTOFF, &binding) == 0);
    CHECK(binding.enabled != 0U && binding.cc == 74U);
    CHECK(midi_cc_map_lookup(&map, 0U, 74U, &target) == 1);
    CHECK(target == MIDI_CC_TARGET_CUTOFF);
    CHECK(midi_cc_map_lookup(&map, 2U, 74U, &target) == 1);
    CHECK(midi_cc_map_learn(&map, MIDI_CC_TARGET_CUTOFF, 2U, 20U) == 0);
    CHECK(midi_cc_map_lookup(&map, 0U, 20U, &target) == 0);
    CHECK(midi_cc_map_lookup(&map, 2U, 20U, &target) == 1);
    CHECK(target == MIDI_CC_TARGET_CUTOFF);
    CHECK(midi_cc_map_clear(&map, MIDI_CC_TARGET_CUTOFF) == 0);
    CHECK(midi_cc_map_lookup(&map, 2U, 20U, &target) == 0);
    CHECK(midi_cc_map_learn(&map, MIDI_CC_TARGET_COUNT, 0U, 1U) < 0);
    CHECK(midi_cc_map_learn(&map, MIDI_CC_TARGET_CUTOFF, 16U, 1U) < 0);
}

static void test_parser_note_state_running_status_and_realtime(void)
{
    MidiParser parser;
    MidiEvent event;
    static const uint8_t bytes[] = {0x90U, 60U, 100U, 64U, 0U, 0xB0U, 74U, 63U};

    midi_parser_init(&parser);
    CHECK(midi_parser_feed(&parser, bytes, sizeof(bytes)) == sizeof(bytes));
    CHECK(midi_parser_is_note_active(&parser, 0U, 60U) != 0);
    CHECK(midi_parser_next_event(&parser, &event) == 1);
    CHECK(event.type == MIDI_EVENT_NOTE_ON && event.channel == 0U);
    CHECK(event.data1 == 60U && event.data2 == 100U);
    CHECK(midi_parser_next_event(&parser, &event) == 1);
    CHECK(event.type == MIDI_EVENT_NOTE_OFF && event.data1 == 64U);
    CHECK(midi_parser_next_event(&parser, &event) == 1);
    CHECK(event.type == MIDI_EVENT_CONTROL_CHANGE);
    CHECK(event.target == MIDI_CC_TARGET_CUTOFF && event.data2 == 63U);

    midi_parser_feed_byte(&parser, 0x90U);
    midi_parser_feed_byte(&parser, 60U);
    midi_parser_feed_byte(&parser, 110U);
    CHECK(midi_parser_is_note_active(&parser, 0U, 60U) != 0);
    midi_parser_feed_byte(&parser, 0xF8U);
    CHECK(midi_parser_next_event(&parser, &event) == 1);
    CHECK(event.type == MIDI_EVENT_NOTE_ON);
    CHECK(midi_parser_next_event(&parser, &event) == 1);
    CHECK(event.type == MIDI_EVENT_CLOCK);
    midi_parser_feed_byte(&parser, 0xFCU);
    CHECK(midi_parser_is_note_active(&parser, 0U, 60U) == 0);
}

static void test_learned_cc_spp_and_all_notes_off(void)
{
    MidiParser parser;
    MidiEvent event;
    static const uint8_t spp[] = {0xF2U, 0x34U, 0x12U};

    midi_parser_init(&parser);
    midi_parser_set_spp_enabled(&parser, 1);
    CHECK(midi_parser_feed(&parser, spp, sizeof(spp)) == sizeof(spp));
    CHECK(midi_parser_next_event(&parser, &event) == 1);
    CHECK(event.type == MIDI_EVENT_SONG_POSITION);
    CHECK(event.value == (uint16_t)(0x34U | (0x12U << 7)));

    midi_parser_feed(&parser, (const uint8_t[]){0x90U, 61U, 100U}, 3U);
    midi_parser_feed(&parser, (const uint8_t[]){0x91U, 61U, 100U}, 3U);
    CHECK(midi_parser_all_notes_off(&parser, 0U) == 1U);
    CHECK(midi_parser_is_note_active(&parser, 0U, 61U) == 0);
    CHECK(midi_parser_is_note_active(&parser, 1U, 61U) != 0);
    CHECK(midi_parser_all_notes_off(&parser, MIDI_ANY_CHANNEL) == 1U);
    CHECK(midi_parser_is_note_active(&parser, 1U, 61U) == 0);
}

int main(void)
{
    test_queue_fifo_and_overflow();
    test_cc_defaults_learn_and_lookup();
    test_parser_note_state_running_status_and_realtime();
    test_learned_cc_spp_and_all_notes_off();
    if (failures != 0U) {
        fprintf(stderr, "%u MIDI test(s) failed\n", failures);
        return 1;
    }
    puts("MIDI tests passed");
    return 0;
}
