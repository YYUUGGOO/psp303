#include <pspaudio.h>
#include <pspaudiolib.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <pspsync.h>
#include <psp_usb_midi.h>

#include "midi_output.h"
#include "midi.h"
#include "pattern.h"
#include "randomizer.h"
#include "storage.h"
#include "transport.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP-303", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define SAMPLE_RATE 44100.0f
#define SEQUENCE_LENGTH 16
#define AUDIO_CHANNEL 0
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272
#define FRAME_STRIDE 512
#define FRAMEBUFFER_BYTES (FRAME_STRIDE * SCREEN_HEIGHT * (int)sizeof(uint32_t))
#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

#define COLOR_BLACK RGB(0, 0, 0)
#define COLOR_WHITE RGB(255, 255, 255)

typedef PatternStep Step;

typedef struct {
    int16_t left;
    int16_t right;
} StereoSample;

typedef enum {
    PARAM_BPM,
    PARAM_CUTOFF,
    PARAM_RESONANCE,
    PARAM_ENVELOPE,
    PARAM_ATTACK,
    PARAM_DECAY,
    PARAM_WAVE,
    PARAM_DRIVE,
    PARAM_DELAY,
    PARAM_SYNC,
    PARAM_MIDI_CHANNEL,
    PARAM_MIDI_SEND,
    PARAM_COUNT
} Parameter;

typedef enum {
    EDIT_SEQUENCE,
    EDIT_PARAMETERS,
    EDIT_PREFERENCES,
    EDIT_SONG
} EditMode;

typedef enum {
    SONG_FIELD_PATTERN,
    SONG_FIELD_REPEATS,
    SONG_FIELD_LOOP_ENABLE,
    SONG_FIELD_LOOP_START,
    SONG_FIELD_LOOP_END,
    SONG_FIELD_COUNT
} SongField;

typedef enum {
    LFO_DEST_CUTOFF,
    LFO_DEST_RESONANCE,
    LFO_DEST_ENVELOPE,
    LFO_DEST_DECAY,
    LFO_DEST_DRIVE,
    LFO_DEST_DELAY,
    LFO_DEST_TUNE,
    LFO_DEST_COUNT
} LFODestination;

static volatile Step sequence[SEQUENCE_LENGTH];
static volatile int playing = 1;
static volatile int playhead = -1;
static volatile int bpm = 128;
static volatile int cutoff = 34;
static volatile int resonance = 72;
static volatile int envelope_mod = 72;
static volatile int attack = 4;
static volatile int decay = 48;
static volatile int square_wave = 0;
static volatile int drive = 28;
static volatile PSPSyncProfile sync_profile = PSPSYNC_PROFILE_OFF;
static volatile int midi_channel = 1;
static volatile int midi_send_enabled = 1;
static volatile int light_mode = 0;
static volatile int master_tune = 0;
static volatile int key_tracking = 0;
static volatile int accent_depth = 45;
static volatile int slide_time = 50;
static volatile int delay_time = 50;
static volatile int delay_feedback_amount = 22;
static volatile int delay_mix = 22;
static volatile int live_transpose = 0;
static volatile int live_note = 0;
static volatile int live_note_active = 0;
static volatile int midi_clock_slave = 0;
static volatile unsigned int external_step_pending;

static float phase;
static float frequency = 110.0f;
static float target_frequency = 110.0f;
static float filter_stage[4];
static float filter_envelope;
static float filter_envelope_trigger;
static float amp_envelope;
static float accent_gain = 1.0f;
static int voice_gate;
static int voice_slide;
static float delay_buffer[44100];
static int delay_write_index;
static uint32_t random_state;
static uint32_t *framebuffer;
static uint32_t *framebuffers[2];
static void *gu_framebuffers[2];
static int draw_buffer_index = 1;
static unsigned int __attribute__((aligned(16))) display_list[32768];
static int gu_frame_active;
static int gu_ready;

#define UI_TEXT_QUEUE_MAX 128U
#define UI_TEXT_MAX 80U

typedef struct {
    int x;
    int y;
    uint32_t foreground;
    uint32_t background;
    char text[UI_TEXT_MAX];
} UiTextCommand;

static UiTextCommand ui_text_queue[UI_TEXT_QUEUE_MAX];
static unsigned int ui_text_count;
static PSPSyncClock sync_clock;
static volatile int exit_requested;
static MidiOutputSnapshot midi_snapshot;
static int midi_available;
static int midi_init_result;
static MidiParser midi_controls;
static Transport transport;
static Pattern runtime_pattern;
static RandomizerSettings randomizer_settings;
static StorageSong song;
static unsigned int pattern_slot;
static uint8_t pattern_length = SEQUENCE_LENGTH;
static PatternDirection pattern_direction = PATTERN_FORWARD;
static int8_t pattern_transpose;
static uint8_t pattern_swing = 50U;
static int pattern_saved = 0;
static volatile int runtime_pattern_dirty;
static int preference_cursor;
static volatile int lfo_enabled = 0;
static volatile int lfo_rate = 35;
static volatile int lfo_depth = 35;
static volatile int lfo_quantized_notes = 0;
static volatile LFODestination lfo_destination = LFO_DEST_CUTOFF;
static float lfo_phase;
static uint32_t midi_clock_count;
static int midi_learn_armed;
static volatile int song_mode;
static unsigned int song_entry_index;
static unsigned int song_repeat_count;
static volatile unsigned int song_step_count;
static volatile unsigned int song_cycle_pending;
static int previous_playhead = -1;
static int song_saved;

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    exit_requested = 1;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int callback = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(callback);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thread = sceKernelCreateThread("exit_thread", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (thread >= 0) {
        sceKernelStartThread(thread, 0, NULL);
    }
}

