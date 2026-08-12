#include "randomizer.h"

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

static void test_deterministic_rng_and_clamps(void)
{
    RandomizerSettings first;
    uint32_t first_state = 0U;
    uint32_t second_state = 0U;

    randomizer_defaults(&first);
    CHECK(randomizer_next(&first_state) == randomizer_next(&second_state));
    CHECK(first_state != 0U);
    CHECK(second_state != 0U);
    first.root = 255U;
    first.scale = RANDOM_SCALE_COUNT;
    first.octave_range = 0U;
    first.density = 255U;
    first.accent_chance = 255U;
    first.slide_chance = 255U;
    first.probability = 255U;
    first.probability_variation = 255U;
    first.ratchet_chance = 255U;
    first.gate = 255U;
    randomizer_clamp(&first);
    CHECK(first.root == 11U);
    CHECK(first.scale == RANDOM_SCALE_CHROMATIC);
    CHECK(first.octave_range == 1U);
    CHECK(first.density == 100U && first.accent_chance == 100U);
    CHECK(first.slide_chance == 100U && first.probability == 100U);
    CHECK(first.probability_variation == 100U);
    CHECK(first.ratchet_chance == 100U && first.gate == 100U);
}

static void test_scales(void)
{
    static const struct {
        RandomScale scale;
        uint8_t in_note;
    } cases[] = {
        {RANDOM_SCALE_CHROMATIC, 61U},
        {RANDOM_SCALE_MAJOR, 62U},
        {RANDOM_SCALE_NATURAL_MINOR, 63U},
        {RANDOM_SCALE_DORIAN, 69U},
        {RANDOM_SCALE_PHRYGIAN, 61U},
        {RANDOM_SCALE_MAJOR_PENTATONIC, 67U},
        {RANDOM_SCALE_MINOR_PENTATONIC, 63U}
    };
    size_t i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(randomizer_note_in_scale(cases[i].in_note, 60U, cases[i].scale) != 0);
    }
    CHECK(randomizer_note_in_scale(61U, 60U, RANDOM_SCALE_MAJOR) == 0);
    CHECK(randomizer_note_in_scale(73U, 61U, RANDOM_SCALE_MAJOR) != 0);
}

static void test_generation_edges_and_determinism(void)
{
    Pattern off;
    Pattern on;
    Pattern repeat;
    RandomizerSettings settings;
    uint32_t state = 12345U;
    uint32_t same_state = 12345U;
    unsigned int i;

    pattern_init(&off);
    off.length = 8U;
    randomizer_defaults(&settings);
    settings.density = 0U;
    settings.accent_chance = 100U;
    settings.slide_chance = 100U;
    settings.probability = 0U;
    settings.ratchet_chance = 0U;
    settings.gate = 0U;
    randomizer_generate(&off, &settings, &state);
    CHECK(off.length == 8U);
    CHECK(off.steps[0].active == 1U);
    CHECK(off.steps[0].probability == 100U);
    for (i = 1U; i < off.length; ++i) {
        CHECK(off.steps[i].active == 0U);
        CHECK(off.steps[i].accent == 0U && off.steps[i].slide == 0U);
        CHECK(off.steps[i].probability == 0U && off.steps[i].gate == 0U);
        CHECK(randomizer_note_in_scale(off.steps[i].note, settings.root, settings.scale));
    }

    pattern_init(&on);
    on.length = 8U;
    settings.density = 100U;
    settings.accent_chance = 100U;
    settings.slide_chance = 100U;
    settings.probability = 100U;
    settings.probability_variation = 0U;
    settings.ratchet_chance = 100U;
    settings.gate = 100U;
    state = 12345U;
    randomizer_generate(&on, &settings, &state);
    pattern_init(&repeat);
    repeat.length = 8U;
    randomizer_generate(&repeat, &settings, &same_state);
    CHECK(memcmp(&on, &repeat, sizeof(on)) == 0);
    for (i = 0U; i < on.length; ++i) {
        CHECK(on.steps[i].active == 1U);
        CHECK(on.steps[i].accent == 1U && on.steps[i].slide == 1U);
        CHECK(on.steps[i].ratchet_count >= 1U && on.steps[i].ratchet_count <= 4U);
        CHECK(on.steps[i].gate <= 100U);
        CHECK(randomizer_note_in_scale(on.steps[i].note, settings.root, settings.scale));
    }
}

static void test_mutation_edges(void)
{
    Pattern pattern;
    Pattern before;
    RandomizerSettings settings;
    uint32_t state = 7U;
    unsigned int i;

    pattern_init(&pattern);
    before = pattern;
    randomizer_defaults(&settings);
    randomizer_mutate(&pattern, &settings, 0U, &state);
    CHECK(memcmp(&pattern, &before, sizeof(pattern)) == 0);

    randomizer_mutate(&pattern, &settings, 100U, &state);
    for (i = 0U; i < pattern.length; ++i) {
        CHECK(randomizer_note_in_scale(pattern.steps[i].note, settings.root, settings.scale));
        CHECK(pattern.steps[i].probability <= 100U);
        CHECK(pattern.steps[i].ratchet_count >= 1U && pattern.steps[i].ratchet_count <= 4U);
        CHECK(pattern.steps[i].gate <= 100U);
    }
}

int main(void)
{
    test_deterministic_rng_and_clamps();
    test_scales();
    test_generation_edges_and_determinism();
    test_mutation_edges();
    if (failures != 0U) {
        fprintf(stderr, "%u randomizer test(s) failed\n", failures);
        return 1;
    }
    puts("randomizer tests passed");
    return 0;
}
