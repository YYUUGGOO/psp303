#ifndef PSP303_MIDI_H
#define PSP303_MIDI_H

#include <stddef.h>
#include <stdint.h>

#define PSP303_MIDI_QUEUE_CAPACITY 64U
#define MIDI_EVENT_QUEUE_CAPACITY PSP303_MIDI_QUEUE_CAPACITY
#define MIDI_CHANNEL_COUNT 16U
#define MIDI_NOTE_COUNT 128U
#define MIDI_ANY_CHANNEL 0xFFU
#define PSP303_MIDI_CC_COUNT 8U

typedef enum MidiMessageType {
    MIDI_MESSAGE_NONE = 0,
    MIDI_MESSAGE_NOTE_OFF,
    MIDI_MESSAGE_NOTE_ON,
    MIDI_MESSAGE_CC,
    MIDI_MESSAGE_CLOCK,
    MIDI_MESSAGE_START,
    MIDI_MESSAGE_STOP,
    MIDI_MESSAGE_CONTINUE,
    MIDI_MESSAGE_SONG_POSITION
} MidiMessageType;

typedef struct MidiEvent {
    uint32_t timestamp_us;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t target;
    uint16_t value;
    MidiMessageType type;
    uint8_t channel;
} MidiEvent;

/* Compatibility aliases for callers that prefer event terminology. */
typedef MidiMessageType MidiEventType;
#define MIDI_EVENT_NONE MIDI_MESSAGE_NONE
#define MIDI_EVENT_NOTE_OFF MIDI_MESSAGE_NOTE_OFF
#define MIDI_EVENT_NOTE_ON MIDI_MESSAGE_NOTE_ON
#define MIDI_EVENT_CONTROL_CHANGE MIDI_MESSAGE_CC
#define MIDI_EVENT_CLOCK MIDI_MESSAGE_CLOCK
#define MIDI_EVENT_START MIDI_MESSAGE_START
#define MIDI_EVENT_STOP MIDI_MESSAGE_STOP
#define MIDI_EVENT_CONTINUE MIDI_MESSAGE_CONTINUE
#define MIDI_EVENT_SONG_POSITION MIDI_MESSAGE_SONG_POSITION
#define MIDI_EVENT_SPP MIDI_MESSAGE_SONG_POSITION

typedef enum MidiCcTarget {
    MIDI_CC_TARGET_CUTOFF = 0,
    MIDI_CC_TARGET_RESONANCE,
    MIDI_CC_TARGET_ENVELOPE,
    MIDI_CC_TARGET_DECAY,
    MIDI_CC_TARGET_DRIVE,
    MIDI_CC_TARGET_DELAY_TIME,
    MIDI_CC_TARGET_DELAY_FEEDBACK,
    MIDI_CC_TARGET_DELAY_MIX,
    MIDI_CC_TARGET_SWING,
    MIDI_CC_TARGET_TRANSPOSE,
    MIDI_CC_TARGET_COUNT
} MidiCcTarget;

#define MIDI_CC_TARGET_NONE 0xFFU

typedef struct MidiQueue {
    MidiEvent events[PSP303_MIDI_QUEUE_CAPACITY];
    uint8_t head;
    uint8_t count;
    uint32_t dropped;
} MidiQueue;

typedef MidiQueue MidiEventQueue;

typedef struct MidiNoteState {
    uint8_t active[MIDI_NOTE_COUNT];
    uint8_t velocity[MIDI_NOTE_COUNT];
    uint8_t current_valid;
    uint8_t current_note;
} MidiNoteState;

typedef struct MidiCcBinding {
    uint8_t enabled;
    uint8_t channel;
    uint8_t cc;
} MidiCcBinding;

typedef struct MidiCcMap {
    MidiCcBinding bindings[MIDI_CC_TARGET_COUNT];
} MidiCcMap;

typedef struct MidiControlState {
    MidiQueue queue;
    MidiNoteState notes;
    int16_t cc_parameter[128];
    uint8_t learn_active;
    uint8_t learn_parameter;
    uint8_t running;
    uint32_t clock_count;
    uint16_t song_position;

    MidiCcMap cc_map;
    uint8_t running_status;
    uint8_t pending_status;
    uint8_t pending_data_count;
    uint8_t pending_data1;
    uint8_t sysex;
    uint8_t spp_enabled;
    uint8_t active_notes[MIDI_CHANNEL_COUNT][MIDI_NOTE_COUNT];
} MidiControlState;

typedef MidiControlState MidiParser;

/* The MIDI receive side and the consumer side must be single producer and
   single consumer respectively. This queue is fixed-size and allocation-free;
   callers must not reset it while the other thread/callback is using it. */
void midi_queue_init(MidiQueue *queue);
int midi_queue_push(MidiQueue *queue, const MidiEvent *event);
int midi_queue_pop(MidiQueue *queue, MidiEvent *event);

void midi_event_queue_init(MidiEventQueue *queue);
void midi_event_queue_reset(MidiEventQueue *queue);
int midi_event_queue_push(MidiEventQueue *queue, const MidiEvent *event);
int midi_event_queue_pop(MidiEventQueue *queue, MidiEvent *event);
size_t midi_event_queue_count(const MidiEventQueue *queue);
uint32_t midi_event_queue_dropped(const MidiEventQueue *queue);

MidiMessageType midi_event_type(const MidiEvent *event);

int midi_note_on(MidiNoteState *notes, uint8_t note, uint8_t velocity);
int midi_note_off(MidiNoteState *notes, uint8_t note);
void midi_all_notes_off(MidiNoteState *notes);

void midi_control_init(MidiControlState *state);
void midi_control_process(MidiControlState *state, const MidiEvent *event);
int midi_cc_value(const MidiControlState *state,
                  uint8_t cc,
                  uint8_t *parameter);
int midi_learn_begin(MidiControlState *state, uint8_t parameter);
int midi_learn_process_cc(MidiControlState *state, uint8_t cc);
int midi_learn_clear(MidiControlState *state, uint8_t parameter);
uint8_t midi_map_7bit(uint8_t value, uint8_t maximum);

void midi_cc_map_init(MidiCcMap *map);
int midi_cc_map_learn(MidiCcMap *map,
                      uint8_t target,
                      uint8_t channel,
                      uint8_t cc);
int midi_cc_map_clear(MidiCcMap *map, uint8_t target);
int midi_cc_map_get(const MidiCcMap *map,
                    uint8_t target,
                    MidiCcBinding *binding);
int midi_cc_map_lookup(const MidiCcMap *map,
                       uint8_t channel,
                       uint8_t cc,
                       uint8_t *target);

/* Byte parsing is suitable for a realtime MIDI callback. Events cross to the
   consumer through the fixed queue; file/UI work belongs outside this path. */
void midi_parser_init(MidiParser *parser);
void midi_parser_reset(MidiParser *parser);
void midi_parser_set_spp_enabled(MidiParser *parser, int enabled);
int midi_parser_feed_byte(MidiParser *parser, uint8_t byte);
size_t midi_parser_feed(MidiParser *parser,
                         const uint8_t *bytes,
                         size_t count);
int midi_parser_next_event(MidiParser *parser, MidiEvent *event);
size_t midi_parser_all_notes_off(MidiParser *parser, uint8_t channel);
int midi_parser_is_note_active(const MidiParser *parser,
                               uint8_t channel,
                               uint8_t note);
uint32_t midi_parser_dropped_events(const MidiParser *parser);

#endif
