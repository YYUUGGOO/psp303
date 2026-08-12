#include "pattern.h"

#include <string.h>

static uint32_t pattern_random(uint32_t *state)
{
    uint32_t value = state == NULL ? UINT32_C(0x303303) : *state;

    if (value == 0U) value = UINT32_C(0x303303);
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    if (state != NULL) *state = value;
    return value;
}

static int pattern_length(const Pattern *pattern)
{
    int length = pattern == NULL ? (int)PSP303_MIN_STEPS : (int)pattern->length;

    if (length < (int)PSP303_MIN_STEPS) length = (int)PSP303_MIN_STEPS;
    if (length > (int)PSP303_MAX_STEPS) length = (int)PSP303_MAX_STEPS;
    return length;
}

static int wrap_index(int index, int length)
{
    while (index < 0) index += length;
    while (index >= length) index -= length;
    return index;
}

void pattern_init(Pattern *pattern)
{
    static const uint8_t notes[PSP303_MAX_STEPS] = {
        36, 36, 48, 36, 43, 36, 39, 36,
        36, 46, 36, 43, 39, 36, 34, 36
    };
    static const uint8_t active[PSP303_MAX_STEPS] = {
        1, 0, 1, 1, 1, 0, 1, 1,
        1, 1, 0, 1, 1, 0, 1, 1
    };
    unsigned int i;

    if (pattern == NULL) return;
    memset(pattern, 0, sizeof(*pattern));
    pattern->length = PSP303_MAX_STEPS;
    pattern->direction = PATTERN_FORWARD;
    pattern->transpose = 0;
    pattern->settings.bpm = 128U;
    pattern->settings.cutoff = 34U;
    pattern->settings.resonance = 72U;
    pattern->settings.envelope = 72U;
    pattern->settings.decay = 48U;
    pattern->settings.drive = 28U;
    pattern->settings.delay_time = 50U;
    pattern->settings.delay_feedback = 22U;
    pattern->settings.delay_mix = 22U;
    pattern->settings.swing = 50U;
    pattern->settings.accent_depth = 45U;
    pattern->settings.slide_time = 50U;
    pattern->settings.attack = 4U;

    for (i = 0U; i < PSP303_MAX_STEPS; ++i) {
        pattern->steps[i].note = notes[i];
        pattern->steps[i].active = active[i];
        pattern->steps[i].accent = (uint8_t)(i == 0U || i == 8U || i == 12U);
        pattern->steps[i].slide = (uint8_t)(i == 2U || i == 9U || i == 14U);
        pattern->steps[i].probability = 100U;
        pattern->steps[i].ratchet_count = 1U;
        pattern->steps[i].gate = 75U;
    }
}

void pattern_clamp(Pattern *pattern)
{
    unsigned int i;

    if (pattern == NULL) return;
    if (pattern->length < PSP303_MIN_STEPS) pattern->length = PSP303_MIN_STEPS;
    if (pattern->length > PSP303_MAX_STEPS) pattern->length = PSP303_MAX_STEPS;
    if (pattern->direction < PATTERN_FORWARD ||
        pattern->direction > PATTERN_RANDOM) {
        pattern->direction = PATTERN_FORWARD;
    }
    if (pattern->transpose < PSP303_MIN_TRANSPOSE) {
        pattern->transpose = PSP303_MIN_TRANSPOSE;
    }
    if (pattern->transpose > PSP303_MAX_TRANSPOSE) {
        pattern->transpose = PSP303_MAX_TRANSPOSE;
    }

    if (pattern->settings.bpm < 40U) pattern->settings.bpm = 40U;
    if (pattern->settings.bpm > 300U) pattern->settings.bpm = 300U;
    if (pattern->settings.waveform > 1U) pattern->settings.waveform = 1U;
    if (pattern->settings.cutoff > 100U) pattern->settings.cutoff = 100U;
    if (pattern->settings.resonance > 100U) pattern->settings.resonance = 100U;
    if (pattern->settings.envelope > 100U) pattern->settings.envelope = 100U;
    if (pattern->settings.decay > 100U) pattern->settings.decay = 100U;
    if (pattern->settings.drive > 100U) pattern->settings.drive = 100U;
    if (pattern->settings.delay_time > 100U) pattern->settings.delay_time = 100U;
    if (pattern->settings.delay_feedback > 100U) pattern->settings.delay_feedback = 100U;
    if (pattern->settings.delay_mix > 100U) pattern->settings.delay_mix = 100U;
    if (pattern->settings.swing < 50U) pattern->settings.swing = 50U;
    if (pattern->settings.swing > 75U) pattern->settings.swing = 75U;
    if (pattern->settings.tune_cents < -50) pattern->settings.tune_cents = -50;
    if (pattern->settings.tune_cents > 50) pattern->settings.tune_cents = 50;
    if (pattern->settings.accent_depth > 100U) pattern->settings.accent_depth = 100U;
    if (pattern->settings.slide_time > 100U) pattern->settings.slide_time = 100U;
    if (pattern->settings.key_tracking > 100U) pattern->settings.key_tracking = 100U;
    if (pattern->settings.attack > 100U) pattern->settings.attack = 100U;

    for (i = 0U; i < PSP303_MAX_STEPS; ++i) {
        PatternStep *step = &pattern->steps[i];
        if (step->note > PSP303_MAX_NOTE) step->note = PSP303_MAX_NOTE;
        if (step->active > 1U) step->active = 1U;
        if (step->accent > 1U) step->accent = 1U;
        if (step->slide > 1U) step->slide = 1U;
        if (step->probability > 100U) step->probability = 100U;
        if (step->ratchet_count < 1U) step->ratchet_count = 1U;
        if (step->ratchet_count > 4U) step->ratchet_count = 4U;
        if (step->gate > 100U) step->gate = 100U;
    }
}

int pattern_next_index_with_direction(
    const Pattern *pattern,
    int current,
    int *pingpong_direction,
    uint32_t *rng_state)
{
    int length = pattern_length(pattern);
    int direction = pingpong_direction == NULL ? 1 : *pingpong_direction;

    if (direction < 0) direction = -1;
    else direction = 1;
    if (current < 0 || current >= length) {
        if (pattern != NULL && pattern->direction == PATTERN_REVERSE) {
            current = length;
        } else {
            current = -1;
        }
    }

    switch (pattern == NULL ? PATTERN_FORWARD : pattern->direction) {
        case PATTERN_REVERSE:
            current = current <= 0 ? length - 1 : current - 1;
            break;
        case PATTERN_RANDOM:
            current = (int)(pattern_random(rng_state) % (uint32_t)length);
            break;
        case PATTERN_PING_PONG:
            if (current < 0) {
                current = 0;
                direction = 1;
            } else {
                current += direction;
                if (current >= length) {
                    direction = -1;
                    current = length - 2;
                } else if (current < 0) {
                    direction = 1;
                    current = 1;
                }
            }
            break;
        case PATTERN_FORWARD:
        default:
            current = current < 0 ? 0 : (current + 1) % length;
            break;
    }
    if (pingpong_direction != NULL) *pingpong_direction = direction;
    return wrap_index(current, length);
}

int pattern_next_index(const Pattern *pattern, int current, uint32_t *rng_state)
{
    int pingpong_direction = 1;
    return pattern_next_index_with_direction(
        pattern, current, &pingpong_direction, rng_state);
}

int pattern_previous_index(const Pattern *pattern, int current)
{
    int length = pattern_length(pattern);

    if (current <= 0 || current >= length) return length - 1;
    return current - 1;
}
