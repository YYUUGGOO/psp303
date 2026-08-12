#include "midi.h"

#include <string.h>

static const uint8_t default_ccs[MIDI_CC_TARGET_COUNT] = {
    74U, 71U, 0U, 0U, 0U, 0U, 0U, 0U, 80U, 81U
};

static int valid_channel(uint8_t channel)
{
    return channel < MIDI_CHANNEL_COUNT || channel == MIDI_ANY_CHANNEL;
}

void midi_queue_init(MidiQueue *queue)
{
    if (queue == NULL) return;
    memset(queue, 0, sizeof(*queue));
}

int midi_queue_push(MidiQueue *queue, const MidiEvent *event)
{
    uint8_t index;

    if (queue == NULL || event == NULL) return -1;
    if (queue->count >= PSP303_MIDI_QUEUE_CAPACITY) {
        ++queue->dropped;
        return -1;
    }
    index = (uint8_t)((queue->head + queue->count) % PSP303_MIDI_QUEUE_CAPACITY);
    queue->events[index] = *event;
    ++queue->count;
    return 0;
}

int midi_queue_pop(MidiQueue *queue, MidiEvent *event)
{
    if (queue == NULL || event == NULL) return 0;
    if (queue->count == 0U) return 0;
    *event = queue->events[queue->head];
    queue->head = (uint8_t)((queue->head + 1U) % PSP303_MIDI_QUEUE_CAPACITY);
    --queue->count;
    return 1;
}

void midi_event_queue_init(MidiEventQueue *queue)
{
    midi_queue_init(queue);
}

void midi_event_queue_reset(MidiEventQueue *queue)
{
    midi_queue_init(queue);
}

int midi_event_queue_push(MidiEventQueue *queue, const MidiEvent *event)
{
    return midi_queue_push(queue, event) == 0 ? 1 : 0;
}

int midi_event_queue_pop(MidiEventQueue *queue, MidiEvent *event)
{
    return midi_queue_pop(queue, event);
}

size_t midi_event_queue_count(const MidiEventQueue *queue)
{
    return queue == NULL ? 0U : queue->count;
}

uint32_t midi_event_queue_dropped(const MidiEventQueue *queue)
{
    return queue == NULL ? 0U : queue->dropped;
}

MidiMessageType midi_event_type(const MidiEvent *event)
{
    uint8_t command;

    if (event == NULL) return MIDI_MESSAGE_NONE;
    if (event->status == 0xF8U) return MIDI_MESSAGE_CLOCK;
    if (event->status == 0xFAU) return MIDI_MESSAGE_START;
    if (event->status == 0xFCU) return MIDI_MESSAGE_STOP;
    if (event->status == 0xFBU) return MIDI_MESSAGE_CONTINUE;
    if (event->status == 0xF2U) return MIDI_MESSAGE_SONG_POSITION;
    command = (uint8_t)(event->status & 0xF0U);
    if (command == 0x80U) return MIDI_MESSAGE_NOTE_OFF;
    if (command == 0x90U) return event->data2 == 0U
        ? MIDI_MESSAGE_NOTE_OFF : MIDI_MESSAGE_NOTE_ON;
    if (command == 0xB0U) return MIDI_MESSAGE_CC;
    return MIDI_MESSAGE_NONE;
}

static void select_current_note(MidiNoteState *notes)
{
    int note;

    notes->current_valid = 0U;
    for (note = (int)MIDI_NOTE_COUNT - 1; note >= 0; --note) {
        if (notes->active[note] != 0U) {
            notes->current_note = (uint8_t)note;
            notes->current_valid = 1U;
            return;
        }
    }
}

int midi_note_on(MidiNoteState *notes, uint8_t note, uint8_t velocity)
{
    if (notes == NULL || note >= MIDI_NOTE_COUNT || velocity == 0U) return -1;
    notes->active[note] = 1U;
    notes->velocity[note] = velocity;
    notes->current_note = note;
    notes->current_valid = 1U;
    return 0;
}

int midi_note_off(MidiNoteState *notes, uint8_t note)
{
    if (notes == NULL || note >= MIDI_NOTE_COUNT) return -1;
    notes->active[note] = 0U;
    notes->velocity[note] = 0U;
    if (notes->current_valid != 0U && notes->current_note == note) select_current_note(notes);
    return 0;
}

void midi_all_notes_off(MidiNoteState *notes)
{
    if (notes == NULL) return;
    memset(notes, 0, sizeof(*notes));
}