static float clampf(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static float midi_frequency(int note)
{
    float cents = (float)master_tune / 100.0f;
    return 440.0f * powf(2.0f, ((float)note - 69.0f + cents) / 12.0f);
}

/* Smooth asymmetric diode-pair transfer used by each ladder section. */
static float diode_ladder_clip(float value)
{
    if (value >= 0.0f) return value / (1.0f + value);
    return value / (1.0f - value);
}

static uint32_t next_random(void)
{
    uint32_t x = random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    random_state = x;
    return x;
}

static void randomize_sequence(void)
{
    Pattern pattern;
    unsigned int i;
    pattern_init(&pattern);
    for (i = 0; i < SEQUENCE_LENGTH; ++i) pattern.steps[i] = sequence[i];
    randomizer_generate(&pattern, &randomizer_settings, &random_state);
    for (i = 0; i < SEQUENCE_LENGTH; ++i) sequence[i] = pattern.steps[i];
}

static void mutate_sequence(uint8_t amount)
{
    Pattern pattern;
    unsigned int i;
    pattern_init(&pattern);
    for (i = 0; i < SEQUENCE_LENGTH; ++i) pattern.steps[i] = sequence[i];
    randomizer_mutate(&pattern, &randomizer_settings, amount, &random_state);
    for (i = 0; i < SEQUENCE_LENGTH; ++i) sequence[i] = pattern.steps[i];
}

static const char *scale_root_name(uint8_t root)
{
    static const char *names[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return names[root % 12U];
}

static const char *scale_mode_name(RandomScale scale)
{
    static const char *names[RANDOM_SCALE_COUNT] = {
        "CHR", "MAJ", "MIN", "DOR", "PHR", "PMAJ", "PMIN"
    };
    if (scale < RANDOM_SCALE_CHROMATIC || scale >= RANDOM_SCALE_COUNT) {
        return "CHR";
    }
    return names[scale];
}

static int scale_quantize_note(int note)
{
    int distance;
    if (note < (int)PSP303_MIN_NOTE) note = (int)PSP303_MIN_NOTE;
    if (note > (int)PSP303_MAX_NOTE) note = (int)PSP303_MAX_NOTE;
    for (distance = 0; distance < 12; ++distance) {
        int lower = note - distance;
        int upper = note + distance;
        if (lower >= (int)PSP303_MIN_NOTE
            && randomizer_note_in_scale((uint8_t)lower, randomizer_settings.root,
                                        randomizer_settings.scale)) {
            return lower;
        }
        if (upper <= (int)PSP303_MAX_NOTE
            && randomizer_note_in_scale((uint8_t)upper, randomizer_settings.root,
                                        randomizer_settings.scale)) {
            return upper;
        }
    }
    return note;
}

static int performance_note(int note)
{
    note += (int)pattern_transpose + live_transpose;
    return scale_quantize_note(note);
}

static void adjust_performance_key(int direction)
{
    int root = (int)randomizer_settings.root + (direction < 0 ? -1 : 1);
    if (root < 0) root = 11;
    if (root > 11) root = 0;
    randomizer_settings.root = (uint8_t)root;
    /* Keep the selected key close to the source-C pattern for immediate
       performance transposition, including the C/B wrap. */
    live_transpose = root > 6 ? root - 12 : root;
}

static void adjust_performance_scale(int direction)
{
    int scale = (int)randomizer_settings.scale + (direction < 0 ? -1 : 1);
    if (scale < 0) scale = RANDOM_SCALE_COUNT - 1;
    if (scale >= RANDOM_SCALE_COUNT) scale = 0;
    randomizer_settings.scale = (RandomScale)scale;
}

static void load_default_sequence(void)
{
    static const uint8_t notes[SEQUENCE_LENGTH] = {
        36, 36, 48, 36, 43, 36, 39, 36, 36, 46, 36, 43, 39, 36, 34, 36
    };
    static const uint8_t active[SEQUENCE_LENGTH] = {
        1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1
    };
    int i;
    for (i = 0; i < SEQUENCE_LENGTH; ++i) {
        sequence[i].note = notes[i];
        sequence[i].active = active[i];
        sequence[i].accent = (i == 0 || i == 8 || i == 12);
        sequence[i].slide = (i == 2 || i == 9 || i == 14);
        sequence[i].probability = 100;
        sequence[i].ratchet_count = 1;
        sequence[i].gate = 75;
    }
}

static void build_midi_sequence(MidiOutputSequence *midi_sequence)
{
    int i;

    for (i = 0; i < SEQUENCE_LENGTH; ++i) {
        midi_sequence->steps[i].note = (uint8_t)performance_note(sequence[i].note);
        midi_sequence->steps[i].velocity = sequence[i].accent ? 120U : 100U;
        midi_sequence->steps[i].length_clocks = sequence[i].slide ? 6U : 4U;
        midi_sequence->steps[i].enabled = sequence[i].active;
    }
}

static void build_usb_driver_path(char *path, size_t path_size, int argc, char **argv)
{
    static const char *fallback_paths[] = {
        "UsbMidiDriver.prx",
        "./UsbMidiDriver.prx",
        "ms0:/PSP/GAME/PSP303/UsbMidiDriver.prx"
    };
    SceIoStat file_status;
    const char *slash;
    size_t index;

    if (argc > 0 && argv != NULL && argv[0] != NULL) {
        slash = strrchr(argv[0], '/');
        if (slash != NULL) {
            size_t directory_length = (size_t)(slash - argv[0] + 1);
            if (directory_length + strlen("UsbMidiDriver.prx") + 1U <= path_size) {
                memcpy(path, argv[0], directory_length);
                snprintf(path + directory_length, path_size - directory_length,
                         "UsbMidiDriver.prx");
                if (sceIoGetstat(path, &file_status) >= 0) return;
            }
        }
    }
    for (index = 0U; index < sizeof(fallback_paths) / sizeof(fallback_paths[0]); ++index) {
        if (sceIoGetstat(fallback_paths[index], &file_status) >= 0) {
            snprintf(path, path_size, "%s", fallback_paths[index]);
            return;
        }
    }
    /* Preserve the documented path so the loader returns a useful PSP error. */
    snprintf(path, path_size, "%s", fallback_paths[2]);
}

static void pattern_from_sequence(Pattern *pattern)
{
    unsigned int i;
    pattern_init(pattern);
    for (i = 0; i < SEQUENCE_LENGTH; ++i) pattern->steps[i] = sequence[i];
    pattern->length = pattern_length;
    pattern->direction = pattern_direction;
    pattern->transpose = pattern_transpose;
    pattern->settings.bpm = (uint16_t)bpm;
    pattern->settings.cutoff = (uint8_t)cutoff;
    pattern->settings.resonance = (uint8_t)resonance;
    pattern->settings.envelope = (uint8_t)envelope_mod;
    pattern->settings.attack = (uint8_t)attack;
    pattern->settings.decay = (uint8_t)decay;
    pattern->settings.waveform = (uint8_t)square_wave;
    pattern->settings.drive = (uint8_t)drive;
    pattern->settings.delay_time = (uint8_t)delay_time;
    pattern->settings.delay_feedback = (uint8_t)delay_feedback_amount;
    pattern->settings.delay_mix = (uint8_t)delay_mix;
    pattern->settings.tune_cents = (int8_t)master_tune;
    pattern->settings.accent_depth = (uint8_t)accent_depth;
    pattern->settings.slide_time = (uint8_t)slide_time;
    pattern->settings.key_tracking = (uint8_t)key_tracking;
    pattern->settings.swing = pattern_swing;
    pattern_clamp(pattern);
}

static void sequence_from_pattern(const Pattern *pattern)
{
    unsigned int i;
    if (pattern == NULL) return;
    for (i = 0; i < SEQUENCE_LENGTH; ++i) sequence[i] = pattern->steps[i];
    pattern_length = pattern->length;
    pattern_direction = pattern->direction;
    pattern_transpose = pattern->transpose;
    pattern_swing = pattern->settings.swing;
    bpm = pattern->settings.bpm;
    cutoff = pattern->settings.cutoff;
    resonance = pattern->settings.resonance;
    envelope_mod = pattern->settings.envelope;
    attack = pattern->settings.attack;
    decay = pattern->settings.decay;
    square_wave = pattern->settings.waveform != 0U;
    drive = pattern->settings.drive;
    delay_time = pattern->settings.delay_time;
    delay_feedback_amount = pattern->settings.delay_feedback;
    delay_mix = pattern->settings.delay_mix;
    master_tune = pattern->settings.tune_cents;
    accent_depth = pattern->settings.accent_depth;
    slide_time = pattern->settings.slide_time;
    key_tracking = pattern->settings.key_tracking;
}

static void storage_directory(void)
{
    (void)sceIoMkdir("ms0:/PSP/GAME/PSP303", 0777);
    (void)sceIoMkdir("ms0:/PSP/GAME/PSP303/patterns", 0777);
}

static void song_file_path(char *path, size_t path_size)
{
    snprintf(path, path_size, "ms0:/PSP/GAME/PSP303/song.p3sg");
}

static void song_default(void)
{
    storage_song_init(&song);
    song.length = 1U;
    song.entries[0].pattern_slot = 0U;
    song.entries[0].repeats = 1U;
    song.loop_enabled = 0U;
    song.loop_start = 0U;
    song.loop_end = 0U;
    song_saved = 0;
}

static int save_song_file(void)
{
    char path[96];
    song_file_path(path, sizeof(path));
    if (storage_song_save(path, &song) < 0) return -1;
    song_saved = 1;
    return 0;
}

static int load_song_file(void)
{
    char path[96];
    song_file_path(path, sizeof(path));
    if (storage_song_load(path, &song) < 0) {
        song_default();
        return -1;
    }
    song_saved = 1;
    return 0;
}

static int load_pattern_slot(unsigned int slot);

/* A song entry always begins at step one. This keeps the loop range in the
   arrangement, rather than inheriting a running pattern's phase. */
static int activate_song_entry(unsigned int index, int restart_transport)
{
    if (song.length == 0U || index >= song.length) return -1;
    if (load_pattern_slot(song.entries[index].pattern_slot) < 0) return -1;
    pattern_saved = 1;
    pattern_from_sequence(&runtime_pattern);
    transport_set_pattern(&transport, &runtime_pattern);
    transport_set_bpm(&transport, runtime_pattern.settings.bpm);
    transport_set_swing(&transport, runtime_pattern.settings.swing);
    song_step_count = 0U;
    song_cycle_pending = 0U;
    if (restart_transport) transport_start(&transport, &runtime_pattern, 1U);
    return 0;
}

static unsigned int next_song_entry(unsigned int index)
{
    if (song.length == 0U) return 0U;
    if (song.loop_enabled && index == song.loop_end) return song.loop_start;
    return index + 1U < song.length ? index + 1U : 0U;
}

static void reset_song_playback(void)
{
    song_entry_index = 0U;
    song_repeat_count = 0U;
    song_step_count = 0U;
    song_cycle_pending = 0U;
    previous_playhead = -1;
}

static void clamp_song_loop(void)
{
    if (song.length == 0U) {
        song.loop_start = 0U;
        song.loop_end = 0U;
        song.loop_enabled = 0U;
        return;
    }
    if (song.loop_start >= song.length) song.loop_start = song.length - 1U;
    if (song.loop_end >= song.length) song.loop_end = song.length - 1U;
    if (song.loop_end < song.loop_start) song.loop_end = song.loop_start;
}

static void insert_song_entry(unsigned int index)
{
    unsigned int i;
    int looping_whole_song;
    if (song.length >= STORAGE_SONG_CHAIN_MAX) return;
    if (index > song.length) index = song.length;
    looping_whole_song = song.loop_enabled && song.loop_start == 0U
                      && song.loop_end == song.length - 1U;
    if (song.loop_enabled) {
        if (index <= song.loop_start) ++song.loop_start;
        if (index <= song.loop_end) ++song.loop_end;
    }
    for (i = song.length; i > index; --i) song.entries[i] = song.entries[i - 1U];
    song.entries[index].pattern_slot = (uint8_t)pattern_slot;
    song.entries[index].repeats = 1U;
    ++song.length;
    if (looping_whole_song) song.loop_end = song.length - 1U;
    clamp_song_loop();
    song_saved = 0;
}

static void delete_song_entry(unsigned int index)
{
    unsigned int i;
    if (song.length <= 1U || index >= song.length) return;
    if (song.loop_enabled) {
        if (index < song.loop_start) --song.loop_start;
        if (index <= song.loop_end && song.loop_end > 0U) --song.loop_end;
    }
    for (i = index; i + 1U < song.length; ++i) song.entries[i] = song.entries[i + 1U];
    --song.length;
    clamp_song_loop();
    song_saved = 0;
}

static void adjust_song_field(SongField field, unsigned int cursor, int direction)
{
    StorageSongEntry *entry;
    if (song.length == 0U) return;
    if (cursor >= song.length) cursor = song.length - 1U;
    entry = &song.entries[cursor];
    switch (field) {
        case SONG_FIELD_PATTERN: {
            int value = (int)entry->pattern_slot + direction;
            if (value < 0) value = STORAGE_PATTERN_SLOT_COUNT - 1U;
            if (value >= (int)STORAGE_PATTERN_SLOT_COUNT) value = 0;
            entry->pattern_slot = (uint8_t)value;
            break;
        }
        case SONG_FIELD_REPEATS: {
            int value = (int)entry->repeats + direction;
            if (value < 1) value = 1;
            if (value > 16) value = 16;
            entry->repeats = (uint8_t)value;
            break;
        }
        case SONG_FIELD_LOOP_ENABLE:
            song.loop_enabled = !song.loop_enabled;
            /* Enabling Loop means loop the full arrangement by default, never
               just the first pattern. Start and End can then define a region. */
            if (song.loop_enabled) {
                song.loop_start = 0U;
                song.loop_end = song.length - 1U;
            }
            break;
        case SONG_FIELD_LOOP_START: {
            int value = (int)song.loop_start + direction;
            if (value < 0) value = 0;
            if (value >= song.length) value = song.length - 1U;
            song.loop_start = (uint8_t)value;
            break;
        }
        case SONG_FIELD_LOOP_END: {
            int value = (int)song.loop_end + direction;
            if (value < 0) value = 0;
            if (value >= song.length) value = song.length - 1U;
            song.loop_end = (uint8_t)value;
            break;
        }
        default:
            return;
    }
    clamp_song_loop();
    song_saved = 0;
}

static int save_pattern_slot(unsigned int slot)
{
    Pattern pattern;
    char path[128];
    pattern_from_sequence(&pattern);
    if (slot >= STORAGE_PATTERN_SLOT_COUNT) return -1;
    snprintf(path, sizeof(path), "ms0:/PSP/GAME/PSP303/patterns/pattern%02u.p303", slot);
    return storage_pattern_save(path, &pattern);
}

static int load_pattern_slot(unsigned int slot)
{
    Pattern pattern;
    char path[128];
    if (slot >= STORAGE_PATTERN_SLOT_COUNT) return -1;
    snprintf(path, sizeof(path), "ms0:/PSP/GAME/PSP303/patterns/pattern%02u.p303", slot);
    if (storage_pattern_load(path, &pattern) < 0) return -1;
    sequence_from_pattern(&pattern);
    pattern_slot = slot;
    return 0;
}

/* Pattern-slot navigation is a load action: the selected slot and the
   sequencer grid must never disagree. Missing slots leave the current pattern
   untouched so an unsaved slot cannot silently overwrite the edit buffer. */
static int select_pattern_slot(int direction)
{
    unsigned int slot;
    if (direction < 0) {
        slot = pattern_slot == 0U ? STORAGE_PATTERN_SLOT_COUNT - 1U
                                  : pattern_slot - 1U;
    } else {
        slot = (pattern_slot + 1U) % STORAGE_PATTERN_SLOT_COUNT;
    }
    if (load_pattern_slot(slot) < 0) return -1;
    pattern_saved = 1;
    return 0;
}

static void apply_midi_input(void)
{
    UsbMidiEvent events[PSP_USB_MIDI_MAX_BATCH_EVENTS];
    int count;
    int i;
    if (!midi_available) return;
    count = UsbMidi_Read(events, PSP_USB_MIDI_MAX_BATCH_EVENTS);
    if (count < 1) return;
    for (i = 0; i < count; ++i) {
        MidiEvent event;
        uint8_t status = events[i].status;
        uint8_t command = status & 0xF0U;
        memset(&event, 0, sizeof(event));
        event.channel = status & 0x0FU;
        event.data1 = events[i].data1;
        event.data2 = events[i].data2;
        event.target = MIDI_CC_TARGET_NONE;
        if (status == 0xF8U) event.type = MIDI_EVENT_CLOCK;
        else if (status == 0xFAU) event.type = MIDI_EVENT_START;
        else if (status == 0xFCU) event.type = MIDI_EVENT_STOP;
        else if (status == 0xFBU) event.type = MIDI_EVENT_CONTINUE;
        else if (status == 0xF2U) {
            event.type = MIDI_EVENT_SONG_POSITION;
            event.value = (uint16_t)events[i].data1 | (uint16_t)events[i].data2 << 7;
        } else if (command == 0x90U && events[i].data2 != 0U) event.type = MIDI_EVENT_NOTE_ON;
        else if (command == 0x80U || (command == 0x90U && events[i].data2 == 0U)) event.type = MIDI_EVENT_NOTE_OFF;
        else if (command == 0xB0U) {
            event.type = MIDI_EVENT_CONTROL_CHANGE;
            (void)midi_cc_map_lookup(&midi_controls.cc_map, event.channel, event.data1, &event.target);
        } else continue;
        if (event.type == MIDI_EVENT_NOTE_ON) {
            live_note = event.data1;
            live_note_active = event.data2 != 0U;
            midi_controls.active_notes[event.channel][event.data1] = 1U;
            target_frequency = midi_frequency(live_note);
            accent_gain = event.data2 >= 100U ? 1.0f + ((float)accent_depth / 100.0f) : 1.0f;
        } else if (event.type == MIDI_EVENT_NOTE_OFF) {
            if (event.data1 == (uint8_t)live_note) live_note_active = 0;
            midi_controls.active_notes[event.channel][event.data1] = 0U;
        } else if (event.type == MIDI_EVENT_CONTROL_CHANGE) {
            if (midi_learn_armed) {
                (void)midi_cc_map_learn(&midi_controls.cc_map, MIDI_CC_TARGET_CUTOFF,
                                        event.channel, event.data1);
                midi_learn_armed = 0;
            }
            if (event.target != MIDI_CC_TARGET_NONE) {
                int value = ((int)event.data2 * 100 + 63) / 127;
                switch (event.target) {
                    case 0: cutoff = value; break;
                    case 1: resonance = value; break;
                    case 2: envelope_mod = value; break;
                    case 3: decay = value; break;
                    case 4: drive = value; break;
                    case 5: delay_time = value; break;
                    case 6: delay_feedback_amount = value; break;
                    case 7: delay_mix = value; break;
                    case 8: transport_set_swing(&transport, (uint8_t)(50U + value / 4U)); break;
                    case 9: live_transpose = value - 50; break;
                    default: break;
                }
            }
        } else if (event.type == MIDI_EVENT_CLOCK && midi_clock_slave) {
            ++midi_clock_count;
            if ((midi_clock_count % 6U) == 0U && external_step_pending < 32U) {
                ++external_step_pending;
            }
        } else if (event.type == MIDI_EVENT_START) {
            midi_clock_slave = 1; playing = 1;
            transport_set_source(&transport, TRANSPORT_SOURCE_MIDI);
            transport_midi_start(&transport, &runtime_pattern);
            if (midi_available) (void)MidiOutput_Stop();
            midi_clock_count = 0U;
        } else if (event.type == MIDI_EVENT_STOP) {
            playing = 0; live_note_active = 0; midi_clock_slave = 1; transport_midi_stop(&transport);
            memset(midi_controls.active_notes, 0, sizeof(midi_controls.active_notes));
            if (midi_available) (void)MidiOutput_Stop();
            (void)midi_parser_all_notes_off(&midi_controls, MIDI_ANY_CHANNEL);
        } else if (event.type == MIDI_EVENT_CONTINUE) {
            midi_clock_slave = 1; playing = 1;
            transport_set_source(&transport, TRANSPORT_SOURCE_MIDI);
            transport_midi_continue(&transport, &runtime_pattern);
        }
    }
}

static void trigger_step(int index, int transport_confirmed)
{
    int previous = (index + SEQUENCE_LENGTH - 1) % SEQUENCE_LENGTH;
    Step step;
    step.note = sequence[index].note;
    step.active = sequence[index].active;
    step.accent = sequence[index].accent;
    step.slide = sequence[index].slide;
    step.probability = sequence[index].probability;
    step.ratchet_count = sequence[index].ratchet_count;
    step.gate = sequence[index].gate;

    if (!step.active || (!transport_confirmed && step.probability < 100U
        && (next_random() % 100U) >= step.probability)) {
        return;
    }

    {
        int note = performance_note(step.note);
        target_frequency = midi_frequency(note);
    }
    voice_slide = sequence[previous].active && sequence[previous].slide;
    if (!voice_slide) {
        /* Keep oscillator phase continuous at note boundaries. Resetting it
         * produces an audible step regardless of filter, LFO, or drive. */
        frequency += (target_frequency - frequency) * 0.08f;
    }
    if (!voice_slide) filter_envelope_trigger = 1.0f;
    accent_gain = step.accent ? 1.0f + ((float)accent_depth / 125.0f) : 1.0f;
    voice_gate = 1;
}

#define LFO_SYNC_DIVISION_COUNT 8

static int lfo_sync_division_index(int rate)
{
    if (rate < 0) rate = 0;
    if (rate > 100) rate = 100;
    return (rate * (LFO_SYNC_DIVISION_COUNT - 1) + 50) / 100;
}

static const char *lfo_sync_division_name(int rate)
{
    static const char *names[LFO_SYNC_DIVISION_COUNT] = {
        "4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32"
    };
    return names[lfo_sync_division_index(rate)];
}

static float lfo_free_rate_hz(int rate)
{
    if (rate < 0) rate = 0;
    if (rate > 100) rate = 100;
    return 0.1f + ((float)rate / 100.0f) * 11.9f;
}

/* Keep display formatting integer-only. PSP firmware printf variants are not
   dependable for %f in a 60 Hz GUI path, unlike PPSSPP's host libc. */
static unsigned int lfo_free_rate_millihz(int rate)
{
    if (rate < 0) rate = 0;
    if (rate > 100) rate = 100;
    return 100U + (unsigned int)rate * 119U;
}

static float lfo_sync_rate_hz(int bpm, int rate)
{
    static const float cycles_per_beat[LFO_SYNC_DIVISION_COUNT] = {
        0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f
    };
    int index = lfo_sync_division_index(rate);
    return ((float)bpm / 60.0f) * cycles_per_beat[index];
}

static void audio_callback(void *buffer, unsigned int length, void *userdata)
{
    StereoSample *output = (StereoSample *)buffer;
    int local_bpm = bpm;
    int local_delay_time = delay_time;
    int delay_samples = ((int)SAMPLE_RATE * (80 + local_delay_time * 920 / 100)) / local_bpm / 4;
    if (delay_samples < 1) delay_samples = 1;
    if (delay_samples >= (int)(sizeof(delay_buffer) / sizeof(delay_buffer[0]))) {
        delay_samples = (int)(sizeof(delay_buffer) / sizeof(delay_buffer[0])) - 1;
    }
    float envelope_seconds = 0.06f + ((float)decay / 100.0f) * 0.62f;
    float envelope_decay = expf(-1.0f / (envelope_seconds * SAMPLE_RATE));
    float attack_seconds = 0.001f + ((float)attack / 100.0f) * 0.080f;
    float amp_attack = 1.0f - expf(-1.0f / (attack_seconds * SAMPLE_RATE));
    float slide_seconds = 0.012f + ((float)slide_time / 100.0f) * 0.280f;
    float slide_coefficient = 1.0f - expf(-1.0f / (slide_seconds * SAMPLE_RATE));
    float resonance_amount = (float)resonance / 100.0f;
    float base_cutoff_hz = 65.0f * powf(2.0f, (float)cutoff / 14.3f);
    float drive_gain = 1.0f + ((float)drive / 100.0f) * 9.0f;
    float delay_wet = ((float)delay_mix / 100.0f) * 0.65f;
    float delay_feedback = ((float)delay_feedback_amount / 100.0f) * 0.78f;
    int local_lfo_enabled = lfo_enabled;
    float local_lfo_increment;
    float local_lfo_depth = (float)lfo_depth / 100.0f;
    int local_lfo_quantized_notes = lfo_quantized_notes;
    unsigned int i;
    (void)userdata;

    local_lfo_increment = (local_lfo_quantized_notes
                           ? lfo_sync_rate_hz(local_bpm, lfo_rate)
                           : lfo_free_rate_hz(lfo_rate)) / SAMPLE_RATE;

    pspSyncSetTempo(&sync_clock, (uint32_t)local_bpm * 1000U);
    if (sync_clock.profile != sync_profile) pspSyncSetProfile(&sync_clock, sync_profile);
    if (playing && !sync_clock.running) pspSyncStart(&sync_clock, 1);
    if (!playing && sync_clock.running) pspSyncStop(&sync_clock);

    {
    float filter_pole = 0.08f;
    for (i = 0; i < length; ++i) {
        PSPSyncFrame sync = {0, 0};
        float sample = 0.0f;
        float lfo_cutoff_multiplier = 1.0f;
        float lfo_modulation = 0.0f;
        float current_resonance = resonance_amount;
        float current_envelope_mod = (float)envelope_mod;
        float current_envelope_decay = envelope_decay;
        float current_drive_gain = drive_gain;
        float current_delay_wet = delay_wet;
        float current_delay_feedback = delay_feedback;
        float current_target_frequency = target_frequency;

        if (local_lfo_enabled && local_lfo_depth > 0.0f) {
            float lfo_value = sinf(lfo_phase * 6.28318530718f) * local_lfo_depth;
            if (local_lfo_quantized_notes) {
                /* At full depth, quantized mode spans one octave in semitones. */
                float semitones = floorf(lfo_value * 12.0f + 0.5f);
                lfo_modulation = semitones / 12.0f;
            } else {
                lfo_modulation = lfo_value;
            }
            switch (lfo_destination) {
                case LFO_DEST_CUTOFF:
                    lfo_cutoff_multiplier = local_lfo_quantized_notes
                                          ? powf(2.0f, lfo_modulation)
                                          : 1.0f + lfo_modulation * 0.75f;
                    break;
                case LFO_DEST_RESONANCE:
                    current_resonance = clampf(resonance_amount
                                               + lfo_modulation * 0.45f,
                                               0.0f, 0.98f);
                    break;
                case LFO_DEST_ENVELOPE:
                    current_envelope_mod = clampf((float)envelope_mod
                                                  + lfo_modulation * 60.0f,
                                                  0.0f, 100.0f);
                    break;
                case LFO_DEST_DECAY: {
                    float decay_rate = 1.0f - envelope_decay;
                    decay_rate *= 1.0f - lfo_modulation * 0.65f;
                    current_envelope_decay = 1.0f - clampf(decay_rate,
                                                            0.00001f, 0.5f);
                    break;
                }
                case LFO_DEST_DRIVE: {
                    float modulated_drive = clampf((float)drive
                                                   + lfo_modulation * 60.0f,
                                                   0.0f, 100.0f);
                    current_drive_gain = 1.0f + (modulated_drive / 100.0f) * 9.0f;
                    break;
                }
                case LFO_DEST_DELAY:
                    current_delay_wet = clampf(delay_wet
                                               * (1.0f + lfo_modulation * 0.9f),
                                               0.0f, 0.65f);
                    current_delay_feedback = clampf(delay_feedback
                                                    * (1.0f + lfo_modulation * 0.7f),
                                                    0.0f, 0.78f);
                    break;
                case LFO_DEST_TUNE:
                default:
                    break;
            }
        }
        lfo_phase += local_lfo_increment;
        if (lfo_phase >= 1.0f) lfo_phase -= 1.0f;

        if (lfo_destination == LFO_DEST_TUNE
            && local_lfo_enabled && local_lfo_depth > 0.0f) {
            float tune_semitones = local_lfo_quantized_notes
                                 ? lfo_modulation * 12.0f
                                 : lfo_modulation * 2.0f;
            current_target_frequency *= expf(tune_semitones * 0.057762265f);
        }

        if (!midi_clock_slave) {
            TransportEvent transport_event;
            sync = pspSyncNext(&sync_clock);
            sync.tick = 0;
            (void)transport_process_sample(&transport, &runtime_pattern);
            while (transport_poll_event(&transport, &transport_event) != 0) {
                if (transport_event.type == TRANSPORT_EVENT_STEP) {
                    sync.tick = 1;
                    playhead = transport_event.step_index;
                    if (song_mode && playing) {
                        unsigned int length = runtime_pattern.length;
                        if (length < PSP303_MIN_STEPS || length > PSP303_MAX_STEPS) {
                            length = SEQUENCE_LENGTH;
                        }
                        if (++song_step_count >= length) {
                            song_step_count = 0U;
                            if (song_cycle_pending < UINT32_MAX) ++song_cycle_pending;
                        }
                    }
                } else if (transport_event.type == TRANSPORT_EVENT_NOTE_ON) {
                    trigger_step(transport_event.step_index, 1);
                } else if (transport_event.type == TRANSPORT_EVENT_NOTE_OFF) {
                    /* A slide holds the gate through its following note. */
                    if (!sequence[transport_event.step_index].slide) voice_gate = 0;
                }
            }
        } else if (external_step_pending > 0U) {
            sync.tick = 1;
            --external_step_pending;
        }

        if (playing) {
            if (sync.tick) {
                if (midi_clock_slave) {
                    playhead = (playhead + 1) % SEQUENCE_LENGTH;
                    if (song_mode && playing) {
                        unsigned int length = runtime_pattern.length;
                        if (length < PSP303_MIN_STEPS || length > PSP303_MAX_STEPS) {
                            length = SEQUENCE_LENGTH;
                        }
                        if (++song_step_count >= length) {
                            song_step_count = 0U;
                            if (song_cycle_pending < UINT32_MAX) ++song_cycle_pending;
                        }
                    }
                    trigger_step(playhead, 0);
                }
            }

            frequency += (current_target_frequency - frequency)
                       * (voice_slide ? slide_coefficient : 0.08f);
            phase += frequency / SAMPLE_RATE;
            if (phase >= 1.0f) phase -= 1.0f;

            {
                int voice_active = live_note_active || voice_gate;
                /* A fast cutoff jump at a note boundary is a full-band
                 * impulse when the ladder has resonance. Slew this control
                 * signal at the filter input instead of post-processing the
                 * output, which also keeps the audio path stable. */
                filter_envelope += (filter_envelope_trigger - filter_envelope)
                                 * 0.0015f;
                filter_envelope_trigger *= current_envelope_decay;
                if (voice_active) {
                float oscillator = square_wave ? (phase < 0.5f ? 0.72f : -0.72f)
                                               : (phase * 2.0f - 1.0f);
                float envelope_scale = 1.0f + filter_envelope
                                     * (current_envelope_mod / 100.0f) * 7.0f;
                float tracked_note = live_note_active ? (float)live_note
                                   : (playhead >= 0 ? (float)sequence[playhead].note : 36.0f);
                float note_tracking = ((tracked_note - 36.0f) / 72.0f)
                                    * ((float)key_tracking / 100.0f);
                float cutoff_hz = clampf(base_cutoff_hz * (1.0f + note_tracking)
                                         * envelope_scale * lfo_cutoff_multiplier,
                                         40.0f, 11000.0f);
                float normalized = clampf(cutoff_hz / (SAMPLE_RATE * 0.5f),
                                          0.002f, 0.92f);
                /* Approximate the ladder pole cheaply. The cutoff moves
                   smoothly, so updating every fourth sample is inaudible and
                   keeps the PSP audio callback within its realtime budget. */
                if ((i & 3U) == 0U) {
                    filter_pole = clampf(normalized
                                         * (1.36f - 0.36f * normalized),
                                         0.002f, 0.72f);
                }
                float feedback = current_resonance * 3.85f
                               * diode_ladder_clip(filter_stage[3]);
                float input = diode_ladder_clip(oscillator - feedback);

                /* Four cascaded nonlinear diode-ladder sections: 24 dB/oct. */
                filter_stage[0] += filter_pole
                    * (input - filter_stage[0]);
                filter_stage[1] += filter_pole
                    * (diode_ladder_clip(filter_stage[0]) - filter_stage[1]);
                filter_stage[2] += filter_pole
                    * (diode_ladder_clip(filter_stage[1]) - filter_stage[2]);
                filter_stage[3] += filter_pole
                    * (diode_ladder_clip(filter_stage[2]) - filter_stage[3]);

                if (voice_active) amp_envelope += (1.0f - amp_envelope) * amp_attack;
                else amp_envelope *= envelope_decay;
                sample = diode_ladder_clip(filter_stage[3])
                       * amp_envelope * accent_gain * 0.72f;
                } else {
                amp_envelope += (0.0f - amp_envelope) * 0.0015f;
                sample = filter_stage[3] * amp_envelope * 0.4f;
                }
            }
        } else {
            amp_envelope += (0.0f - amp_envelope) * 0.0015f;
            sample = filter_stage[3] * amp_envelope * 0.25f;
        }

        if (drive > 0 || current_drive_gain > 1.0f) {
            float driven = sample * current_drive_gain;
            sample = driven / (1.0f + fabsf(driven));
        }

        {
            int read_index = delay_write_index - delay_samples;
            float delayed;
            while (read_index < 0) read_index += (int)(sizeof(delay_buffer) / sizeof(delay_buffer[0]));
            delayed = delay_buffer[read_index];
            delay_buffer[delay_write_index] = clampf(sample + delayed * current_delay_feedback,
                                                     -1.0f, 1.0f);
            delay_write_index = (delay_write_index + 1) % (int)(sizeof(delay_buffer) / sizeof(delay_buffer[0]));
            sample = sample * (1.0f - current_delay_wet * 0.35f)
                   + delayed * current_delay_wet;
        }

        sample = clampf(sample, -1.0f, 1.0f);
        output[i].right = (int16_t)(sample * 26000.0f);
        output[i].left = sync_profile == PSPSYNC_PROFILE_OFF ? output[i].right : sync.pulse;
    }
    }
}

static const char *pitch_name(int note)
{
    static const char *names[] = {"C ", "C#", "D ", "D#", "E ", "F ",
                                  "F#", "G ", "G#", "A ", "A#", "B "};
    return names[note % 12];
}

static const char *lfo_destination_name(LFODestination destination)
{
    static const char *names[LFO_DEST_COUNT] = {
        "CUTOFF", "RESONANCE", "ENVELOPE", "DECAY",
        "DRIVE", "DELAY", "TUNE"
    };
    if (destination < 0 || destination >= LFO_DEST_COUNT) return "CUTOFF";
    return names[destination];
}

typedef struct {
    uint32_t color;
    int16_t x;
    int16_t y;
    int16_t z;
} UiVertex;

static void begin_gu_frame(void)
{
    if (!gu_ready || gu_frame_active) return;
    sceGuStart(GU_DIRECT, display_list);
    sceGuDrawBuffer(GU_PSM_8888, gu_framebuffers[draw_buffer_index], FRAME_STRIDE);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_BLEND);
    gu_frame_active = 1;
}

static void finish_gu_frame(void)
{
    if (!gu_frame_active) return;
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    gu_frame_active = 0;
}

static void flush_text_queue(void)
{
    unsigned int i;
    pspDebugScreenSetBase(framebuffer);
    for (i = 0U; i < ui_text_count; ++i) {
        UiTextCommand *command = &ui_text_queue[i];
        int character;
        pspDebugScreenSetTextColor(command->foreground);
        pspDebugScreenSetBackColor(command->background);
        for (character = 0; command->text[character] != '\0'; ++character) {
            pspDebugScreenPutChar(command->x + character * 8, command->y,
                                  command->foreground,
                                  (uint8_t)command->text[character]);
        }
    }
    ui_text_count = 0U;
}

static void gu_draw_rect(int x, int y, int width, int height, uint32_t color)
{
    UiVertex *vertices = (UiVertex *)sceGuGetMemory(2U * sizeof(UiVertex));
    if (vertices == NULL) return;
    vertices[0].color = color;
    vertices[0].x = (int16_t)x;
    vertices[0].y = (int16_t)y;
    vertices[0].z = 0;
    vertices[1].color = color;
    vertices[1].x = (int16_t)(x + width);
    vertices[1].y = (int16_t)(y + height);
    vertices[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, NULL, vertices);
}

static void gu_draw_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    UiVertex *vertices = (UiVertex *)sceGuGetMemory(2U * sizeof(UiVertex));
    if (vertices == NULL) return;
    vertices[0].color = color;
    vertices[0].x = (int16_t)x0;
    vertices[0].y = (int16_t)y0;
    vertices[0].z = 0;
    vertices[1].color = color;
    vertices[1].x = (int16_t)x1;
    vertices[1].y = (int16_t)y1;
    vertices[1].z = 0;
    sceGuDrawArray(GU_LINES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, NULL, vertices);
}

