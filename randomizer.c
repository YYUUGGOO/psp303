#include "randomizer.h"

#include <stddef.h>

static const uint8_t SCALE_CHROMATIC[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t SCALE_MAJOR[] = {0, 2, 4, 5, 7, 9, 11};
static const uint8_t SCALE_MINOR[] = {0, 2, 3, 5, 7, 8, 10};
static const uint8_t SCALE_DORIAN[] = {0, 2, 3, 5, 7, 9, 10};
static const uint8_t SCALE_PHRYGIAN[] = {0, 1, 3, 5, 7, 8, 10};
static const uint8_t SCALE_MAJOR_PENTA[] = {0, 2, 4, 7, 9};
static const uint8_t SCALE_MINOR_PENTA[] = {0, 3, 5, 7, 10};

static const uint8_t *scale_values(RandomScale scale, unsigned int *count)
{
    switch (scale) {
        case RANDOM_SCALE_MAJOR: *count = 7U; return SCALE_MAJOR;
        case RANDOM_SCALE_NATURAL_MINOR: *count = 7U; return SCALE_MINOR;
        case RANDOM_SCALE_DORIAN: *count = 7U; return SCALE_DORIAN;
        case RANDOM_SCALE_PHRYGIAN: *count = 7U; return SCALE_PHRYGIAN;
        case RANDOM_SCALE_MAJOR_PENTATONIC: *count = 5U; return SCALE_MAJOR_PENTA;
        case RANDOM_SCALE_MINOR_PENTATONIC: *count = 5U; return SCALE_MINOR_PENTA;
        case RANDOM_SCALE_CHROMATIC:
        default: *count = 12U; return SCALE_CHROMATIC;
    }
}

static uint8_t percent(uint32_t value)
{
    return (uint8_t)(value % 100U);
}

void randomizer_defaults(RandomizerSettings *settings)
{
    if (settings == NULL) return;
    settings->root = 0U;
    settings->scale = RANDOM_SCALE_NATURAL_MINOR;
    settings->density = 78U;
    settings->accent_chance = 24U;
    settings->slide_chance = 18U;
    settings->octave_range = 2U;
    settings->probability = 100U;
    settings->probability_variation = 0U;
    settings->ratchet_chance = 5U;
    settings->gate = 75U;
}

void randomizer_clamp(RandomizerSettings *settings)
{
    if (settings == NULL) return;
    if (settings->root > 11U) settings->root = 11U;
    if (settings->scale < RANDOM_SCALE_CHROMATIC || settings->scale >= RANDOM_SCALE_COUNT) {
        settings->scale = RANDOM_SCALE_CHROMATIC;
    }
    if (settings->octave_range < 1U) settings->octave_range = 1U;
    if (settings->octave_range > 8U) settings->octave_range = 8U;
    if (settings->density > 100U) settings->density = 100U;
    if (settings->accent_chance > 100U) settings->accent_chance = 100U;
    if (settings->slide_chance > 100U) settings->slide_chance = 100U;
    if (settings->probability > 100U) settings->probability = 100U;
    if (settings->probability_variation > 100U) settings->probability_variation = 100U;
    if (settings->ratchet_chance > 100U) settings->ratchet_chance = 100U;
    if (settings->gate > 100U) settings->gate = 100U;
}

uint32_t randomizer_next(uint32_t *state)
{
    uint32_t value = state == NULL ? UINT32_C(0x303303) : *state;

    if (value == 0U) value = UINT32_C(0x303303);
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    if (state != NULL) *state = value;
    return value;
}

int randomizer_note_in_scale(uint8_t note, uint8_t root, RandomScale scale)
{
    unsigned int i;
    unsigned int count;
    const uint8_t *values = scale_values(scale, &count);
    uint8_t pitch = (uint8_t)((note + 120U - (root % 12U)) % 12U);

    for (i = 0U; i < count; ++i) {
        if (values[i] == pitch) return 1;
    }
    return 0;
}

static uint8_t generated_note(const RandomizerSettings *settings, uint32_t value)
{
    unsigned int count;
    const uint8_t *values = scale_values(settings->scale, &count);
    unsigned int octave = (value >> 16) % settings->octave_range;
    unsigned int degree = (value >> 8) % count;
    int note = 36 + (int)settings->root + (int)values[degree] + (int)octave * 12;

    while (note > 108) note -= 12;
    while (note < 24) note += 12;
    return (uint8_t)note;
}

static uint8_t generated_probability(const RandomizerSettings *settings, uint32_t value)
{
    unsigned int reduction = settings->probability_variation == 0U
        ? 0U
        : (unsigned int)((value >> 20) % (settings->probability_variation + 1U));
    unsigned int probability = settings->probability > reduction
        ? settings->probability - reduction
        : 0U;

    return (uint8_t)probability;
}

void randomizer_generate(
    Pattern *pattern,
    const RandomizerSettings *settings,
    uint32_t *state)
{
    RandomizerSettings local;
    unsigned int i;

    if (pattern == NULL) return;
    if (settings == NULL) {
        randomizer_defaults(&local);
        settings = &local;
    } else {
        local = *settings;
        randomizer_clamp(&local);
        settings = &local;
    }
    pattern_clamp(pattern);
    for (i = 0U; i < (unsigned int)pattern->length; ++i) {
        uint32_t value = randomizer_next(state);
        PatternStep *step = &pattern->steps[i];

        step->active = (uint8_t)(percent(value) < settings->density);
        step->note = generated_note(settings, value);
        step->accent = (uint8_t)(step->active != 0U
            && percent(value >> 16) < settings->accent_chance);
        step->slide = (uint8_t)(step->active != 0U
            && percent(value >> 24) < settings->slide_chance);
        step->probability = generated_probability(settings, value);
        step->ratchet_count = (uint8_t)(percent(value >> 27) < settings->ratchet_chance
            ? 2U + ((value >> 29) & 1U)
            : 1U);
        step->gate = (uint8_t)(settings->gate == 0U
            ? 0U
            : settings->gate - (uint8_t)((value >> 5) % (settings->gate + 1U)) / 4U);
    }

    /* A generated pattern always has a usable first downbeat. */
    pattern->steps[0].active = 1U;
    pattern->steps[0].note = generated_note(settings, 0U);
    pattern->steps[0].probability = 100U;
    pattern_clamp(pattern);
}

void randomizer_mutate(
    Pattern *pattern,
    const RandomizerSettings *settings,
    uint8_t amount,
    uint32_t *state)
{
    RandomizerSettings local;
    unsigned int i;

    if (pattern == NULL) return;
    if (settings == NULL) {
        randomizer_defaults(&local);
        settings = &local;
    } else {
        local = *settings;
        randomizer_clamp(&local);
        settings = &local;
    }
    if (amount > 100U) amount = 100U;
    pattern_clamp(pattern);

    for (i = 0U; i < (unsigned int)pattern->length; ++i) {
        PatternStep *step = &pattern->steps[i];
        uint32_t value = randomizer_next(state);
        unsigned int field;

        if (percent(value) >= amount) continue;
        field = (unsigned int)((value >> 8) % 7U);
        switch (field) {
            case 0U: step->note = generated_note(settings, value); break;
            case 1U: step->active = (uint8_t)!step->active; break;
            case 2U: step->accent = (uint8_t)!step->accent; break;
            case 3U: step->slide = (uint8_t)!step->slide; break;
            case 4U: step->probability = generated_probability(settings, value); break;
            case 5U: step->ratchet_count = (uint8_t)(1U + ((value >> 20) % 4U)); break;
            default:
                step->gate = (uint8_t)((value >> 24) % 101U);
                break;
        }
        /* A selected mutation never leaves its note outside the requested
           root/scale, even when the selected field was not pitch. */
        if (!randomizer_note_in_scale(step->note, settings->root, settings->scale)) {
            step->note = generated_note(settings, value);
        }
    }
    pattern_clamp(pattern);
}