void midi_cc_map_init(MidiCcMap *map)
{
    unsigned int i;

    if (map == NULL) return;
    memset(map, 0, sizeof(*map));
    for (i = 0U; i < MIDI_CC_TARGET_COUNT; ++i) {
        map->bindings[i].channel = MIDI_ANY_CHANNEL;
        map->bindings[i].cc = default_ccs[i];
        map->bindings[i].enabled = (i < 2U || i >= 8U) && default_ccs[i] != 0U;
    }
}

int midi_cc_map_learn(MidiCcMap *map, uint8_t target, uint8_t channel, uint8_t cc)
{
    if (map == NULL || target >= MIDI_CC_TARGET_COUNT
        || !valid_channel(channel) || cc > 127U) return -1;
    map->bindings[target].enabled = 1U;
    map->bindings[target].channel = channel;
    map->bindings[target].cc = cc;
    return 0;
}

int midi_cc_map_clear(MidiCcMap *map, uint8_t target)
{
    if (map == NULL || target >= MIDI_CC_TARGET_COUNT) return -1;
    map->bindings[target].enabled = 0U;
    map->bindings[target].channel = MIDI_ANY_CHANNEL;
    map->bindings[target].cc = 0U;
    return 0;
}

int midi_cc_map_get(const MidiCcMap *map, uint8_t target, MidiCcBinding *binding)
{
    if (map == NULL || binding == NULL || target >= MIDI_CC_TARGET_COUNT) return -1;
    *binding = map->bindings[target];
    return 0;
}

int midi_cc_map_lookup(const MidiCcMap *map, uint8_t channel, uint8_t cc, uint8_t *target)
{
    unsigned int i;

    if (map == NULL || target == NULL || channel >= MIDI_CHANNEL_COUNT || cc > 127U) return -1;
    *target = MIDI_CC_TARGET_NONE;
    for (i = 0U; i < MIDI_CC_TARGET_COUNT; ++i) {
        const MidiCcBinding *binding = &map->bindings[i];
        if (binding->enabled != 0U && binding->cc == cc
            && (binding->channel == MIDI_ANY_CHANNEL || binding->channel == channel)) {
            *target = (uint8_t)i;
            return 1;
        }
    }
    return 0;
}

static void sync_channel_note_state(MidiControlState *state, uint8_t channel, uint8_t note,
                                     uint8_t active, uint8_t velocity)
{
    if (channel == 0U) {
        if (active != 0U) (void)midi_note_on(&state->notes, note, velocity);
        else (void)midi_note_off(&state->notes, note);
    }
}

void midi_control_init(MidiControlState *state)
{
    unsigned int i;

    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    midi_queue_init(&state->queue);
    midi_cc_map_init(&state->cc_map);
    for (i = 0U; i < 128U; ++i) state->cc_parameter[i] = -1;
    state->cc_parameter[74U] = 0;
    state->cc_parameter[71U] = 1;
}

void midi_control_process(MidiControlState *state, const MidiEvent *event)
{
    MidiMessageType type;

    if (state == NULL || event == NULL) return;
    type = midi_event_type(event);
    switch (type) {
        case MIDI_MESSAGE_NOTE_ON:
            if (event->status == 0x90U) {
                sync_channel_note_state(state, 0U, event->data1, 1U, event->data2);
            }
            break;
        case MIDI_MESSAGE_NOTE_OFF:
            if ((event->status & 0xF0U) == 0x80U || event->data2 == 0U) {
                sync_channel_note_state(state, (uint8_t)(event->status & 0x0FU),
                                         event->data1, 0U, 0U);
            }
            break;
        case MIDI_MESSAGE_CLOCK: ++state->clock_count; break;
        case MIDI_MESSAGE_START: state->running = 1U; state->clock_count = 0U; break;
        case MIDI_MESSAGE_CONTINUE: state->running = 1U; break;
        case MIDI_MESSAGE_STOP:
            state->running = 0U;
            midi_all_notes_off(&state->notes);
            memset(state->active_notes, 0, sizeof(state->active_notes));
            break;
        case MIDI_MESSAGE_SONG_POSITION:
            state->song_position = (uint16_t)event->data1
                                 | (uint16_t)((uint16_t)event->data2 << 7U);
            break;
        case MIDI_MESSAGE_CC:
        case MIDI_MESSAGE_NONE:
        default: break;
    }
}