static void gu_draw_circle_outline(int center_x, int center_y, int radius,
                                   uint32_t color)
{
    enum { SEGMENTS = 16 };
    UiVertex *vertices = (UiVertex *)sceGuGetMemory(
        (unsigned int)(SEGMENTS * 2) * sizeof(UiVertex));
    int segment;

    if (vertices == NULL) return;
    for (segment = 0; segment < SEGMENTS; ++segment) {
        float start = (float)segment * 6.28318530718f / (float)SEGMENTS;
        float end = (float)(segment + 1) * 6.28318530718f / (float)SEGMENTS;
        UiVertex *start_vertex = &vertices[segment * 2];
        UiVertex *end_vertex = &vertices[segment * 2 + 1];

        start_vertex->color = color;
        start_vertex->x = (int16_t)(center_x + (int)(cosf(start) * radius));
        start_vertex->y = (int16_t)(center_y + (int)(sinf(start) * radius));
        start_vertex->z = 0;
        end_vertex->color = color;
        end_vertex->x = (int16_t)(center_x + (int)(cosf(end) * radius));
        end_vertex->y = (int16_t)(center_y + (int)(sinf(end) * radius));
        end_vertex->z = 0;
    }
    sceGuDrawArray(GU_LINES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   SEGMENTS * 2, NULL, vertices);
}

