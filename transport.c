#include "transport.h"

#include <string.h>

static uint32_t transport_random(uint32_t *state)
{
    uint32_t value = state == NULL ? UINT32_C(0x303303) : *state;

    if (value == 0U) value = UINT32_C(0x303303);
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    if (state != NULL) *state = value;
    return value;
}

static uint16_t clamp_bpm(uint16_t bpm)
{
    if (bpm < TRANSPORT_MIN_BPM) return TRANSPORT_MIN_BPM;
    if (bpm > TRANSPORT_MAX_BPM) return TRANSPORT_MAX_BPM;
    return bpm;
}

static uint32_t clamp_sample_rate(uint32_t sample_rate)
{
    if (sample_rate < TRANSPORT_MIN_SAMPLE_RATE) return TRANSPORT_MIN_SAMPLE_RATE;
    if (sample_rate > TRANSPORT_MAX_SAMPLE_RATE) return TRANSPORT_MAX_SAMPLE_RATE;
    return sample_rate;
}

static uint16_t clamp_ppqn(uint16_t ppqn)
{
    if (ppqn < TRANSPORT_MIN_PPQN) return TRANSPORT_MIN_PPQN;
    if (ppqn > TRANSPORT_MAX_PPQN) return TRANSPORT_MAX_PPQN;
    return ppqn;
}

static void update_increment(Transport *transport)
{
    uint64_t numerator;
    uint64_t denominator;

    numerator = (uint64_t)transport->bpm * (uint64_t)transport->ppqn
        * TRANSPORT_Q32_ONE;
    denominator = UINT64_C(60) * (uint64_t)transport->sample_rate;
    transport->phase_increment_q32 = denominator == 0U ? 0U : numerator / denominator;
}

static uint8_t pattern_swing(const Transport *transport)
{
    uint8_t swing = transport->swing;

    if (swing < 50U) swing = 50U;
    if (swing > 75U) swing = 75U;
    return swing;
}

static uint64_t base_step_duration(const Transport *transport)
{
    return ((uint64_t)transport->ppqn * TRANSPORT_Q32_ONE) / 4U;
}

static uint64_t step_duration(const Transport *transport, int index)
{
    uint64_t pair_duration = base_step_duration(transport) * 2U;
    uint64_t long_duration = pair_duration * (uint64_t)pattern_swing(transport) / 100U;
    uint64_t duration = (index & 1) == 0 ? long_duration : pair_duration - long_duration;

    return duration == 0U ? 1U : duration;
}

static uint8_t step_ratchets(const PatternStep *step)
{
    if (step->ratchet_count < 1U) return 1U;
    if (step->ratchet_count > 4U) return 4U;
    return step->ratchet_count;
}

static uint8_t effective_note(const Pattern *pattern, const PatternStep *step)
{
    int note = (int)step->note + (int)pattern->transpose;

    if (note < (int)PSP303_MIN_NOTE) note = (int)PSP303_MIN_NOTE;
    if (note > (int)PSP303_MAX_NOTE) note = (int)PSP303_MAX_NOTE;
    return (uint8_t)note;
}

static int enqueue_event(Transport *transport, const TransportEvent *event)
{
    uint8_t index;

    if (transport->event_count >= TRANSPORT_MAX_EVENTS) {
        ++transport->dropped_events;
        return 0;
    }
    index = (uint8_t)((transport->event_head + transport->event_count)
        % TRANSPORT_MAX_EVENTS);
    transport->events[index] = *event;
    ++transport->event_count;
    return 1;
}

static int enqueue_step_event(
    Transport *transport,
    const Pattern *pattern,
    int index,
    const PatternStep *step)
{
    TransportEvent event;

    memset(&event, 0, sizeof(event));
    event.type = TRANSPORT_EVENT_STEP;
    event.step_index = (uint8_t)index;
    event.note = effective_note(pattern, step);
    event.active = step->active;
    event.accent = step->accent;
    event.slide = step->slide;
    event.ratchet_count = step_ratchets(step);
    event.midi_tick = transport->midi_tick_total;
    event.time_q32 = transport->timeline_q32;
    return enqueue_event(transport, &event);
}

