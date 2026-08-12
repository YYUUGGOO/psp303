#ifndef PSP303_RANDOMIZER_H
#define PSP303_RANDOMIZER_H

#include <stdint.h>

#include "pattern.h"

typedef enum RandomScale {
    RANDOM_SCALE_CHROMATIC = 0,
    RANDOM_SCALE_MAJOR,
    RANDOM_SCALE_NATURAL_MINOR,
    RANDOM_SCALE_DORIAN,
    RANDOM_SCALE_PHRYGIAN,
    RANDOM_SCALE_MAJOR_PENTATONIC,
    RANDOM_SCALE_MINOR_PENTATONIC,
    RANDOM_SCALE_COUNT
} RandomScale;

typedef struct RandomizerSettings {
    uint8_t root;                 /* 0..11, C through B. */
    RandomScale scale;
    uint8_t density;              /* Active-step chance, 0..100 percent. */
    uint8_t accent_chance;        /* Accent chance on active steps. */
    uint8_t slide_chance;         /* Slide chance on active steps. */
    uint8_t octave_range;         /* Number of available octaves, 1..8. */
    uint8_t probability;          /* Base trigger probability, 0..100. */
    uint8_t probability_variation;/* Random reduction from base probability. */
    uint8_t ratchet_chance;       /* Chance of a 2..4-way ratchet. */
    uint8_t gate;                 /* Base gate percentage, 0..100. */
} RandomizerSettings;

void randomizer_defaults(RandomizerSettings *settings);
void randomizer_clamp(RandomizerSettings *settings);
uint32_t randomizer_next(uint32_t *state);
void randomizer_generate(
    Pattern *pattern,
    const RandomizerSettings *settings,
    uint32_t *state);
void randomizer_mutate(
    Pattern *pattern,
    const RandomizerSettings *settings,
    uint8_t amount,
    uint32_t *state);
int randomizer_note_in_scale(uint8_t note, uint8_t root, RandomScale scale);

#endif