static void gu_fill_circle(int center_x, int center_y, int radius, uint32_t color)
{
    UiVertex *vertices = (UiVertex *)sceGuGetMemory(18U * sizeof(UiVertex));
    int i;
    if (vertices == NULL) return;
    vertices[0].color = color;
    vertices[0].x = (int16_t)center_x;
    vertices[0].y = (int16_t)center_y;
    vertices[0].z = 0;
    for (i = 0; i < 17; ++i) {
        float angle = (float)i * 6.28318530718f / 16.0f;
        vertices[i + 1].color = color;
        vertices[i + 1].x = (int16_t)(center_x + (int)(cosf(angle) * radius));
        vertices[i + 1].y = (int16_t)(center_y + (int)(sinf(angle) * radius));
        vertices[i + 1].z = 0;
    }
    sceGuDrawArray(GU_TRIANGLE_FAN,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   18, NULL, vertices);
}

/* Small fixed-width font keeps the normal UI entirely inside the GU pass. */
static const char gu_glyph_order[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-+#:./<>[]";
static const uint8_t gu_glyphs[][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}, /* C */
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
    {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F}, /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}, /* I */
    {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}, /* 5 */
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}, /* 9 */
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, /* - */
    {0x04, 0x04, 0x1F, 0x04, 0x04, 0x00, 0x00}, /* + */
    {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}, /* # */
    {0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00}, /* : */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* . */
    {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}, /* / */
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}, /* < */
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}, /* > */
    {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}, /* [ */
    {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}  /* ] */
};

static const uint8_t *gu_glyph(char character)
{
    const char *match = strchr(gu_glyph_order, character);
    if (match == NULL) return NULL;
    return gu_glyphs[(unsigned int)(match - gu_glyph_order)];
}

static void gu_draw_text(int x, int y, uint32_t color, const char *text)
{
    int character_index;
    for (character_index = 0; text[character_index] != '\0'; ++character_index) {
        const uint8_t *glyph = gu_glyph(text[character_index]);
        int row;
        if (glyph == NULL) continue;
        for (row = 0; row < 7; ++row) {
            int column = 0;
            while (column < 5) {
                int start;
                while (column < 5 && (glyph[row] & (1U << (4 - column))) == 0U) {
                    ++column;
                }
                start = column;
                while (column < 5 && (glyph[row] & (1U << (4 - column))) != 0U) {
                    ++column;
                }
                if (column > start) {
                    gu_draw_rect(x + character_index * 8 + start, y + row,
                                 column - start, 1, color);
                }
            }
        }
    }
}

static void fill_rect(int x, int y, int width, int height, uint32_t color)
{
    int row;
    int column;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > SCREEN_WIDTH) width = SCREEN_WIDTH - x;
    if (y + height > SCREEN_HEIGHT) height = SCREEN_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    if (gu_frame_active) {
        gu_draw_rect(x, y, width, height, color);
        return;
    }

    for (row = y; row < y + height; ++row) {
        uint32_t *pixel = framebuffer + row * FRAME_STRIDE + x;
        for (column = 0; column < width; ++column) pixel[column] = color;
    }
}

static void outline_rect(int x, int y, int width, int height, int thickness, uint32_t color)
{
    fill_rect(x, y, width, thickness, color);
    fill_rect(x, y + height - thickness, width, thickness, color);
    fill_rect(x, y, thickness, height, color);
    fill_rect(x + width - thickness, y, thickness, height, color);
}

static void fill_circle(int center_x, int center_y, int radius, uint32_t color)
{
    int y;
    if (gu_frame_active) {
        gu_fill_circle(center_x, center_y, radius, color);
        return;
    }
    for (y = -radius; y <= radius; ++y) {
        int x;
        for (x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                int px = center_x + x;
                int py = center_y + y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * FRAME_STRIDE + px] = color;
                }
            }
        }
    }
}

static void draw_text(int x, int y, uint32_t foreground, uint32_t background, const char *text)
{
    if (gu_frame_active) {
        (void)background;
        gu_draw_text(x, y, foreground, text);
        return;
    }
    {
        int i;
        pspDebugScreenSetTextColor(foreground);
        pspDebugScreenSetBackColor(background);
        for (i = 0; text[i] != '\0'; ++i) {
            pspDebugScreenPutChar(x + i * 8, y, foreground,
                                  (uint8_t)text[i]);
        }
    }
}

static int parameter_value(Parameter parameter)
{
    switch (parameter) {
        case PARAM_BPM: return bpm;
        case PARAM_CUTOFF: return cutoff;
        case PARAM_RESONANCE: return resonance;
        case PARAM_ENVELOPE: return envelope_mod;
        case PARAM_ATTACK: return attack;
        case PARAM_DECAY: return decay;
        case PARAM_WAVE: return square_wave ? 100 : 0;
        case PARAM_DRIVE: return drive;
        case PARAM_DELAY: return delay_mix;
        case PARAM_SYNC: return (int)sync_profile * 50;
        case PARAM_MIDI_CHANNEL: return midi_channel;
        case PARAM_MIDI_SEND: return midi_send_enabled ? 100 : 0;
        default: return 0;
    }
}