static int enqueue_note_event(
    Transport *transport,
    const Pattern *pattern,
    int index,
    const PatternStep *step,
    TransportEventType type,
    uint8_t ratchet_index,
    uint64_t time_q32)
{
    TransportEvent event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.step_index = (uint8_t)index;
    event.note = effective_note(pattern, step);
    event.velocity = step->accent != 0U ? 127U : 100U;
    event.active = step->active;
    event.accent = step->accent;
    event.slide = step->slide;
    event.ratchet_index = ratchet_index;
    event.ratchet_count = step_ratchets(step);
    event.midi_tick = (uint32_t)(time_q32 >> 32);
    event.time_q32 = time_q32;
    return enqueue_event(transport, &event);
}

static void schedule_step(Transport *transport, const Pattern *pattern)
{
    const PatternStep *step;
    uint8_t ratchets;
    uint8_t probability;
    uint64_t duration;
    uint64_t subdivision;
    unsigned int ratchet;

    if (pattern == NULL) return;
    step = &pattern->steps[transport->current_index];
    ratchets = step_ratchets(step);
    duration = step_duration(transport, transport->current_index);
    subdivision = duration / (uint64_t)ratchets;
    if (subdivision == 0U) subdivision = 1U;
    probability = step->probability > 100U ? 100U : step->probability;
    transport->next_step_time_q32 = transport->timeline_q32 + duration;
    transport->midi_tick_accumulator = 0U;
    transport->has_step = 1U;
    (void)enqueue_step_event(transport, pattern, transport->current_index, step);

    if (step->active == 0U || (transport_random(&transport->rng_state) % 100U) >= probability) {
        return;
    }
    for (ratchet = 0U; ratchet < (unsigned int)ratchets; ++ratchet) {
        uint64_t on_time = transport->timeline_q32 + subdivision * ratchet;
        uint64_t gate_time = subdivision * (uint64_t)step->gate / 100U;
        uint64_t off_time = on_time + gate_time;

        (void)enqueue_note_event(
            transport, pattern, transport->current_index, step,
            TRANSPORT_EVENT_NOTE_ON, (uint8_t)ratchet, on_time);
        (void)enqueue_note_event(
            transport, pattern, transport->current_index, step,
            TRANSPORT_EVENT_NOTE_OFF, (uint8_t)ratchet, off_time);
    }
}

static void update_whole_tick_accumulator(Transport *transport)
{
    uint64_t whole_tick = transport->timeline_q32 >> 32;

    if (whole_tick > transport->last_whole_tick) {
        uint64_t delta = whole_tick - transport->last_whole_tick;
        uint64_t total = (uint64_t)transport->midi_tick_accumulator + delta;
        transport->midi_tick_accumulator = total > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)total;
        transport->midi_tick_total += delta > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)delta;
        transport->last_whole_tick = whole_tick;
    }
}

static void service_step_boundaries(Transport *transport, const Pattern *pattern)
{
    unsigned int guard = 0U;

    if (pattern != NULL) transport->pattern = pattern;
    pattern = transport->pattern;
    if (pattern == NULL || transport->has_step == 0U) return;

    while (transport->timeline_q32 >= transport->next_step_time_q32
           && guard++ < PSP303_MAX_STEPS) {
        transport->current_index = transport_next_index(transport, pattern);
        schedule_step(transport, pattern);
    }
}

void transport_defaults(TransportConfig *config)
{
    if (config == NULL) return;
    config->source = TRANSPORT_SOURCE_INTERNAL;
    config->bpm = TRANSPORT_DEFAULT_BPM;
    config->sample_rate = TRANSPORT_DEFAULT_SAMPLE_RATE;
    config->ppqn = TRANSPORT_DEFAULT_PPQN;
    config->swing = 50U;
    config->rng_seed = UINT32_C(0x303303);
}

