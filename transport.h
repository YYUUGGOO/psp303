#ifndef PSP303_TRANSPORT_H
#define PSP303_TRANSPORT_H

#include <stdint.h>

#include "pattern.h"

#define TRANSPORT_DEFAULT_BPM 120U
#define TRANSPORT_DEFAULT_SAMPLE_RATE 44100U
#define TRANSPORT_DEFAULT_PPQN 24U
#define TRANSPORT_MIN_BPM 20U
#define TRANSPORT_MAX_BPM 300U
#define TRANSPORT_MIN_SAMPLE_RATE 1000U
#define TRANSPORT_MAX_SAMPLE_RATE 192000U
#define TRANSPORT_MIN_PPQN 1U
#define TRANSPORT_MAX_PPQN 384U
#define TRANSPORT_MAX_EVENTS 64U
#define TRANSPORT_Q32_ONE (UINT64_C(1) << 32)

typedef enum TransportSource {
    TRANSPORT_SOURCE_INTERNAL = 0,
    TRANSPORT_SOURCE_MIDI
} TransportSource;

/* Short aliases are useful when the source is the only transport setting. */
#define TRANSPORT_INTERNAL TRANSPORT_SOURCE_INTERNAL
#define TRANSPORT_MIDI TRANSPORT_SOURCE_MIDI

typedef enum TransportEventType {
    TRANSPORT_EVENT_NONE = 0,
    TRANSPORT_EVENT_STEP,
    TRANSPORT_EVENT_NOTE_ON,
    TRANSPORT_EVENT_NOTE_OFF
} TransportEventType;

typedef struct TransportConfig {
    TransportSource source;
    uint16_t bpm;
    uint32_t sample_rate;
    uint16_t ppqn;
    uint8_t swing;
    uint32_t rng_seed;
} TransportConfig;

typedef struct TransportEvent {
    TransportEventType type;
    uint8_t step_index;
    uint8_t note;
    uint8_t velocity;
    uint8_t active;
    uint8_t accent;
    uint8_t slide;
    uint8_t ratchet_index;
    uint8_t ratchet_count;
    uint32_t midi_tick;
    uint64_t time_q32;
} TransportEvent;

/* All storage, including the event queue, is part of the state object. */
typedef struct Transport {
    TransportSource source;
    uint8_t running;
    uint16_t bpm;
    uint32_t sample_rate;
    uint16_t ppqn;
    uint8_t swing;

    uint64_t phase_q32;           /* Fractional MIDI-clock phase, Q32. */
    uint64_t phase_increment_q32; /* Internal-clock increment per sample. */
    uint64_t timeline_q32;        /* Monotonic MIDI-clock time, Q32. */
    uint64_t next_step_time_q32;
    uint64_t last_whole_tick;
    uint32_t midi_tick_accumulator;
    uint32_t midi_tick_total;

    int current_index;
    int8_t pingpong_direction;
    uint32_t rng_state;
    uint8_t has_step;
    const Pattern *pattern;

    TransportEvent events[TRANSPORT_MAX_EVENTS];
    uint8_t event_head;
    uint8_t event_count;
    uint32_t dropped_events;
} Transport;

void transport_defaults(TransportConfig *config);
void transport_init(Transport *transport, const TransportConfig *config);
void transport_clamp(Transport *transport);
void transport_set_source(Transport *transport, TransportSource source);
void transport_set_bpm(Transport *transport, uint16_t bpm);
void transport_set_sample_rate(Transport *transport, uint32_t sample_rate);
void transport_set_ppqn(Transport *transport, uint16_t ppqn);
void transport_set_swing(Transport *transport, uint8_t swing);
void transport_set_pattern(Transport *transport, const Pattern *pattern);

/* reset=1 is MIDI Start; reset=0 starts without resetting phase/index. */
void transport_start(Transport *transport, const Pattern *pattern, uint8_t reset);
void transport_stop(Transport *transport);
void transport_continue(Transport *transport, const Pattern *pattern);
void transport_midi_start(Transport *transport, const Pattern *pattern);
void transport_midi_stop(Transport *transport);
void transport_midi_continue(Transport *transport, const Pattern *pattern);

/* Advance one sample or an external MIDI clock tick. Return events enqueued. */
uint32_t transport_process_sample(Transport *transport, const Pattern *pattern);
uint32_t transport_advance_samples(
    Transport *transport,
    const Pattern *pattern,
    uint32_t sample_count);
uint32_t transport_midi_clock_tick(Transport *transport, const Pattern *pattern);
uint32_t transport_tick(Transport *transport, const Pattern *pattern);

/* Return the direction-aware next step, updating ping-pong state. */
int transport_next_index(Transport *transport, const Pattern *pattern);

/* Events are returned in due-time order; zero means no event is due yet. */
int transport_poll_event(Transport *transport, TransportEvent *event);
uint32_t transport_pending_events(const Transport *transport);
uint32_t transport_dropped_events(const Transport *transport);

#endif