static void draw_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    if (gu_frame_active) {
        gu_draw_line(x0, y0, x1, y1, color);
        return;
    }

    while (1) {
        if (x0 >= 0 && x0 < SCREEN_WIDTH && y0 >= 0 && y0 < SCREEN_HEIGHT) {
            framebuffer[y0 * FRAME_STRIDE + x0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        {
            int twice_error = error * 2;
            if (twice_error >= dy) { error += dy; x0 += sx; }
            if (twice_error <= dx) { error += dx; y0 += sy; }
        }
    }
}

static void outline_circle(int center_x, int center_y, int radius, uint32_t color)
{
    if (gu_frame_active) {
        gu_draw_circle_outline(center_x, center_y, radius, color);
        return;
    }
    int angle;
    for (angle = 0; angle < 360; angle += 4) {
        float radians = (float)angle * 0.01745329252f;
        int x = center_x + (int)(cosf(radians) * radius);
        int y = center_y + (int)(sinf(radians) * radius);
        fill_rect(x, y, 2, 2, color);
    }
}

static uint32_t theme_background(void)
{
    return light_mode ? COLOR_WHITE : COLOR_BLACK;
}

static uint32_t theme_foreground(void)
{
    return light_mode ? COLOR_BLACK : COLOR_WHITE;
}

static void draw_parameter_icon(Parameter tile_parameter, Parameter selected,
                                int parameter_editing, int blink_on)
{
    static const char *short_names[PARAM_COUNT] = {
        "BPM", "CUTOFF", "RESONANCE", "ENV MOD", "ATTACK", "DECAY",
        "WAVE", "DRIVE", "DELAY", "SYNC", "MIDI CH", "MIDI OUT"
    };
    int column = (int)tile_parameter % 4;
    int row = (int)tile_parameter / 4;
    int x = 8 + column * 115;
    int y = 48 + row * 56;
    int value = parameter_value(tile_parameter);
    uint32_t page_bg = theme_background();
    uint32_t page_fg = theme_foreground();
    int focused = selected == tile_parameter;
    int inverted = focused && (parameter_editing || blink_on);
    uint32_t background = inverted ? page_fg : page_bg;
    uint32_t foreground = inverted ? page_bg : page_fg;
    char value_text[12];

    fill_rect(x, y, 110, 50, background);
    outline_rect(x, y, 110, 50, 1, page_fg);
    draw_text(x + 6, y + 5, foreground, background, short_names[tile_parameter]);

    if (tile_parameter == PARAM_WAVE) {
        snprintf(value_text, sizeof(value_text), "%s", square_wave ? "SQUARE" : "SAW");
    } else if (tile_parameter == PARAM_SYNC) {
        snprintf(value_text, sizeof(value_text), "%s", pspSyncProfileName(sync_profile));
    } else if (tile_parameter == PARAM_MIDI_SEND) {
        snprintf(value_text, sizeof(value_text), "%s", midi_send_enabled ? "ON" : "OFF");
    } else {
        snprintf(value_text, sizeof(value_text), "%d", value);
    }
    draw_text(x + 6, y + 27, foreground, background, value_text);

    if (tile_parameter != PARAM_WAVE && tile_parameter != PARAM_SYNC
        && tile_parameter != PARAM_MIDI_SEND) {
        int normalized = tile_parameter == PARAM_BPM
                       ? (bpm - 60) * 100 / 140
                       : tile_parameter == PARAM_MIDI_CHANNEL
                       ? (midi_channel - 1) * 100 / 15
                       : value;
        float angle = (-135.0f + normalized * 270.0f / 100.0f) * 0.01745329252f;
        outline_circle(x + 88, y + 31, 11, foreground);
        fill_circle(x + 88, y + 31, 2, foreground);
        draw_line(x + 88, y + 31, x + 88 + (int)(cosf(angle) * 8.0f),
                  y + 31 + (int)(sinf(angle) * 8.0f), foreground);
    } else if (tile_parameter == PARAM_WAVE) {
        if (square_wave) {
            draw_line(x + 54, y + 41, x + 54, y + 24, foreground);
            draw_line(x + 54, y + 24, x + 76, y + 24, foreground);
            draw_line(x + 76, y + 24, x + 76, y + 41, foreground);
            draw_line(x + 76, y + 41, x + 101, y + 41, foreground);
        } else {
            draw_line(x + 54, y + 41, x + 76, y + 24, foreground);
            draw_line(x + 76, y + 24, x + 76, y + 41, foreground);
            draw_line(x + 76, y + 41, x + 101, y + 24, foreground);
        }
    } else if (tile_parameter == PARAM_MIDI_SEND) {
        outline_rect(x + 70, y + 25, 30, 15, 1, foreground);
        fill_rect(midi_send_enabled ? x + 87 : x + 73, y + 28, 10, 9, foreground);
    }
}

static void draw_step_key(int index, int cursor)
{
    int x = 8 + index * 29;
    int y = 124;
    int is_playhead = index == playhead;
    int is_active = sequence[index].active;
    uint32_t page_bg = theme_background();
    uint32_t page_fg = theme_foreground();
    uint32_t background = is_active ? page_fg : page_bg;
    uint32_t foreground = is_active ? page_bg : page_fg;
    char number[4];

    fill_rect(x, y, 27, 62, background);
    outline_rect(x, y, 27, 62, 1, page_fg);
    if (index == cursor) outline_rect(x - 2, y - 2, 31, 66, 1, page_fg);
    if (is_playhead) fill_rect(x + 8, y - 7, 11, 4, page_fg);

    snprintf(number, sizeof(number), "%02d", index + 1);
    draw_text(x + 5, y + 7, foreground, background, number);
    if (is_active) {
        draw_text(x + 5, y + 25, foreground, background, pitch_name(sequence[index].note));
    } else {
        draw_text(x + 9, y + 25, foreground, background, "-");
    }
    if (sequence[index].accent) draw_text(x + 4, y + 45, foreground, background, "A");
    if (sequence[index].slide) draw_text(x + 15, y + 45, foreground, background, "S");
}

static const char *song_field_name(SongField field)
{
    switch (field) {
        case SONG_FIELD_PATTERN: return "PATTERN";
        case SONG_FIELD_REPEATS: return "REPEATS";
        case SONG_FIELD_LOOP_ENABLE: return "LOOP";
        case SONG_FIELD_LOOP_START: return "LOOP START";
        case SONG_FIELD_LOOP_END: return "LOOP END";
        default: return "FIELD";
    }
}

static void draw_breadcrumbs(EditMode edit_mode, uint32_t background,
                             uint32_t foreground)
{
    const char *active = edit_mode == EDIT_SEQUENCE ? "SEQ" :
                         edit_mode == EDIT_PARAMETERS ? "PAR" :
                         edit_mode == EDIT_SONG ? "SONG" : "PREF";
    char text[64];
    snprintf(text, sizeof(text), "PAGE [%s]   SEQ  PAR  PREF  SONG", active);
    draw_text(8, 252, foreground, background, text);
}

static void draw_song_page(unsigned int song_cursor, SongField song_field,
                           int song_editing, int blink_on,
                           uint32_t background, uint32_t foreground)
{
    unsigned int song_length = song.length;
    unsigned int safe_cursor = song_cursor;
    unsigned int first = 0U;
    unsigned int i;
    unsigned int visible;
    char text[64];

    /* Storage is validated on load, but keep the renderer bounded in case a
       partially-written card or a concurrent state change reaches the frame. */
    if (song_length > STORAGE_SONG_CHAIN_MAX) song_length = STORAGE_SONG_CHAIN_MAX;
    if (song_length == 0U) safe_cursor = 0U;
    else if (safe_cursor >= song_length) safe_cursor = song_length - 1U;
    if (song_field < SONG_FIELD_PATTERN || song_field >= SONG_FIELD_COUNT) {
        song_field = SONG_FIELD_PATTERN;
    }
    if (safe_cursor >= 5U) first = safe_cursor - 4U;
    visible = song_length > first ? song_length - first : 0U;
    if (visible > 5U) visible = 5U;

    fill_rect(8, 48, 460, 160, background);
    outline_rect(8, 48, 460, 160, 1, foreground);
    snprintf(text, sizeof(text), "SONG %s  ENTRY %02u/%02u  %s %s",
             song_mode && playing ? "PLAY" : "STOP",
             song_length == 0U ? 0U : safe_cursor + 1U, song_length,
             song_editing ? "EDIT" : "BROWSE", song_field_name(song_field));
    draw_text(16, 56, foreground, background, text);
    draw_text(28, 72, foreground, background, "#       PATTERN       REPEATS");

    for (i = 0U; i < visible; ++i) {
        unsigned int entry_index = first + i;
        int y = 84 + (int)i * 16;
        int pattern_focused = entry_index == safe_cursor
                           && song_field == SONG_FIELD_PATTERN;
        int repeat_focused = entry_index == safe_cursor
                          && song_field == SONG_FIELD_REPEATS;
        int pattern_inverted = pattern_focused && (song_editing || blink_on);
        int repeat_inverted = repeat_focused && (song_editing || blink_on);
        uint32_t pattern_background = pattern_inverted ? foreground : background;
        uint32_t pattern_foreground = pattern_inverted ? background : foreground;
        uint32_t repeat_background = repeat_inverted ? foreground : background;
        uint32_t repeat_foreground = repeat_inverted ? background : foreground;

        if (song_mode && playing && entry_index == song_entry_index) {
            draw_text(16, y + 3, foreground, background, ">");
        }
        snprintf(text, sizeof(text), "%02u", entry_index + 1U);
        draw_text(32, y + 3, foreground, background, text);
        fill_rect(78, y, 104, 14, pattern_background);
        outline_rect(78, y, 104, 14, 1, foreground);
        snprintf(text, sizeof(text), "PAT %02u", song.entries[entry_index].pattern_slot);
        draw_text(84, y + 3, pattern_foreground, pattern_background, text);
        fill_rect(194, y, 104, 14, repeat_background);
        outline_rect(194, y, 104, 14, 1, foreground);
        snprintf(text, sizeof(text), "REP %02u", song.entries[entry_index].repeats);
        draw_text(200, y + 3, repeat_foreground, repeat_background, text);
    }

    {
        SongField fields[3] = {
            SONG_FIELD_LOOP_ENABLE, SONG_FIELD_LOOP_START, SONG_FIELD_LOOP_END
        };
        const char *labels[3] = {"LOOP", "START", "END"};
        int values[3] = {
            song.loop_enabled ? 1 : 0,
            song_length == 0U ? 0 : song.loop_start + 1U,
            song_length == 0U ? 0 : song.loop_end + 1U
        };
        for (i = 0U; i < 3U; ++i) {
            int x = 16 + (int)i * 142;
            int focused = song_field == fields[i];
            int inverted = focused && (song_editing || blink_on);
            uint32_t tile_background = inverted ? foreground : background;
            uint32_t tile_foreground = inverted ? background : foreground;
            fill_rect(x, 170, 128, 28, tile_background);
            outline_rect(x, 170, 128, 28, 1, foreground);
            if (fields[i] == SONG_FIELD_LOOP_ENABLE) {
                snprintf(text, sizeof(text), "%s %s", labels[i], values[i] ? "ON" : "OFF");
            } else {
                snprintf(text, sizeof(text), "%s %02d", labels[i], values[i]);
            }
            draw_text(x + 7, 180, tile_foreground, tile_background, text);
        }
    }

    draw_text(8, 216, foreground, background, song_editing
              ? "UP/DOWN VALUE NUB ADJUST X DONE"
              : "DPAD FOCUS X SELECT SQ ADD O DELETE");
    draw_text(8, 230, foreground, background,
              "TRI SAVE L+TRI LOAD R+START PLAY/STOP");
    draw_breadcrumbs(EDIT_SONG, background, foreground);
}

static void draw_lfo_parameter_icon(int index, int lfo_editing, int blink_on,
                                    uint32_t page_background,
                                    uint32_t page_foreground)
{
    static const char *names[5] = {
        "RATE", "DEPTH", "MODE", "ENABLE", "DEST"
    };
    int x = index < 3 ? 8 + index * 115 : 8 + (index - 3) * 115;
    int y = index < 3 ? 48 : 104;
    int focused = index == preference_cursor;
    int inverted = focused && (lfo_editing || blink_on);
    uint32_t tile_background = inverted ? page_foreground : page_background;
    uint32_t tile_foreground = inverted ? page_background : page_foreground;
    char value[12];

    fill_rect(x, y, 110, 50, tile_background);
    outline_rect(x, y, 110, 50, 1, page_foreground);
    draw_text(x + 6, y + 5, tile_foreground, tile_background, names[index]);

    if (index == 0) {
        if (lfo_quantized_notes) {
            snprintf(value, sizeof(value), "%s", lfo_sync_division_name(lfo_rate));
        } else {
            snprintf(value, sizeof(value), "%u MHZ", lfo_free_rate_millihz(lfo_rate));
        }
    }
    else if (index == 1) snprintf(value, sizeof(value), "%03d", lfo_depth);
    else if (index == 2) snprintf(value, sizeof(value), "%s",
                                  lfo_quantized_notes ? "NOTE" : "SMOOTH");
    else if (index == 3) snprintf(value, sizeof(value), "%s",
                                  lfo_enabled ? "ON" : "OFF");
    else snprintf(value, sizeof(value), "%s",
                  lfo_destination_name(lfo_destination));
    draw_text(x + 6, y + 27, tile_foreground, tile_background, value);

    if (index < 2) {
        int normalized = index == 0 ? lfo_rate : lfo_depth;
        float angle = (-135.0f + normalized * 270.0f / 100.0f)
                    * 0.01745329252f;
        outline_circle(x + 88, y + 31, 11, tile_foreground);
        fill_circle(x + 88, y + 31, 2, tile_foreground);
        draw_line(x + 88, y + 31,
                  x + 88 + (int)(cosf(angle) * 8.0f),
                  y + 31 + (int)(sinf(angle) * 8.0f), tile_foreground);
    } else if (index == 2) {
        if (lfo_quantized_notes) {
            draw_line(x + 58, y + 41, x + 58, y + 33, tile_foreground);
            draw_line(x + 58, y + 33, x + 72, y + 33, tile_foreground);
            draw_line(x + 72, y + 33, x + 72, y + 25, tile_foreground);
            draw_line(x + 72, y + 25, x + 96, y + 25, tile_foreground);
        } else {
            draw_line(x + 58, y + 36, x + 68, y + 27, tile_foreground);
            draw_line(x + 68, y + 27, x + 78, y + 36, tile_foreground);
            draw_line(x + 78, y + 36, x + 88, y + 27, tile_foreground);
            draw_line(x + 88, y + 27, x + 98, y + 36, tile_foreground);
        }
    } else if (index == 3) {
        outline_rect(x + 70, y + 25, 30, 15, 1, tile_foreground);
        fill_rect(lfo_enabled ? x + 87 : x + 73, y + 28,
                  10, 9, tile_foreground);
    } else {
        draw_line(x + 70, y + 40, x + 98, y + 40, tile_foreground);
        draw_line(x + 70, y + 40, x + 70, y + 34, tile_foreground);
        draw_line(x + 98, y + 40, x + 98, y + 34, tile_foreground);
    }
}

static void draw_preferences_page(uint32_t background, uint32_t foreground,
                                  int lfo_editing, int blink_on)
{
    int x;

    fill_rect(8, 48, 460, 160, background);
    outline_rect(8, 48, 460, 160, 1, foreground);
    for (x = 0; x < 5; ++x) {
        draw_lfo_parameter_icon(x, lfo_editing, blink_on, background, foreground);
    }

    draw_text(16, 166, foreground, background,
              "LFO -> DESTINATION  NUB X RATE  NUB Y DEPTH");
    draw_text(16, 182, foreground, background,
              lfo_enabled ? "ACTIVE" : "BYPASS" );
    draw_text(8, 216, foreground, background, lfo_editing
              ? "UP/DOWN VALUE   NUB ADJUST   X DONE"
              : "DPAD CHOOSE PARAMETER       X SELECT");
    draw_text(8, 230, foreground, background,
              "TRI MIDI LEARN   SELECT NEXT PAGE   NOTE THEME");
    draw_breadcrumbs(EDIT_PREFERENCES, background, foreground);
}

static void draw_ui(int cursor, Parameter parameter, EditMode edit_mode,
                    int parameter_editing, int lfo_editing, int song_editing,
                    int blink_on, unsigned int song_cursor, SongField song_field)
{
    int i;
    char text[64];
    uint32_t background = theme_background();
    uint32_t foreground = theme_foreground();

    fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, background);
    outline_rect(4, 4, 472, 264, 2, foreground);

    outline_rect(8, 8, 48, 28, 2, foreground);
    draw_text(14, 16, foreground, background,
              edit_mode == EDIT_SEQUENCE ? "SEQ" :
              edit_mode == EDIT_PARAMETERS ? "PAR" :
              edit_mode == EDIT_SONG ? "SONG" : "PREF");
    draw_text(72, 10, foreground, background, "PSP-303 ACID");
    if (!midi_available) {
        snprintf(text, sizeof(text), "USB E%08X", (unsigned int)midi_init_result);
        draw_text(208, 16, foreground, background, text);
    } else if (midi_snapshot.link_established && midi_snapshot.running) {
        draw_text(208, 16, foreground, background, "USB CLOCK");
    } else if (midi_snapshot.link_established) {
        draw_text(216, 16, foreground, background, "USB LINK");
    } else {
        draw_text(216, 16, foreground, background, "USB WAIT");
    }
    outline_rect(352, 8, 116, 28, 2, foreground);
    snprintf(text, sizeof(text), "%c %3d.0", playing ? '>' : '|', bpm);
    draw_text(364, 16, foreground, background, text);
    fill_rect(8, 42, 460, 2, foreground);

    if (edit_mode == EDIT_PREFERENCES) {
        draw_preferences_page(background, foreground, lfo_editing, blink_on);
    } else if (edit_mode == EDIT_SONG) {
        draw_song_page(song_cursor, song_field, song_editing, blink_on,
                       background, foreground);
    } else if (edit_mode == EDIT_PARAMETERS) {
        for (i = 0; i < PARAM_COUNT; ++i) {
            draw_parameter_icon((Parameter)i, parameter, parameter_editing, blink_on);
        }
        fill_rect(8, 218, 460, 2, foreground);
        if (parameter_editing) {
            draw_text(8, 228, foreground, background, "UP/DOWN VALUE   L: x10              X DONE");
        } else {
            draw_text(8, 228, foreground, background, "DPAD CHOOSE PARAMETER             X SELECT");
        }
        draw_text(8, 246, foreground, background, "SELECT SEQUENCER   START RUN   NOTE THEME");
        draw_breadcrumbs(EDIT_PARAMETERS, background, foreground);
    } else {
        snprintf(text, sizeof(text), "CUT %d RES %d ENV %d DEC %d DRV %d DLY %d",
                 cutoff, resonance, envelope_mod, decay, drive, delay_mix);
        draw_text(8, 54, foreground, background, text);
        snprintf(text, sizeof(text), "KEY %s %s SLOT %02u WAVE %s",
                 scale_root_name(randomizer_settings.root),
                 scale_mode_name(randomizer_settings.scale), pattern_slot,
                 square_wave ? "SQUARE" : "SAW");
        draw_text(8, 74, foreground, background, text);
        fill_rect(8, 94, 460, 2, foreground);
        snprintf(text, sizeof(text), "STEP %02d  %s  G:%d A:%d S:%d",
                 cursor + 1, pitch_name(sequence[cursor].note), sequence[cursor].active,
                 sequence[cursor].accent, sequence[cursor].slide);
        draw_text(8, 102, foreground, background, text);
        draw_text(400, 102, foreground, background, playing ? "RUN" : "STOP");
        for (i = 0; i < SEQUENCE_LENGTH; ++i) draw_step_key(i, cursor);
        fill_rect(8, 194, 460, 2, foreground);
        draw_text(8, 204, foreground, background, "X TRIG TRI ACC O SLD SQ RANDOM START RUN");
        draw_text(8, 238, foreground, background, "R UP/DN KEY R L/R MODE L+R L/R SLOT");
        draw_breadcrumbs(EDIT_SEQUENCE, background, foreground);
    }
}