void transport_init(Transport *transport, const TransportConfig *config)
{
    TransportConfig local;

    if (transport == NULL) return;
    if (config == NULL) {
        transport_defaults(&local);
        config = &local;
    }
    memset(transport, 0, sizeof(*transport));
    transport->source = config->source;
    transport->bpm = config->bpm == 0U ? TRANSPORT_DEFAULT_BPM : config->bpm;
    transport->sample_rate = config->sample_rate == 0U
        ? TRANSPORT_DEFAULT_SAMPLE_RATE
        : config->sample_rate;
    transport->ppqn = config->ppqn == 0U ? TRANSPORT_DEFAULT_PPQN : config->ppqn;
    transport->swing = config->swing == 0U ? 50U : config->swing;
    transport->rng_state = config->rng_seed;
    transport->pingpong_direction = 1;
    transport->current_index = -1;
    transport_clamp(transport);
}

void transport_clamp(Transport *transport)
{
    if (transport == NULL) return;
    if (transport->source != TRANSPORT_SOURCE_INTERNAL
        && transport->source != TRANSPORT_SOURCE_MIDI) {
        transport->source = TRANSPORT_SOURCE_INTERNAL;
    }
    transport->bpm = clamp_bpm(transport->bpm);
    transport->sample_rate = clamp_sample_rate(transport->sample_rate);
    transport->ppqn = clamp_ppqn(transport->ppqn);
    if (transport->swing < 50U) transport->swing = 50U;
    if (transport->swing > 75U) transport->swing = 75U;
    if (transport->pingpong_direction < 0) transport->pingpong_direction = -1;
    else transport->pingpong_direction = 1;
    if (transport->rng_state == 0U) transport->rng_state = UINT32_C(0x303303);
    update_increment(transport);
    transport->phase_q32 = transport->timeline_q32 & (TRANSPORT_Q32_ONE - 1U);
}

void transport_set_source(Transport *transport, TransportSource source)
{
    if (transport == NULL) return;
    transport->source = source;
    transport_clamp(transport);
}

void transport_set_bpm(Transport *transport, uint16_t bpm)
{
    if (transport == NULL) return;
    transport->bpm = clamp_bpm(bpm);
    update_increment(transport);
}

void transport_set_sample_rate(Transport *transport, uint32_t sample_rate)
{
    if (transport == NULL) return;
    transport->sample_rate = clamp_sample_rate(sample_rate);
    update_increment(transport);
}

void transport_set_ppqn(Transport *transport, uint16_t ppqn)
{
    if (transport == NULL) return;
    transport->ppqn = clamp_ppqn(ppqn);
    update_increment(transport);
}

void transport_set_swing(Transport *transport, uint8_t swing)
{
    if (transport == NULL) return;
    transport->swing = swing < 50U ? 50U : (swing > 75U ? 75U : swing);
}

void transport_set_pattern(Transport *transport, const Pattern *pattern)
{
    if (transport == NULL) return;
    transport->pattern = pattern;
}

void transport_start(Transport *transport, const Pattern *pattern, uint8_t reset)
{
    if (transport == NULL) return;
    if (pattern != NULL) transport->pattern = pattern;
    transport->running = 1U;
    if (reset != 0U) {
        transport->phase_q32 = 0U;
        transport->timeline_q32 = 0U;
        transport->next_step_time_q32 = 0U;
        transport->last_whole_tick = 0U;
        transport->midi_tick_accumulator = 0U;
        transport->midi_tick_total = 0U;
        transport->current_index = -1;
        transport->pingpong_direction = 1;
        transport->has_step = 0U;
        transport->event_head = 0U;
        transport->event_count = 0U;
        if (transport->pattern != NULL) {
            transport->current_index = transport_next_index(transport, transport->pattern);
            schedule_step(transport, transport->pattern);
        }
    } else if (transport->pattern != NULL && transport->has_step == 0U) {
        transport->current_index = transport_next_index(transport, transport->pattern);
        schedule_step(transport, transport->pattern);
    }
}

