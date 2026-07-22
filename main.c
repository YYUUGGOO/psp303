#include <pspaudio.h>
#include <pspaudiolib.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <pspsync.h>

#include "midi_output.h"

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

typedef struct {
    uint8_t note;
    uint8_t active;
    uint8_t accent;
    uint8_t slide;
} Step;

typedef struct {
    int16_t left;
    int16_t right;
} StereoSample;

typedef enum {
    PARAM_BPM,
    PARAM_CUTOFF,
    PARAM_RESONANCE,
    PARAM_ENVELOPE,
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
    EDIT_PARAMETERS
} EditMode;

static volatile Step sequence[SEQUENCE_LENGTH];
static volatile int playing = 1;
static volatile int playhead = -1;
static volatile int bpm = 128;
static volatile int cutoff = 34;
static volatile int resonance = 72;
static volatile int envelope_mod = 72;
static volatile int decay = 48;
static volatile int square_wave = 0;
static volatile int drive = 28;
static volatile int delay_amount = 22;
static volatile PSPSyncProfile sync_profile = PSPSYNC_PROFILE_OFF;
static volatile int midi_channel = 1;
static volatile int midi_send_enabled = 1;
static volatile int light_mode = 0;

static float phase;
static float frequency = 110.0f;
static float target_frequency = 110.0f;
static float filter_stage[4];
static float filter_input_history;
static float filter_envelope;
static float amp_envelope;
static float accent_gain = 1.0f;
static float delay_buffer[44100];
static int delay_write_index;
static uint32_t random_state;
static uint32_t *framebuffer;
static uint32_t *framebuffers[2];
static int draw_buffer_index = 1;
static PSPSyncClock sync_clock;
static volatile int exit_requested;
static MidiOutputSnapshot midi_snapshot;
static int midi_available;
static int midi_init_result;

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
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
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
    static const uint8_t minor_scale[] = {0, 3, 5, 7, 10, 12};
    int i;
    for (i = 0; i < SEQUENCE_LENGTH; ++i) {
        uint32_t value = next_random();
        sequence[i].active = (value % 100) < 78;
        sequence[i].note = 36 + minor_scale[(value >> 8) % 6];
        sequence[i].accent = sequence[i].active && (((value >> 16) % 100) < 24);
        sequence[i].slide = sequence[i].active && (((value >> 24) % 100) < 18);
    }
    sequence[0].active = 1;
    sequence[0].note = 36;
    sequence[0].accent = 1;
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
    }
}