int midi_cc_value(const MidiControlState *state, uint8_t cc, uint8_t *parameter)
{
    int16_t value;

    if (state == NULL || parameter == NULL || cc >= 128U) return -1;
    value = state->cc_parameter[cc];
    if (value < 0) return -1;
    *parameter = (uint8_t)value;
    return 0;
}

int midi_learn_begin(MidiControlState *state, uint8_t parameter)
{
    if (state == NULL || parameter >= PSP303_MIDI_CC_COUNT) return -1;
    state->learn_active = 1U;
    state->learn_parameter = parameter;
    return 0;
}

int midi_learn_process_cc(MidiControlState *state, uint8_t cc)
{
    unsigned int i;

    if (state == NULL || cc >= 128U || state->learn_active == 0U) return -1;
    for (i = 0U; i < 128U; ++i) {
        if (state->cc_parameter[i] == state->learn_parameter) state->cc_parameter[i] = -1;
    }
    state->cc_parameter[cc] = state->learn_parameter;
    state->learn_active = 0U;
    return 0;
}

int midi_learn_clear(MidiControlState *state, uint8_t parameter)
{
    unsigned int i;

    if (state == NULL || parameter >= PSP303_MIDI_CC_COUNT) return -1;
    for (i = 0U; i < 128U; ++i) {
        if (state->cc_parameter[i] == parameter) state->cc_parameter[i] = -1;
    }
    return 0;
}

uint8_t midi_map_7bit(uint8_t value, uint8_t maximum)
{
    if (value >= 127U) return maximum;
    return (uint8_t)(((uint16_t)value * maximum) / 127U);
}

static void clear_active_notes(MidiParser *parser, uint8_t channel)
{
    if (channel == MIDI_ANY_CHANNEL) memset(parser->active_notes, 0, sizeof(parser->active_notes));
    else if (channel < MIDI_CHANNEL_COUNT) memset(parser->active_notes[channel], 0, MIDI_NOTE_COUNT);
}

static unsigned int data_bytes_for_status(uint8_t status)
{
    uint8_t command = (uint8_t)(status & 0xF0U);
    return command == 0xC0U || command == 0xD0U ? 1U : 2U;
}

static void emit_parsed_event(MidiParser *parser, uint8_t status, uint8_t data1, uint8_t data2)
{
    MidiEvent event;
    uint8_t command = (uint8_t)(status & 0xF0U);
    uint8_t channel = (uint8_t)(status & 0x0FU);

    memset(&event, 0, sizeof(event));
    event.status = status;
    event.data1 = data1;
    event.data2 = data2;
    event.target = MIDI_CC_TARGET_NONE;
    event.channel = channel;
    if (command == 0x80U || command == 0x90U) {
        uint8_t active = (uint8_t)(command == 0x90U && data2 != 0U);
        event.status = active != 0U ? status : (uint8_t)(0x80U | channel);
        parser->active_notes[channel][data1] = active;
        sync_channel_note_state(parser, channel, data1, active, data2);
    } else if (command == 0xB0U) {
        (void)midi_cc_map_lookup(&parser->cc_map, channel, data1, &event.target);
        if (data1 == 120U || data1 == 123U) clear_active_notes(parser, channel);
    } else {
        return;
    }
    event.type = midi_event_type(&event);
    (void)midi_queue_push(&parser->queue, &event);
}

void midi_parser_init(MidiParser *parser)
{
    midi_control_init(parser);
}

void midi_parser_reset(MidiParser *parser)
{
    uint8_t spp_enabled;
    if (parser == NULL) return;
    spp_enabled = parser->spp_enabled;
    midi_control_init(parser);
    parser->spp_enabled = spp_enabled;
}

void midi_parser_set_spp_enabled(MidiParser *parser, int enabled)
{
    if (parser != NULL) parser->spp_enabled = enabled != 0 ? 1U : 0U;
}

static void emit_realtime(MidiParser *parser, uint8_t byte)
{
    MidiEvent event;
    memset(&event, 0, sizeof(event));
    event.status = byte;
    event.target = MIDI_CC_TARGET_NONE;
    if (byte != 0xF8U && byte != 0xFAU && byte != 0xFBU && byte != 0xFCU) return;
    event.type = midi_event_type(&event);
    if (byte == 0xFCU) {
        clear_active_notes(parser, MIDI_ANY_CHANNEL);
        midi_all_notes_off(&parser->notes);
    }
    (void)midi_queue_push(&parser->queue, &event);
}