void transport_stop(Transport *transport)
{
    if (transport == NULL) return;
    transport->running = 0U;
}

void transport_continue(Transport *transport, const Pattern *pattern)
{
    if (transport == NULL) return;
    if (pattern != NULL) transport->pattern = pattern;
    transport->running = 1U;
}

void transport_midi_start(Transport *transport, const Pattern *pattern)
{
    transport_start(transport, pattern, 1U);
}

void transport_midi_stop(Transport *transport)
{
    transport_stop(transport);
}

void transport_midi_continue(Transport *transport, const Pattern *pattern)
{
    transport_continue(transport, pattern);
}

int transport_next_index(Transport *transport, const Pattern *pattern)
{
    const Pattern *selected;
    int pingpong_direction;
    int index;

    if (transport == NULL) return 0;
    selected = pattern != NULL ? pattern : transport->pattern;
    if (selected == NULL) return 0;
    transport->pattern = selected;
    pingpong_direction = transport->pingpong_direction;
    index = pattern_next_index_with_direction(
        selected,
        transport->current_index,
        &pingpong_direction,
        &transport->rng_state);
    transport->pingpong_direction = (int8_t)pingpong_direction;
    return index;
}

uint32_t transport_process_sample(Transport *transport, const Pattern *pattern)
{
    uint8_t before;

    if (transport == NULL || transport->running == 0U
        || transport->source != TRANSPORT_SOURCE_INTERNAL) return 0U;
    if (pattern != NULL) transport->pattern = pattern;
    before = transport->event_count;
    transport->timeline_q32 += transport->phase_increment_q32;
    transport->phase_q32 = transport->timeline_q32 & (TRANSPORT_Q32_ONE - 1U);
    update_whole_tick_accumulator(transport);
    service_step_boundaries(transport, pattern);
    return (uint32_t)((transport->event_count + TRANSPORT_MAX_EVENTS - before)
        % TRANSPORT_MAX_EVENTS);
}

uint32_t transport_advance_samples(
    Transport *transport,
    const Pattern *pattern,
    uint32_t sample_count)
{
    uint32_t i;
    uint32_t generated = 0U;

    for (i = 0U; i < sample_count; ++i) {
        generated += transport_process_sample(transport, pattern);
    }
    return generated;
}

uint32_t transport_midi_clock_tick(Transport *transport, const Pattern *pattern)
{
    uint8_t before;

    if (transport == NULL || transport->running == 0U
        || transport->source != TRANSPORT_SOURCE_MIDI) return 0U;
    if (pattern != NULL) transport->pattern = pattern;
    before = transport->event_count;
    transport->timeline_q32 += TRANSPORT_Q32_ONE;
    transport->phase_q32 = transport->timeline_q32 & (TRANSPORT_Q32_ONE - 1U);
    update_whole_tick_accumulator(transport);
    service_step_boundaries(transport, pattern);
    return (uint32_t)((transport->event_count + TRANSPORT_MAX_EVENTS - before)
        % TRANSPORT_MAX_EVENTS);
}

uint32_t transport_tick(Transport *transport, const Pattern *pattern)
{
    return transport_midi_clock_tick(transport, pattern);
}

int transport_poll_event(Transport *transport, TransportEvent *event)
{
    TransportEvent *queued;

    if (transport == NULL || event == NULL || transport->event_count == 0U) return 0;
    queued = &transport->events[transport->event_head];
    if (queued->time_q32 > transport->timeline_q32) return 0;
    *event = *queued;
    transport->event_head = (uint8_t)((transport->event_head + 1U) % TRANSPORT_MAX_EVENTS);
    --transport->event_count;
    return 1;
}

uint32_t transport_pending_events(const Transport *transport)
{
    return transport == NULL ? 0U : transport->event_count;
}

uint32_t transport_dropped_events(const Transport *transport)
{
    return transport == NULL ? 0U : transport->dropped_events;
}