static void build_midi_sequence(MidiOutputSequence *midi_sequence)
{
    int i;

    for (i = 0; i < SEQUENCE_LENGTH; ++i) {
        midi_sequence->steps[i].note = sequence[i].note;
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

static void trigger_step(int index)
{
    int previous = (index + SEQUENCE_LENGTH - 1) % SEQUENCE_LENGTH;
    Step step;
    step.note = sequence[index].note;
    step.active = sequence[index].active;
    step.accent = sequence[index].accent;
    step.slide = sequence[index].slide;

    if (!step.active) {
        return;
    }

    target_frequency = midi_frequency(step.note);
    if (!(sequence[previous].active && sequence[previous].slide)) {
        frequency = target_frequency;
        phase = 0.0f;
    }
    filter_envelope = 1.0f;
    amp_envelope = 1.0f;
    accent_gain = step.accent ? 1.45f : 1.0f;
}

static void audio_callback(void *buffer, unsigned int length, void *userdata)
{
    StereoSample *output = (StereoSample *)buffer;
    int local_bpm = bpm;
    int local_delay = delay_amount;
    int delay_samples = ((int)SAMPLE_RATE * 45) / local_bpm;
    float envelope_seconds = 0.06f + ((float)decay / 100.0f) * 0.62f;
    float envelope_decay = expf(-1.0f / (envelope_seconds * SAMPLE_RATE));
    float resonance_amount = (float)resonance / 100.0f;
    float base_cutoff_hz = 65.0f * powf(2.0f, (float)cutoff / 14.3f);
    float drive_gain = 1.0f + ((float)drive / 100.0f) * 9.0f;
    float delay_wet = ((float)local_delay / 100.0f) * 0.48f;
    float delay_feedback = ((float)local_delay / 100.0f) * 0.68f;
    unsigned int i;
    (void)userdata;

    pspSyncSetTempo(&sync_clock, (uint32_t)local_bpm * 1000U);
    if (sync_clock.profile != sync_profile) pspSyncSetProfile(&sync_clock, sync_profile);
    if (playing && !sync_clock.running) pspSyncStart(&sync_clock, 1);
    if (!playing && sync_clock.running) pspSyncStop(&sync_clock);

    for (i = 0; i < length; ++i) {
        PSPSyncFrame sync = pspSyncNext(&sync_clock);
        float sample = 0.0f;

        if (playing) {
            if (sync.tick) {
                playhead = (playhead + 1) % SEQUENCE_LENGTH;
                trigger_step(playhead);
            }

            frequency += (target_frequency - frequency) * 0.0012f;
            phase += frequency / SAMPLE_RATE;
            if (phase >= 1.0f) phase -= 1.0f;

            if (sequence[playhead].active) {
                float oscillator = square_wave ? (phase < 0.5f ? 0.72f : -0.72f)
                                               : (phase * 2.0f - 1.0f);
                float envelope_scale = 1.0f + filter_envelope
                                     * ((float)envelope_mod / 100.0f) * 7.0f;
                float cutoff_hz = clampf(base_cutoff_hz * envelope_scale, 40.0f, 11000.0f);
                float normalized = clampf(2.0f * cutoff_hz / SAMPLE_RATE, 0.002f, 0.92f);
                float pole = normalized * (1.8f - 0.8f * normalized);
                float feedback_coefficient = 2.0f * pole - 1.0f;
                float t1 = (1.0f - pole) * 1.386249f;
                float t2 = 12.0f + t1 * t1;
                float feedback = resonance_amount * (t2 + 6.0f * t1) / (t2 - 6.0f * t1);
                float input = oscillator - feedback * filter_stage[3];
                float old_stage0 = filter_stage[0];
                float old_stage1 = filter_stage[1];
                float old_stage2 = filter_stage[2];

                /* Four-pole Stilson/Smith ladder with nonlinear final-stage saturation. */
                filter_stage[0] = input * pole + filter_input_history * pole
                                - feedback_coefficient * filter_stage[0];
                filter_stage[1] = filter_stage[0] * pole + old_stage0 * pole
                                - feedback_coefficient * filter_stage[1];
                filter_stage[2] = filter_stage[1] * pole + old_stage1 * pole
                                - feedback_coefficient * filter_stage[2];
                filter_stage[3] = filter_stage[2] * pole + old_stage2 * pole
                                - feedback_coefficient * filter_stage[3];
                filter_stage[3] = clampf(filter_stage[3], -1.5f, 1.5f);
                filter_stage[3] -= (filter_stage[3] * filter_stage[3] * filter_stage[3]) * (1.0f / 6.0f);
                filter_input_history = input;

                if (!sequence[playhead].slide && sync_clock.phase_q32 > (3ULL << 30)) {
                    amp_envelope *= 0.992f;
                }
                sample = filter_stage[3] * amp_envelope * accent_gain * 0.72f;
                filter_envelope *= envelope_decay;
                amp_envelope *= 0.99996f;
            } else {
                amp_envelope *= 0.985f;
                sample = filter_stage[3] * amp_envelope * 0.4f;
            }
        } else {
            amp_envelope *= 0.98f;
            sample = filter_stage[3] * amp_envelope * 0.25f;
        }

        if (drive > 0) {
            float driven = sample * drive_gain;
            sample = driven / (1.0f + fabsf(driven));
        }

        {
            int read_index = delay_write_index - delay_samples;
            float delayed;
            while (read_index < 0) read_index += (int)(sizeof(delay_buffer) / sizeof(delay_buffer[0]));
            delayed = delay_buffer[read_index];
            delay_buffer[delay_write_index] = clampf(sample + delayed * delay_feedback, -1.0f, 1.0f);
            delay_write_index = (delay_write_index + 1) % (int)(sizeof(delay_buffer) / sizeof(delay_buffer[0]));
            sample = sample * (1.0f - delay_wet * 0.35f) + delayed * delay_wet;
        }

        sample = clampf(sample, -1.0f, 1.0f);
        output[i].right = (int16_t)(sample * 26000.0f);
        output[i].left = sync_profile == PSPSYNC_PROFILE_OFF ? output[i].right : sync.pulse;
    }
}

static const char *note_name(int note)
{
    static const char *names[] = {"C ", "C#", "D ", "D#", "E ", "F ",
                                  "F#", "G ", "G#", "A ", "A#", "B "};
    static char text[5];
    snprintf(text, sizeof(text), "%s%d", names[note % 12], note / 12 - 1);
    return text;
}

static const char *pitch_name(int note)
{
    static const char *names[] = {"C ", "C#", "D ", "D#", "E ", "F ",
                                  "F#", "G ", "G#", "A ", "A#", "B "};
    return names[note % 12];
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
    int i;
    pspDebugScreenSetTextColor(foreground);
    pspDebugScreenSetBackColor(background);
    for (i = 0; text[i] != '\0'; ++i) {
        pspDebugScreenPutChar(x + i * 8, y, foreground, (uint8_t)text[i]);
    }
}

static int parameter_value(Parameter parameter)
{
    switch (parameter) {
        case PARAM_BPM: return bpm;
        case PARAM_CUTOFF: return cutoff;
        case PARAM_RESONANCE: return resonance;
        case PARAM_ENVELOPE: return envelope_mod;
        case PARAM_DECAY: return decay;
        case PARAM_WAVE: return square_wave ? 100 : 0;
        case PARAM_DRIVE: return drive;
        case PARAM_DELAY: return delay_amount;
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
        "BPM", "CUTOFF", "RESONANCE", "ENV MOD", "DECAY",
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

static void draw_ui(int cursor, Parameter parameter, EditMode edit_mode,
                    int parameter_editing, int blink_on)
{
    int i;
    char text[64];
    uint32_t background = theme_background();
    uint32_t foreground = theme_foreground();

    fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, background);
    outline_rect(4, 4, 472, 264, 2, foreground);

    outline_rect(8, 8, 48, 28, 2, foreground);
    draw_text(14, 16, foreground, background, edit_mode == EDIT_SEQUENCE ? "SEQ" : "PAR");
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

    if (edit_mode == EDIT_PARAMETERS) {
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
    } else {
        snprintf(text, sizeof(text), "CUT %d RES %d ENV %d DEC %d DRV %d DLY %d",
                 cutoff, resonance, envelope_mod, decay, drive, delay_amount);
        draw_text(8, 54, foreground, background, text);
        snprintf(text, sizeof(text), "WAVE %s  SYNC %s  MIDI CH %d OUT %s",
                 square_wave ? "SQUARE" : "SAW",
                 pspSyncProfileName(sync_profile), midi_channel,
                 midi_send_enabled ? "ON" : "OFF");
        draw_text(8, 74, foreground, background, text);
        fill_rect(8, 94, 460, 2, foreground);
        snprintf(text, sizeof(text), "STEP %02d  %s  G:%d A:%d S:%d",
                 cursor + 1, note_name(sequence[cursor].note), sequence[cursor].active,
                 sequence[cursor].accent, sequence[cursor].slide);
        draw_text(8, 102, foreground, background, text);
        draw_text(400, 102, foreground, background, playing ? "RUN" : "STOP");
        for (i = 0; i < SEQUENCE_LENGTH; ++i) draw_step_key(i, cursor);
        fill_rect(8, 194, 460, 2, foreground);
        draw_text(8, 204, foreground, background, "X TRIG  TRI ACC  O SLD  SQ RANDOM  START RUN");
        draw_text(8, 238, foreground, background, "DPAD STEP/NOTE  SELECT PARAMS  NOTE THEME");
    }
}

static void present_frame(void)
{
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
            delay_amount += direction;
            if (delay_amount < 0) delay_amount = 0;
            if (delay_amount > 100) delay_amount = 100;
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
    int parameter_editing = 0;
    unsigned int ui_frame = 0;
    unsigned int up_held_frames = 0;
    unsigned int down_held_frames = 0;
    unsigned int left_held_frames = 0;
    unsigned int right_held_frames = 0;
    PSPSyncConfig sync_config = {
        .sample_rate = 44100,
        .bpm_milli = 128000,
        .ticks_per_quarter = 4,
        .pulse_level = 29490
    };
    MidiOutputSequence midi_sequence;
    char usb_driver_path[256];
    int sequence_dirty = 0;
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
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    random_state = sceKernelGetSystemTimeLow();
    if (random_state == 0) random_state = 0x303303u;
    load_default_sequence();
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
            edit_mode = edit_mode == EDIT_SEQUENCE ? EDIT_PARAMETERS : EDIT_SEQUENCE;
            parameter_editing = 0;
        }
        if (edit_mode == EDIT_SEQUENCE) {
            if (dpad_actions & PSP_CTRL_LEFT) cursor = (cursor + 15) % 16;
            if (dpad_actions & PSP_CTRL_RIGHT) cursor = (cursor + 1) % 16;
            if ((dpad_actions & PSP_CTRL_UP) && sequence[cursor].note < 72) {
                ++sequence[cursor].note;
                sequence_dirty = 1;
            }
            if ((dpad_actions & PSP_CTRL_DOWN) && sequence[cursor].note > 24) {
                --sequence[cursor].note;
                sequence_dirty = 1;
            }
            if (pressed & PSP_CTRL_CROSS) {
                sequence[cursor].active = !sequence[cursor].active;
                sequence_dirty = 1;
            }
            if (pressed & PSP_CTRL_TRIANGLE) {
                sequence[cursor].accent = !sequence[cursor].accent;
                sequence_dirty = 1;
            }
            if (pressed & PSP_CTRL_CIRCLE) {
                sequence[cursor].slide = !sequence[cursor].slide;
                sequence_dirty = 1;
            }
            if (pressed & PSP_CTRL_SQUARE) {
                randomize_sequence();
                sequence_dirty = 1;
            }
        } else {
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
                int row_width = row == 2 ? 3 : 4;
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
            playing = !playing;
            if (playing) {
                playhead = -1;
                if (midi_available) (void)MidiOutput_Start();
            } else if (midi_available) {
                (void)MidiOutput_Stop();
            }
        }

        if (midi_available && sequence_dirty) {
            build_midi_sequence(&midi_sequence);
            if (MidiOutput_SetSequence(&midi_sequence) >= 0) sequence_dirty = 0;
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
        draw_ui(cursor, parameter, edit_mode, parameter_editing,
                ((ui_frame / 20U) & 1U) == 0U);
        /* Queue the complete back buffer, then wait until it becomes visible. */
        present_frame();
        ++ui_frame;
    }

    playing = 0;
    /* Also retries cleanup after a partially failed USB initialization. */
    finish_midi_shutdown();
    pspAudioEnd();
    sceKernelExitGame();

    return 0;
}