int midi_parser_feed_byte(MidiParser *parser, uint8_t byte)
{
    unsigned int expected;
    if (parser == NULL) return 0;
    if (byte >= 0xF8U) { emit_realtime(parser, byte); return 1; }
    if ((byte & 0x80U) != 0U) {
        if (byte == 0xF0U) {
            parser->sysex = 1U; parser->running_status = 0U;
            parser->pending_status = 0U; parser->pending_data_count = 0U;
        } else if (byte == 0xF7U) {
            parser->sysex = 0U; parser->pending_status = 0U; parser->pending_data_count = 0U;
        } else if (byte <= 0xEFU) {
            parser->sysex = 0U; parser->running_status = byte;
            parser->pending_status = 0U; parser->pending_data_count = 0U;
        } else if (byte == 0xF1U || byte == 0xF2U || byte == 0xF3U) {
            parser->sysex = 0U; parser->running_status = 0U;
            parser->pending_status = byte; parser->pending_data_count = 0U;
        } else {
            parser->sysex = 0U; parser->running_status = 0U;
            parser->pending_status = 0U; parser->pending_data_count = 0U;
        }
        return 1;
    }
    if (parser->sysex != 0U) return 1;
    if (parser->pending_status != 0U) {
        if (parser->pending_data_count == 0U) parser->pending_data1 = byte;
        ++parser->pending_data_count;
        if (parser->pending_data_count >= (parser->pending_status == 0xF2U ? 2U : 1U)) {
            if (parser->pending_status == 0xF2U) {
                parser->song_position = (uint16_t)parser->pending_data1 | (uint16_t)(byte << 7U);
                if (parser->spp_enabled != 0U) {
                    MidiEvent event = {0U, 0xF2U, parser->pending_data1, byte,
                                       MIDI_CC_TARGET_NONE, parser->song_position,
                                       MIDI_MESSAGE_SONG_POSITION, 0U};
                    (void)midi_queue_push(&parser->queue, &event);
                }
            }
            parser->pending_status = 0U; parser->pending_data_count = 0U;
        }
        return 1;
    }
    if (parser->running_status == 0U) return 1;
    expected = data_bytes_for_status(parser->running_status);
    if (parser->pending_data_count == 0U) parser->pending_data1 = byte;
    ++parser->pending_data_count;
    if (parser->pending_data_count >= expected) {
        emit_parsed_event(parser, parser->running_status, parser->pending_data1,
                          expected == 2U ? byte : 0U);
        parser->pending_data_count = 0U;
    }
    return 1;
}

size_t midi_parser_feed(MidiParser *parser, const uint8_t *bytes, size_t count)
{
    size_t i;
    if (parser == NULL || bytes == NULL) return 0U;
    for (i = 0U; i < count; ++i) (void)midi_parser_feed_byte(parser, bytes[i]);
    return count;
}

int midi_parser_next_event(MidiParser *parser, MidiEvent *event)
{
    return parser == NULL ? 0 : midi_queue_pop(&parser->queue, event);
}

size_t midi_parser_all_notes_off(MidiParser *parser, uint8_t channel)
{
    size_t first = 0U, last = MIDI_CHANNEL_COUNT, pushed = 0U, c, note;
    if (parser == NULL || !valid_channel(channel)) return 0U;
    if (channel != MIDI_ANY_CHANNEL) { first = channel; last = first + 1U; }
    for (c = first; c < last; ++c) for (note = 0U; note < MIDI_NOTE_COUNT; ++note) {
        if (parser->active_notes[c][note] != 0U) {
            MidiEvent event = {0U, (uint8_t)(0x80U | c), (uint8_t)note, 0U,
                               MIDI_CC_TARGET_NONE, 0U, MIDI_MESSAGE_NOTE_OFF,
                               (uint8_t)c};
            if (midi_queue_push(&parser->queue, &event) == 0) ++pushed;
            parser->active_notes[c][note] = 0U;
            if (c == 0U) (void)midi_note_off(&parser->notes, (uint8_t)note);
        }
    }
    return pushed;
}

int midi_parser_is_note_active(const MidiParser *parser, uint8_t channel, uint8_t note)
{
    if (parser == NULL || channel >= MIDI_CHANNEL_COUNT || note >= MIDI_NOTE_COUNT) return 0;
    return parser->active_notes[channel][note] != 0U;
}

uint32_t midi_parser_dropped_events(const MidiParser *parser)
{
    return parser == NULL ? 0U : parser->queue.dropped;
}
