#ifndef PSP303_PATTERN_H
#define PSP303_PATTERN_H

/* Pattern data is deliberately plain C so it can be shared with audio code. */
#include <stdint.h>

#define PSP303_MAX_STEPS 16U
#define PSP303_MIN_STEPS 3U
#define PSP303_MIN_NOTE 0U
#define PSP303_MAX_NOTE 127U
#define PSP303_MIN_TRANSPOSE (-24)
#define PSP303_MAX_TRANSPOSE 24

typedef enum PatternDirection {
    PATTERN_FORWARD = 0,
    PATTERN_REVERSE,
    PATTERN_PING_PONG,
    PATTERN_RANDOM
} PatternDirection;

typedef struct PatternStep {
    uint8_t note;          /* MIDI note number, 0..127. */
    uint8_t active;        /* Boolean. */
    uint8_t accent;        /* Boolean. */
    uint8_t slide;         /* Boolean; slide into the following note. */
    uint8_t probability;   /* Trigger probability, 0..100 percent. */
    uint8_t ratchet_count; /* Number of attacks in the step, 1..4. */
    uint8_t gate;          /* Note length inside each ratchet, 0..100 percent. */
} PatternStep;

/* Synth-side values kept with a pattern. Transport uses bpm and swing. */
typedef struct PatternSettings {
    uint16_t bpm;
    uint8_t waveform;
    uint8_t cutoff;
    uint8_t resonance;
    uint8_t envelope;
    uint8_t decay;
    uint8_t drive;
    uint8_t delay_time;
    uint8_t delay_feedback;
    uint8_t delay_mix;
    uint8_t swing;         /* 50..75 percent. */
    int8_t tune_cents;     /* -50..50 cents. */
    uint8_t accent_depth;
    uint8_t slide_time;
    uint8_t key_tracking;
    uint8_t attack;
} PatternSettings;

typedef struct Pattern {
    PatternStep steps[PSP303_MAX_STEPS];
    uint8_t length;             /* 3..16 steps. */
    PatternDirection direction;
    int8_t transpose;           /* -24..24 semitones. */
    PatternSettings settings;
} Pattern;

void pattern_init(Pattern *pattern);
void pattern_clamp(Pattern *pattern);

/* Return the next index using an explicit ping-pong direction (+1 or -1). */
int pattern_next_index_with_direction(
    const Pattern *pattern,
    int current,
    int *pingpong_direction,
    uint32_t *rng_state);

/* Compatibility helper for callers that do not retain ping-pong direction. */
int pattern_next_index(const Pattern *pattern, int current, uint32_t *rng_state);
int pattern_previous_index(const Pattern *pattern, int current);

#endif