static int initialize_gu(void)
{
    int result = sceGuInit();
    if (result < 0) return result;
    sceGuStart(GU_DIRECT, display_list);
    sceGuDrawBuffer(GU_PSM_8888, gu_framebuffers[draw_buffer_index], FRAME_STRIDE);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, gu_framebuffers[0], FRAME_STRIDE);
    sceGuOffset(2048 - SCREEN_WIDTH / 2, 2048 - SCREEN_HEIGHT / 2);
    sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_BLEND);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    gu_ready = 1;
    return 0;
}

static void present_frame(void)
{
    if (gu_ready) {
        void *next_draw_buffer;
        finish_gu_frame();
        sceDisplayWaitVblankStart();
        next_draw_buffer = sceGuSwapBuffers();
        if (next_draw_buffer == gu_framebuffers[0]) draw_buffer_index = 0;
        else if (next_draw_buffer == gu_framebuffers[1]) draw_buffer_index = 1;
        else draw_buffer_index ^= 1;
        framebuffer = framebuffers[draw_buffer_index];
        pspDebugScreenSetBase(framebuffer);
        flush_text_queue();
        return;
    }
    sceDisplaySetFrameBuf(framebuffer, FRAME_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
    draw_buffer_index ^= 1;
    framebuffer = framebuffers[draw_buffer_index];
    pspDebugScreenSetBase(framebuffer);
}

static void finish_midi_shutdown(void)
{
    SceCtrlData pad;
    unsigned int previous_buttons = 0U;
    unsigned int retry_frames = 0U;
    int result;

    (void)MidiOutput_Stop();
    result = MidiOutput_Shutdown();
    while (result < 0) {
        unsigned int pressed;
        char error_text[48];
        uint32_t background = theme_background();
        uint32_t foreground = theme_foreground();

        fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, background);
        outline_rect(4, 4, 472, 264, 2, foreground);
        draw_text(24, 72, foreground, background, "USB MIDI SHUTDOWN NEEDS ATTENTION");
        snprintf(error_text, sizeof(error_text), "ERROR 0x%08X", (unsigned int)result);
        draw_text(24, 104, foreground, background, error_text);
        draw_text(24, 136, foreground, background, "KEEP THE PSP ON AND USB CONNECTED");
        draw_text(24, 168, foreground, background, "X RETRY");
        present_frame();

        sceCtrlPeekBufferPositive(&pad, 1);
        pressed = pad.Buttons & ~previous_buttons;
        previous_buttons = pad.Buttons;
        ++retry_frames;
        if ((pressed & PSP_CTRL_CROSS) || retry_frames >= 60U) {
            retry_frames = 0U;
            result = MidiOutput_Shutdown();
        }
    }
}

static void adjust_parameter(Parameter parameter, int direction)
{
    switch (parameter) {
        case PARAM_BPM:
            bpm += direction;
            if (bpm < 60) bpm = 60;
            if (bpm > 200) bpm = 200;
            break;
        case PARAM_CUTOFF:
            cutoff += direction;
            if (cutoff < 0) cutoff = 0;
            if (cutoff > 100) cutoff = 100;
            break;
        case PARAM_RESONANCE:
            resonance += direction;
            if (resonance < 0) resonance = 0;
            if (resonance > 100) resonance = 100;
            break;
        case PARAM_ENVELOPE:
            envelope_mod += direction;
            if (envelope_mod < 0) envelope_mod = 0;
            if (envelope_mod > 100) envelope_mod = 100;
            break;
        case PARAM_ATTACK:
            attack += direction;
            if (attack < 0) attack = 0;
            if (attack > 100) attack = 100;
            break;
        case PARAM_DECAY:
            decay += direction;
            if (decay < 0) decay = 0;
            if (decay > 100) decay = 100;
            break;
        case PARAM_WAVE:
            square_wave = !square_wave;
            break;
        case PARAM_DRIVE:
            drive += direction;
            if (drive < 0) drive = 0;
            if (drive > 100) drive = 100;
            break;
        case PARAM_DELAY:
            delay_mix += direction;
            if (delay_mix < 0) delay_mix = 0;
            if (delay_mix > 100) delay_mix = 100;
            break;
        case PARAM_SYNC: {
            int profile = (int)sync_profile + (direction < 0 ? -1 : 1);
            if (profile < 0) profile = PSPSYNC_PROFILE_COUNT - 1;
            if (profile >= PSPSYNC_PROFILE_COUNT) profile = 0;
            sync_profile = (PSPSyncProfile)profile;
            break;
        }
        case PARAM_MIDI_CHANNEL:
            midi_channel += direction;
            if (midi_channel < 1) midi_channel = 1;
            if (midi_channel > 16) midi_channel = 16;
            break;
        case PARAM_MIDI_SEND:
            midi_send_enabled = !midi_send_enabled;
            break;
        default: break;
    }
    if (parameter <= PARAM_DELAY) {
        pattern_saved = 0;
        runtime_pattern_dirty = 1;
    }
}

static void adjust_lfo_rate(int amount);

static void adjust_preference(int direction)
{
    if (preference_cursor == 0) {
        adjust_lfo_rate(direction);
    } else if (preference_cursor == 1) {
        lfo_depth += direction;
        if (lfo_depth < 0) lfo_depth = 0;
        if (lfo_depth > 100) lfo_depth = 100;
    } else if (preference_cursor == 2) {
        lfo_quantized_notes = !lfo_quantized_notes;
    } else if (preference_cursor == 3) {
        lfo_enabled = !lfo_enabled;
    } else if (preference_cursor == 4) {
        int destination = (int)lfo_destination + (direction < 0 ? -1 : 1);
        if (destination < 0) destination = LFO_DEST_COUNT - 1;
        if (destination >= LFO_DEST_COUNT) destination = 0;
        lfo_destination = (LFODestination)destination;
    }
}

static void adjust_lfo_rate(int amount)
{
    if (lfo_quantized_notes) {
        int index = lfo_sync_division_index(lfo_rate);
        if (amount < 0) --index;
        if (amount > 0) ++index;
        if (index < 0) index = 0;
        if (index >= LFO_SYNC_DIVISION_COUNT) index = LFO_SYNC_DIVISION_COUNT - 1;
        lfo_rate = (index * 100 + (LFO_SYNC_DIVISION_COUNT - 1) / 2)
                 / (LFO_SYNC_DIVISION_COUNT - 1);
        return;
    }
    lfo_rate += amount;
    if (lfo_rate < 0) lfo_rate = 0;
    if (lfo_rate > 100) lfo_rate = 100;
}

static void adjust_lfo_depth(int amount)
{
    lfo_depth += amount;
    if (lfo_depth < 0) lfo_depth = 0;
    if (lfo_depth > 100) lfo_depth = 100;
}

static int analog_direction(unsigned char value, int invert)
{
    if (value < 80U) return invert ? 1 : -1;
    if (value > 176U) return invert ? -1 : 1;
    return 0;
}

static int analog_stick_direction(unsigned char x, unsigned char y)
{
    int x_direction = analog_direction(x, 0);
    int y_direction = analog_direction(y, 1);
    int x_distance = x > 128U ? (int)x - 128 : 128 - (int)x;
    int y_distance = y > 128U ? (int)y - 128 : 128 - (int)y;

    if (x_direction == 0) return y_direction;
    if (y_direction == 0) return x_direction;
    return x_distance >= y_distance ? x_direction : y_direction;
}

static int analog_repeats(int direction, int *previous_direction,
                          unsigned int *held_frames)
{
    if (direction == 0) {
        *previous_direction = 0;
        *held_frames = 0;
        return 0;
    }
    if (direction != *previous_direction) {
        *previous_direction = direction;
        *held_frames = 0;
        return 1;
    }
    ++(*held_frames);
    return *held_frames >= 10U && ((*held_frames - 10U) % 5U) == 0U;
}

