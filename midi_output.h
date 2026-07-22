#ifndef PSP303_MIDI_OUTPUT_H
#define PSP303_MIDI_OUTPUT_H

#include <stdint.h>

#define MIDI_OUTPUT_STEP_COUNT 16U

typedef struct MidiOutputStep {
    uint8_t note;
    uint8_t velocity;
    uint8_t length_clocks;
    uint8_t enabled;
} MidiOutputStep;

typedef struct MidiOutputSequence {
    MidiOutputStep steps[MIDI_OUTPUT_STEP_COUNT];
} MidiOutputSequence;

typedef struct MidiOutputSnapshot {
    int initialized;
    int running;
    int last_result;
    unsigned int bpm;
    unsigned int ticks_sent;
    unsigned int dropped_ticks;
    unsigned int sequencer_step;
    unsigned int channel;
    int notes_enabled;
    uint8_t usb_active;
    uint8_t cable_connected;
    uint8_t link_established;
} MidiOutputSnapshot;

int MidiOutput_Init(
    const char *driver_path,
    unsigned int bpm,
    const MidiOutputSequence *sequence);
int MidiOutput_SetBpm(unsigned int bpm);
int MidiOutput_SetChannel(unsigned int channel);
int MidiOutput_SetNotesEnabled(int enabled);
int MidiOutput_SetSequence(const MidiOutputSequence *sequence);
int MidiOutput_Start(void);
int MidiOutput_Stop(void);
int MidiOutput_GetSnapshot(MidiOutputSnapshot *snapshot);
int MidiOutput_Shutdown(void);

#endif