static int dpad_repeats(unsigned int held, unsigned int pressed,
                        unsigned int button, unsigned int *held_frames)
{
    if (!(held & button)) {
        *held_frames = 0;
        return 0;
    }
    if (pressed & button) {
        *held_frames = 0;
        return 1;
    }

    ++(*held_frames);
    return *held_frames >= 18U && ((*held_frames - 18U) % 4U) == 0U;
}

int main(int argc, char **argv)
{
    SceCtrlData pad;
    unsigned int previous_buttons = 0;
    int cursor = 0;
    Parameter parameter = PARAM_CUTOFF;
    EditMode edit_mode = EDIT_SEQUENCE;
    unsigned int song_cursor = 0U;
    SongField song_field = SONG_FIELD_PATTERN;
    int parameter_editing = 0;
    int lfo_editing = 0;
    int song_editing = 0;
    unsigned int ui_frame = 0;
    unsigned int up_held_frames = 0;
    unsigned int down_held_frames = 0;
    unsigned int left_held_frames = 0;
    unsigned int right_held_frames = 0;
    unsigned int analog_x_held_frames = 0;
    unsigned int analog_y_held_frames = 0;
    int previous_analog_x_direction = 0;
    int previous_analog_y_direction = 0;
    unsigned int analog_adjust_held_frames = 0;
    int previous_analog_adjust_direction = 0;
    PSPSyncConfig sync_config = {
        .sample_rate = 44100,
        .bpm_milli = 128000,
        .ticks_per_quarter = 4,
        .pulse_level = 29490
    };
    MidiOutputSequence midi_sequence;
    char usb_driver_path[256];
    int sequence_dirty = 0;
    int midi_sequence_dirty = 0;
    int last_midi_bpm = 128;
    int last_midi_channel = 1;
    int last_midi_send_enabled = 1;

    uintptr_t vram_base;

    setup_callbacks();
    vram_base = (uintptr_t)sceGeEdramGetAddr();
    /* Two uncached VRAM buffers prevent the LCD from scanning a partial redraw. */
    framebuffers[0] = (uint32_t *)(vram_base | 0x40000000u);
    framebuffers[1] = (uint32_t *)((vram_base + FRAMEBUFFER_BYTES) | 0x40000000u);
    pspDebugScreenInitEx(framebuffers[0], PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
    framebuffer = framebuffers[draw_buffer_index];
    pspDebugScreenSetBase(framebuffer);
    gu_framebuffers[0] = (void *)0;
    gu_framebuffers[1] = (void *)(uintptr_t)FRAMEBUFFER_BYTES;
    (void)initialize_gu();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    random_state = sceKernelGetSystemTimeLow();
    if (random_state == 0) random_state = 0x303303u;
    randomizer_defaults(&randomizer_settings);
    midi_parser_init(&midi_controls);
    {
        TransportConfig transport_config;
        transport_defaults(&transport_config);
        transport_config.bpm = 128U;
        transport_config.sample_rate = (uint32_t)SAMPLE_RATE;
        transport_init(&transport, &transport_config);
    }
    storage_song_init(&song);
    load_default_sequence();
    storage_directory();
    (void)load_song_file();
    if (load_pattern_slot(0U) < 0) {
        pattern_slot = 0U;
        /* A fresh install has no pattern file yet. Seed slot 00 so the
           default song can immediately start and chain. */
        if (save_pattern_slot(0U) == 0) pattern_saved = 1;
    } else {
        pattern_saved = 1;
    }
    pattern_from_sequence(&runtime_pattern);
    transport_set_pattern(&transport, &runtime_pattern);
    if (playing) transport_start(&transport, &runtime_pattern, 1U);
    pspSyncInit(&sync_clock, &sync_config);
    build_midi_sequence(&midi_sequence);
    build_usb_driver_path(usb_driver_path, sizeof(usb_driver_path), argc, argv);
    midi_init_result = MidiOutput_Init(usb_driver_path, (unsigned int)bpm, &midi_sequence);
    midi_available = midi_init_result >= 0;
    memset(&midi_snapshot, 0, sizeof(midi_snapshot));
    midi_snapshot.last_result = midi_init_result;
    if (midi_available) {
        (void)MidiOutput_GetSnapshot(&midi_snapshot);
        if (playing) (void)MidiOutput_Start();
    }

    if (pspAudioInit() < 0) {
        finish_midi_shutdown();
        sceKernelExitGame();
        return 1;
    }
    pspAudioSetChannelCallback(AUDIO_CHANNEL, audio_callback, NULL);

    while (!exit_requested) {
        unsigned int pressed;
        unsigned int dpad_actions = 0;
        sceCtrlPeekBufferPositive(&pad, 1);
        apply_midi_input();
        pressed = pad.Buttons & ~previous_buttons;
        if (dpad_repeats(pad.Buttons, pressed, PSP_CTRL_UP, &up_held_frames)) {
            dpad_actions |= PSP_CTRL_UP;
        }
        if (dpad_repeats(pad.Buttons, pressed, PSP_CTRL_DOWN, &down_held_frames)) {
            dpad_actions |= PSP_CTRL_DOWN;
        }
        if (dpad_repeats(pad.Buttons, pressed, PSP_CTRL_LEFT, &left_held_frames)) {
            dpad_actions |= PSP_CTRL_LEFT;
        }
        if (dpad_repeats(pad.Buttons, pressed, PSP_CTRL_RIGHT, &right_held_frames)) {
            dpad_actions |= PSP_CTRL_RIGHT;
        }

        if (pressed & PSP_CTRL_SELECT) {
            if (pad.Buttons & PSP_CTRL_LTRIGGER) {
                edit_mode = EDIT_PREFERENCES;
            } else if (pad.Buttons & PSP_CTRL_RTRIGGER) {
                edit_mode = EDIT_SONG;
            } else {
                edit_mode = edit_mode == EDIT_SEQUENCE ? EDIT_PARAMETERS :
                             edit_mode == EDIT_PARAMETERS ? EDIT_PREFERENCES :
                             edit_mode == EDIT_PREFERENCES ? EDIT_SONG : EDIT_SEQUENCE;
            }
            parameter_editing = 0;
            lfo_editing = 0;
            song_editing = 0;
        }
        if (edit_mode == EDIT_PREFERENCES) {
            int analog_x = analog_direction(pad.Lx, 0);
            int analog_y = analog_direction(pad.Ly, 1);

            if (pressed & PSP_CTRL_CROSS) {
                lfo_editing = !lfo_editing;
            } else if (lfo_editing) {
                if (dpad_actions & PSP_CTRL_UP) adjust_preference(1);
                if (dpad_actions & PSP_CTRL_DOWN) adjust_preference(-1);
                if (analog_repeats(analog_x, &previous_analog_x_direction,
                                   &analog_x_held_frames)) {
                    if (preference_cursor == 0) adjust_lfo_rate(analog_x * 5);
                    else if (preference_cursor == 4) adjust_preference(analog_x);
                }
                if (analog_repeats(analog_y, &previous_analog_y_direction,
                                   &analog_y_held_frames)) {
                    if (preference_cursor == 1) adjust_lfo_depth(analog_y * 5);
                }
            } else {
                previous_analog_x_direction = 0;
                previous_analog_y_direction = 0;
                analog_x_held_frames = 0;
                analog_y_held_frames = 0;
                if (dpad_actions & PSP_CTRL_LEFT) {
                    if (preference_cursor == 0) preference_cursor = 2;
                    else if (preference_cursor == 3) preference_cursor = 4;
                    else --preference_cursor;
                }
                if (dpad_actions & PSP_CTRL_RIGHT) {
                    if (preference_cursor == 2) preference_cursor = 0;
                    else if (preference_cursor == 4) preference_cursor = 3;
                    else ++preference_cursor;
                }
                if (dpad_actions & PSP_CTRL_UP) {
                    if (preference_cursor == 3) preference_cursor = 0;
                    else if (preference_cursor == 4) preference_cursor = 1;
                }
                if (dpad_actions & PSP_CTRL_DOWN) {
                    if (preference_cursor == 0) preference_cursor = 3;
                    else if (preference_cursor == 1) preference_cursor = 4;
                }
            }
            if (pressed & PSP_CTRL_TRIANGLE) midi_learn_armed = 1;
        } else if (edit_mode == EDIT_SEQUENCE) {
            unsigned int sequence_modifier =
                pad.Buttons & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER);

            previous_analog_x_direction = 0;
            previous_analog_y_direction = 0;
            analog_x_held_frames = 0;
            analog_y_held_frames = 0;
            {
                int analog_step_direction = analog_stick_direction(pad.Lx, pad.Ly);
                if (analog_repeats(analog_step_direction,
                                   &previous_analog_adjust_direction,
                                   &analog_adjust_held_frames)) {
                    int note = (int)sequence[cursor].note + analog_step_direction * 12;
                    if (note < 24) note = 24;
                    if (note > 72) note = 72;
                    if (note != sequence[cursor].note) {
                        sequence[cursor].note = (uint8_t)note;
                        pattern_saved = 0;
                        sequence_dirty = 1;
                    }
                }
            }
            if (!sequence_modifier && (dpad_actions & PSP_CTRL_LEFT)) {
                cursor = (cursor + 15) % 16;
            }
            if (!sequence_modifier && (dpad_actions & PSP_CTRL_RIGHT)) {
                cursor = (cursor + 1) % 16;
            }
            if (!sequence_modifier && (dpad_actions & PSP_CTRL_UP)
                && sequence[cursor].note < 72) {
                ++sequence[cursor].note;
                pattern_saved = 0;
                sequence_dirty = 1;
            }
            if (!sequence_modifier && (dpad_actions & PSP_CTRL_DOWN)
                && sequence[cursor].note > 24) {
                --sequence[cursor].note;
                pattern_saved = 0;
                sequence_dirty = 1;
            }
            if ((pressed & PSP_CTRL_CROSS) && !(pad.Buttons & PSP_CTRL_RTRIGGER)) {
                sequence[cursor].active = !sequence[cursor].active;
                pattern_saved = 0;
                sequence_dirty = 1;
            }
            if ((pressed & PSP_CTRL_TRIANGLE) && !sequence_modifier) {
                sequence[cursor].accent = !sequence[cursor].accent;
                pattern_saved = 0;
                sequence_dirty = 1;
            }
            if ((pressed & PSP_CTRL_CIRCLE) && !sequence_modifier) {
                sequence[cursor].slide = !sequence[cursor].slide;
                pattern_saved = 0;
                sequence_dirty = 1;
            }
            if ((pressed & PSP_CTRL_CROSS) && (pad.Buttons & PSP_CTRL_RTRIGGER)) {
                sequence[cursor].ratchet_count = sequence[cursor].ratchet_count >= 4U
                    ? 1U : sequence[cursor].ratchet_count + 1U;
                pattern_saved = 0;
                sequence_dirty = 1;
            }
            if ((pressed & PSP_CTRL_CIRCLE) && (pad.Buttons & PSP_CTRL_RTRIGGER)) {
                sequence[cursor].gate = sequence[cursor].gate >= 100U
                    ? 25U : sequence[cursor].gate + 25U;
                pattern_saved = 0;
                sequence_dirty = 1;
            }
            if ((pressed & PSP_CTRL_TRIANGLE) && (pad.Buttons & PSP_CTRL_RTRIGGER)) {
                sequence[cursor].probability = sequence[cursor].probability >= 100U
                    ? 25U : sequence[cursor].probability + 25U;
                pattern_saved = 0;
                sequence_dirty = 1;
            }
            if ((pressed & PSP_CTRL_SQUARE) && (pad.Buttons & PSP_CTRL_LTRIGGER)) {
                mutate_sequence(25U);
                pattern_saved = 0;
                sequence_dirty = 1;
            } else if (pressed & PSP_CTRL_SQUARE) {
                randomize_sequence();
                pattern_saved = 0;
                sequence_dirty = 1;
            }
            if ((dpad_actions & PSP_CTRL_UP) && (pad.Buttons & PSP_CTRL_RTRIGGER)
                && !(pad.Buttons & PSP_CTRL_LTRIGGER)) {
                adjust_performance_key(1);
                sequence_dirty = 1;
            }
            if ((dpad_actions & PSP_CTRL_DOWN) && (pad.Buttons & PSP_CTRL_RTRIGGER)
                && !(pad.Buttons & PSP_CTRL_LTRIGGER)) {
                adjust_performance_key(-1);
                sequence_dirty = 1;
            }
            if ((pressed & PSP_CTRL_TRIANGLE) && (pad.Buttons & PSP_CTRL_LTRIGGER)) {
                storage_directory();
                if (save_pattern_slot(pattern_slot) == 0) pattern_saved = 1;
            }
            if ((pressed & PSP_CTRL_CIRCLE) && (pad.Buttons & PSP_CTRL_LTRIGGER)) {
                if (load_pattern_slot(pattern_slot) == 0) { pattern_saved = 1; sequence_dirty = 1; }
            }
            if ((dpad_actions & PSP_CTRL_LEFT) && (pad.Buttons & PSP_CTRL_RTRIGGER)) {
                if (pad.Buttons & PSP_CTRL_LTRIGGER) {
                    if (select_pattern_slot(-1) == 0) sequence_dirty = 1;
                } else {
                    adjust_performance_scale(-1);
                    sequence_dirty = 1;
                }
            }
            if ((dpad_actions & PSP_CTRL_RIGHT) && (pad.Buttons & PSP_CTRL_RTRIGGER)) {
                if (pad.Buttons & PSP_CTRL_LTRIGGER) {
                    if (select_pattern_slot(1) == 0) sequence_dirty = 1;
                } else {
                    adjust_performance_scale(1);
                    sequence_dirty = 1;
                }
            }
            if ((dpad_actions & PSP_CTRL_UP) && (pad.Buttons & PSP_CTRL_LTRIGGER)
                && !(pad.Buttons & PSP_CTRL_RTRIGGER)
                && sequence[cursor].note <= 60U) {
                sequence[cursor].note += 12U;
                pattern_saved = 0;
                sequence_dirty = 1;
            }
            if ((dpad_actions & PSP_CTRL_DOWN) && (pad.Buttons & PSP_CTRL_LTRIGGER)
                && !(pad.Buttons & PSP_CTRL_RTRIGGER)
                && sequence[cursor].note >= 36U) {
                sequence[cursor].note -= 12U;
                pattern_saved = 0;
                sequence_dirty = 1;
            }
        } else if (edit_mode == EDIT_SONG) {
            int analog_song_direction = analog_stick_direction(pad.Lx, pad.Ly);
            int analog_song_new_direction =
                analog_song_direction != 0
                && analog_song_direction != previous_analog_adjust_direction;

            previous_analog_x_direction = 0;
            previous_analog_y_direction = 0;
            analog_x_held_frames = 0;
            analog_y_held_frames = 0;
            if (pressed & PSP_CTRL_CROSS) {
                song_editing = !song_editing;
            } else if (song_editing) {
                /* A switch toggles once per press; numeric fields may repeat. */
                if (song_field == SONG_FIELD_LOOP_ENABLE) {
                    if (pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
                        adjust_song_field(song_field, song_cursor, 1);
                    }
                } else {
                    if (dpad_actions & PSP_CTRL_UP) adjust_song_field(song_field, song_cursor, 1);
                    if (dpad_actions & PSP_CTRL_DOWN) adjust_song_field(song_field, song_cursor, -1);
                }
                if (analog_repeats(analog_song_direction,
                                   &previous_analog_adjust_direction,
                                   &analog_adjust_held_frames)) {
                    if (song_field != SONG_FIELD_LOOP_ENABLE || analog_song_new_direction) {
                        adjust_song_field(song_field, song_cursor, analog_song_direction);
                    }
                }
            } else {
                previous_analog_adjust_direction = 0;
                analog_adjust_held_frames = 0U;
                if (song_field == SONG_FIELD_PATTERN || song_field == SONG_FIELD_REPEATS) {
                    if (dpad_actions & PSP_CTRL_LEFT) song_field = SONG_FIELD_PATTERN;
                    if (dpad_actions & PSP_CTRL_RIGHT) song_field = SONG_FIELD_REPEATS;
                    if ((dpad_actions & PSP_CTRL_UP) && song_cursor > 0U) --song_cursor;
                    if (dpad_actions & PSP_CTRL_DOWN) {
                        if (song_cursor + 1U < song.length) ++song_cursor;
                        else song_field = SONG_FIELD_LOOP_ENABLE;
                    }
                } else {
                    if (dpad_actions & PSP_CTRL_LEFT) {
                        song_field = song_field == SONG_FIELD_LOOP_ENABLE
                                   ? SONG_FIELD_LOOP_END
                                   : (SongField)(song_field - 1);
                    }
                    if (dpad_actions & PSP_CTRL_RIGHT) {
                        song_field = song_field == SONG_FIELD_LOOP_END
                                   ? SONG_FIELD_LOOP_ENABLE
                                   : (SongField)(song_field + 1);
                    }
                    if (dpad_actions & PSP_CTRL_UP) {
                        song_field = SONG_FIELD_REPEATS;
                        if (song.length > 0U) song_cursor = song.length - 1U;
                    }
                }

                if (pressed & PSP_CTRL_CIRCLE) {
                    delete_song_entry(song_cursor);
                    if (song_cursor >= song.length) song_cursor = song.length - 1U;
                }
                if (pressed & PSP_CTRL_SQUARE) {
                    insert_song_entry(song_cursor + 1U);
                    if (song_cursor + 1U < song.length) ++song_cursor;
                }
                if ((pressed & PSP_CTRL_TRIANGLE) && (pad.Buttons & PSP_CTRL_LTRIGGER)) {
                    (void)load_song_file();
                    song_cursor = song.length == 0U ? 0U : song_cursor % song.length;
                    song_entry_index = song.length == 0U ? 0U : song_entry_index % song.length;
                } else if (pressed & PSP_CTRL_TRIANGLE) {
                    (void)save_song_file();
                }
            }
            if (song.length > 0U && song_entry_index >= song.length) {
                song_entry_index = song.length - 1U;
            }
        } else {
            int analog_parameter_direction;
            int analog_parameter_new_direction;

            previous_analog_x_direction = 0;
            previous_analog_y_direction = 0;
            analog_x_held_frames = 0;
            analog_y_held_frames = 0;
            analog_parameter_direction = analog_stick_direction(pad.Lx, pad.Ly);
            analog_parameter_new_direction =
                analog_parameter_direction != 0
                && analog_parameter_direction != previous_analog_adjust_direction;
            if (analog_repeats(analog_parameter_direction,
                               &previous_analog_adjust_direction,
                               &analog_adjust_held_frames)) {
                int coarse_step = (parameter == PARAM_WAVE
                                   || parameter == PARAM_MIDI_SEND) ? 1 : 5;
                if ((parameter != PARAM_WAVE && parameter != PARAM_MIDI_SEND)
                    || analog_parameter_new_direction) {
                    adjust_parameter(parameter, analog_parameter_direction * coarse_step);
                }
            }
            if (pressed & PSP_CTRL_CROSS) {
                parameter_editing = !parameter_editing;
            } else if (parameter_editing) {
                int increment = (pad.Buttons & PSP_CTRL_LTRIGGER) ? 10 : 1;
                if (dpad_actions & PSP_CTRL_UP) adjust_parameter(parameter, increment);
                if (dpad_actions & PSP_CTRL_DOWN) adjust_parameter(parameter, -increment);
            } else {
                int index = (int)parameter;
                int row = index / 4;
                int column = index % 4;
                int row_width = 4;
                if (dpad_actions & PSP_CTRL_LEFT) column = (column + row_width - 1) % row_width;
                if (dpad_actions & PSP_CTRL_RIGHT) column = (column + 1) % row_width;
                if (dpad_actions & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
                    int row_direction = (dpad_actions & PSP_CTRL_UP) ? -1 : 1;
                    do {
                        row = (row + 3 + row_direction) % 3;
                    } while (row * 4 + column >= PARAM_COUNT);
                }
                parameter = (Parameter)(row * 4 + column);
            }
        }
        if (pressed & PSP_CTRL_NOTE) light_mode = !light_mode;
        if (pressed & PSP_CTRL_START) {
            if (pad.Buttons & PSP_CTRL_LTRIGGER) {
                song_mode = !song_mode;
                reset_song_playback();
                if (song_mode && song.length > 0U) {
                    if (activate_song_entry(song_entry_index, playing) == 0) {
                        sequence_dirty = 1;
                        if (playing) {
                            midi_clock_slave = 0;
                            transport_set_source(&transport, TRANSPORT_SOURCE_INTERNAL);
                            transport_start(&transport, &runtime_pattern, 1U);
                            if (midi_available) (void)MidiOutput_Start();
                        }
                    } else {
                        song_mode = 0;
                    }
                }
            } else if (pad.Buttons & PSP_CTRL_RTRIGGER) {
                edit_mode = EDIT_SONG;
                song_editing = 0;
                if (song_mode && playing) {
                    playing = 0;
                    previous_playhead = -1;
                    transport_stop(&transport);
                    if (midi_available) (void)MidiOutput_Stop();
                } else {
                    song_mode = 1;
                    reset_song_playback();
                    if (song.length > 0U
                        && activate_song_entry(song_entry_index, 0) == 0) {
                        sequence_dirty = 1;
                        playing = 1;
                        playhead = -1;
                        midi_clock_slave = 0;
                        transport_set_source(&transport, TRANSPORT_SOURCE_INTERNAL);
                        pattern_from_sequence(&runtime_pattern);
                        transport_set_pattern(&transport, &runtime_pattern);
                        transport_start(&transport, &runtime_pattern, 1U);
                        if (midi_available) (void)MidiOutput_Start();
                    } else {
                        song_mode = 0;
                        playing = 0;
                    }
                }
            } else {
                playing = !playing;
                if (playing) {
                    if (song_mode && song.length > 0U) {
                        reset_song_playback();
                        if (activate_song_entry(song_entry_index, 0) == 0) {
                            sequence_dirty = 1;
                        } else {
                            playing = 0;
                        }
                    }
                    if (playing) {
                        playhead = -1;
                        midi_clock_slave = 0;
                        transport_set_source(&transport, TRANSPORT_SOURCE_INTERNAL);
                        pattern_from_sequence(&runtime_pattern);
                        transport_set_pattern(&transport, &runtime_pattern);
                        transport_start(&transport, &runtime_pattern, 1U);
                        if (midi_available) (void)MidiOutput_Start();
                    }
                } else {
                    previous_playhead = -1;
                    transport_stop(&transport);
                    if (midi_available) (void)MidiOutput_Stop();
                }
            }
        }

        while (song_mode && playing && song_cycle_pending > 0U
               && song.length > 0U) {
            StorageSongEntry *entry = &song.entries[song_entry_index];
            unsigned int next_entry;
            --song_cycle_pending;
            if (++song_repeat_count >= (entry->repeats == 0U ? 1U : entry->repeats)) {
                song_repeat_count = 0U;
                next_entry = next_song_entry(song_entry_index);
                if (!song.loop_enabled && song_entry_index == song.length - 1U) {
                    playing = 0;
                    previous_playhead = -1;
                    transport_stop(&transport);
                    if (midi_available) (void)MidiOutput_Stop();
                } else {
                    song_entry_index = next_entry;
                    entry = &song.entries[song_entry_index];
                    if (entry->pattern_slot < STORAGE_PATTERN_SLOT_COUNT
                        && activate_song_entry(song_entry_index, 1) == 0) {
                        sequence_dirty = 1;
                    } else {
                        playing = 0;
                        previous_playhead = -1;
                        transport_stop(&transport);
                        if (midi_available) (void)MidiOutput_Stop();
                    }
                }
            }
        }
        previous_playhead = playhead;

        if (runtime_pattern_dirty) {
            sequence_dirty = 1;
            runtime_pattern_dirty = 0;
        }
        if (sequence_dirty) {
            pattern_from_sequence(&runtime_pattern);
            transport_set_pattern(&transport, &runtime_pattern);
            transport_set_bpm(&transport, runtime_pattern.settings.bpm);
            transport_set_swing(&transport, runtime_pattern.settings.swing);
            build_midi_sequence(&midi_sequence);
            midi_sequence_dirty = 1;
            sequence_dirty = 0;
        }
        if (midi_available && midi_sequence_dirty) {
            if (MidiOutput_SetSequence(&midi_sequence) >= 0) midi_sequence_dirty = 0;
        }
        if (midi_available && last_midi_bpm != bpm) {
            if (MidiOutput_SetBpm((unsigned int)bpm) >= 0) last_midi_bpm = bpm;
        }
        if (midi_available && last_midi_channel != midi_channel) {
            if (MidiOutput_SetChannel((unsigned int)midi_channel) >= 0) {
                last_midi_channel = midi_channel;
            }
        }
        if (midi_available && last_midi_send_enabled != midi_send_enabled) {
            if (MidiOutput_SetNotesEnabled(midi_send_enabled) >= 0) {
                last_midi_send_enabled = midi_send_enabled;
            }
        }
        if (midi_available && (ui_frame % 15U) == 0U) {
            int was_linked = midi_snapshot.link_established;
            if (MidiOutput_GetSnapshot(&midi_snapshot) >= 0
                && playing && midi_snapshot.link_established
                && (!was_linked || !midi_snapshot.running)) {
                (void)MidiOutput_Start();
                (void)MidiOutput_GetSnapshot(&midi_snapshot);
            }
        }

        previous_buttons = pad.Buttons;
        begin_gu_frame();
        draw_ui(cursor, parameter, edit_mode, parameter_editing, lfo_editing, song_editing,
                ((ui_frame / 20U) & 1U) == 0U, song_cursor, song_field);
        present_frame();
        ++ui_frame;
    }

    playing = 0;
    /* Also retries cleanup after a partially failed USB initialization. */
    finish_midi_shutdown();
    pspAudioEnd();
    if (gu_ready) sceGuTerm();
    sceKernelExitGame();

    return 0;
}
