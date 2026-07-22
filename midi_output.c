#include "midi_output.h"

#include <psp_usb_midi.h>
#include <pspkernel.h>
#include <pspkerror.h>

#include <stddef.h>
#include <string.h>

#define MIDI_CLOCK_PPQN 24U
#define MIDI_CLOCKS_PER_STEP 6U
#define MIDI_COMMAND_TIMEOUT_US 1000000U
#define MIDI_THREAD_PRIORITY 0x19
#define MIDI_THREAD_STACK_SIZE 0x1000
#define PSP_ERROR_AS_INT(value) ((int)(unsigned int)(value))

typedef enum MidiCommand {
    MIDI_COMMAND_NONE = 0,
    MIDI_COMMAND_START,
    MIDI_COMMAND_STOP,
    MIDI_COMMAND_SET_BPM,
    MIDI_COMMAND_SET_CHANNEL,
    MIDI_COMMAND_SET_NOTES_ENABLED,
    MIDI_COMMAND_SET_SEQUENCE,
    MIDI_COMMAND_EXIT
} MidiCommand;

typedef struct MidiSchedule {
    uint32_t interval_us;
    uint32_t next_tick_us;
    uint32_t dropped_ticks;
} MidiSchedule;

typedef struct MidiThreadState {
    SceUID thread;
    SceUID command_sema;
    SceUID acknowledgement_sema;
    SceUID state_sema;
    SceUID control_sema;
    MidiCommand command;
    unsigned int requested_bpm;
    unsigned int requested_channel;
    int requested_notes_enabled;
    MidiOutputSequence requested_sequence;
    MidiOutputSnapshot snapshot;
} MidiThreadState;

static MidiThreadState g_midi = {
    .thread = -1,
    .command_sema = -1,
    .acknowledgement_sema = -1,
    .state_sema = -1,
    .control_sema = -1,
    .command = MIDI_COMMAND_NONE
};

static int lock_sema(SceUID sema)
{
    return sema < 0
        ? PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT)
        : sceKernelWaitSema(sema, 1, NULL);
}

static int unlock_sema(SceUID sema, int operation_result)
{
    int result = sceKernelSignalSema(sema, 1);
    return operation_result < 0 ? operation_result : result;
}

static uint32_t clock_interval_us(unsigned int bpm)
{
    return bpm == 0U ? 0U : 60000000U / (bpm * MIDI_CLOCK_PPQN);
}

static void schedule_start(MidiSchedule *schedule, uint32_t now_us, unsigned int bpm)
{
    schedule->interval_us = clock_interval_us(bpm);
    schedule->next_tick_us = now_us + schedule->interval_us;
}

static uint32_t schedule_wait_us(const MidiSchedule *schedule, uint32_t now_us)
{
    int32_t remaining = (int32_t)(schedule->next_tick_us - now_us);
    return remaining > 0 ? (uint32_t)remaining : 0U;
}

static void schedule_advance(MidiSchedule *schedule, uint32_t now_us)
{
    schedule->next_tick_us += schedule->interval_us;
    if ((int32_t)(now_us - schedule->next_tick_us) >= 0) {
        uint32_t late_us = now_us - schedule->next_tick_us;
        uint32_t missed = late_us / schedule->interval_us + 1U;

        schedule->next_tick_us += missed * schedule->interval_us;
        schedule->dropped_ticks += missed;
    }
}

static void set_event(
    UsbMidiEvent *event,
    uint32_t timestamp_us,
    uint8_t status,
    uint8_t data1,
    uint8_t data2)
{
    event->timestamp_us = timestamp_us;
    event->cable = 0U;
    event->status = status;
    event->data1 = data1;
    event->data2 = data2;
}

static int write_realtime(uint8_t status, uint32_t timestamp_us)
{
    UsbMidiEvent event;

    set_event(&event, timestamp_us, status, 0U, 0U);
    return UsbMidi_Write(&event, 1);
}

static int write_step_start(
    const MidiOutputSequence *sequence,
    unsigned int step_index,
    uint32_t transport_tick,
    uint32_t timestamp_us,
    uint8_t channel_status,
    uint8_t *active_note,
    uint8_t *note_active,
    uint32_t *note_off_tick)
{
    const MidiOutputStep *step = &sequence->steps[step_index];
    UsbMidiEvent events[2];
    int count = 0;
    int legato = *note_active != 0U
              && (int32_t)(*note_off_tick - transport_tick) >= 0;

    /* For a slide, start the new pitch before releasing the old one. */
    if (legato && step->enabled != 0U) {
        set_event(
            &events[count++],
            timestamp_us,
            (uint8_t)(0x90U | channel_status),
            step->note,
            step->velocity);
    }
    if (*note_active != 0U) {
        set_event(
            &events[count++],
            timestamp_us,
            (uint8_t)(0x80U | channel_status),
            *active_note,
            0U);
    }
    if (!legato && step->enabled != 0U) {
        set_event(
            &events[count++],
            timestamp_us,
            (uint8_t)(0x90U | channel_status),
            step->note,
            step->velocity);
    }
    if (count > 0) {
        int result = UsbMidi_Write(events, count);

        if (result < 0) return result;
    }

    *note_active = step->enabled;
    if (step->enabled != 0U) {
        *active_note = step->note;
        *note_off_tick = transport_tick + step->length_clocks;
    }
    return 0;
}

static int write_note_off(uint8_t channel_status, uint8_t note, uint32_t timestamp_us)
{
    UsbMidiEvent event;

    set_event(
        &event,
        timestamp_us,
        (uint8_t)(0x80U | channel_status),
        note,
        0U);
    return UsbMidi_Write(&event, 1);
}

static int publish_snapshot(
    int running,
    int last_result,
    unsigned int bpm,
    unsigned int ticks_sent,
    unsigned int dropped_ticks,
    unsigned int step,
    unsigned int channel,
    int notes_enabled)
{
    int result = lock_sema(g_midi.state_sema);

    if (result < 0) return result;
    g_midi.snapshot.running = running;
    g_midi.snapshot.last_result = last_result;
    g_midi.snapshot.bpm = bpm;
    g_midi.snapshot.ticks_sent = ticks_sent;
    g_midi.snapshot.dropped_ticks = dropped_ticks;
    g_midi.snapshot.sequencer_step = step;
    g_midi.snapshot.channel = channel;
    g_midi.snapshot.notes_enabled = notes_enabled;
    return unlock_sema(g_midi.state_sema, 0);
}

static int midi_thread(SceSize args, void *argp)
{
    MidiSchedule schedule = {0U, 0U, 0U};
    MidiOutputSequence sequence = g_midi.requested_sequence;
    unsigned int bpm = g_midi.requested_bpm;
    unsigned int channel = g_midi.requested_channel;
    int notes_enabled = g_midi.requested_notes_enabled;
    unsigned int ticks_sent = 0U;
    unsigned int transport_tick = 0U;
    unsigned int step = 0U;
    uint8_t active_note = 0U;
    uint8_t note_active = 0U;
    uint32_t note_off_tick = 0U;
    int running = 0;
    int last_result = 0;

    (void)args;
    (void)argp;

    for (;;) {
        SceUInt timeout_us;
        SceUInt *timeout = NULL;
        int wait_result;

        if (running != 0) {
            timeout_us = schedule_wait_us(&schedule, sceKernelGetSystemTimeLow());
            timeout = &timeout_us;
        }
        wait_result = sceKernelWaitSema(g_midi.command_sema, 1, timeout);

        if (wait_result == 0) {
            MidiCommand command = g_midi.command;

            g_midi.command = MIDI_COMMAND_NONE;
            if (command == MIDI_COMMAND_START) {
                if (running == 0 && UsbMidi_IsConnected()) {
                    UsbMidiEvent events[2];
                    uint32_t now_us = sceKernelGetSystemTimeLow();
                    int count = 1;

                    transport_tick = 0U;
                    step = 0U;
                    note_active = 0U;
                    set_event(&events[0], now_us, 0xFAU, 0U, 0U);
                    if (notes_enabled != 0 && sequence.steps[0].enabled != 0U) {
                        set_event(
                            &events[count++],
                            now_us,
                            (uint8_t)(0x90U | (channel - 1U)),
                            sequence.steps[0].note,
                            sequence.steps[0].velocity);
                    }
                    last_result = UsbMidi_Write(events, count);
                    if (last_result == 0) {
                        running = 1;
                        if (notes_enabled != 0 && sequence.steps[0].enabled != 0U) {
                            active_note = sequence.steps[0].note;
                            note_active = 1U;
                            note_off_tick = sequence.steps[0].length_clocks;
                        }
                        schedule_start(&schedule, now_us, bpm);
                    }
                } else {
                    last_result = running != 0
                        ? 0
                        : PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
                }
            } else if (command == MIDI_COMMAND_STOP) {
                if (running != 0 && UsbMidi_IsConnected()) {
                    UsbMidiEvent events[2];
                    uint32_t now_us = sceKernelGetSystemTimeLow();
                    int count = 0;

                    if (note_active != 0U) {
                        set_event(
                            &events[count++],
                            now_us,
                            (uint8_t)(0x80U | (channel - 1U)),
                            active_note,
                            0U);
                    }
                    set_event(&events[count++], now_us, 0xFCU, 0U, 0U);
                    last_result = UsbMidi_Write(events, count);
                } else {
                    last_result = 0;
                }
                running = 0;
                note_active = 0U;
            } else if (command == MIDI_COMMAND_SET_BPM) {
                bpm = g_midi.requested_bpm;
                if (running != 0) {
                    schedule_start(&schedule, sceKernelGetSystemTimeLow(), bpm);
                }
                last_result = 0;
            } else if (command == MIDI_COMMAND_SET_CHANNEL) {
                unsigned int next_channel = g_midi.requested_channel;

                if (note_active != 0U && UsbMidi_IsConnected()) {
                    last_result = write_note_off(
                        (uint8_t)(channel - 1U), active_note,
                        sceKernelGetSystemTimeLow());
                    if (last_result == 0) note_active = 0U;
                } else {
                    last_result = 0;
                    note_active = 0U;
                }
                if (last_result == 0) channel = next_channel;
            } else if (command == MIDI_COMMAND_SET_NOTES_ENABLED) {
                int next_enabled = g_midi.requested_notes_enabled;

                if (next_enabled == 0 && note_active != 0U && UsbMidi_IsConnected()) {
                    last_result = write_note_off(
                        (uint8_t)(channel - 1U), active_note,
                        sceKernelGetSystemTimeLow());
                    if (last_result == 0) note_active = 0U;
                } else {
                    last_result = 0;
                    if (next_enabled == 0) note_active = 0U;
                }
                if (last_result == 0) notes_enabled = next_enabled;
            } else if (command == MIDI_COMMAND_SET_SEQUENCE) {
                sequence = g_midi.requested_sequence;
                last_result = 0;
            } else if (command == MIDI_COMMAND_EXIT) {
                running = 0;
                note_active = 0U;
                last_result = 0;
            } else {
                last_result = PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
            }

            (void)publish_snapshot(
                running, last_result, bpm, ticks_sent,
                schedule.dropped_ticks, step, channel, notes_enabled);
            (void)sceKernelSignalSema(g_midi.acknowledgement_sema, 1);
            if (command == MIDI_COMMAND_EXIT) return 0;
            continue;
        }

        if (running != 0
            && (wait_result == PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_WAIT_TIMEOUT)
                || schedule_wait_us(&schedule, sceKernelGetSystemTimeLow()) == 0U)) {
            uint32_t timestamp_us = schedule.next_tick_us;
            uint32_t now_us = sceKernelGetSystemTimeLow();

            if (schedule_wait_us(&schedule, now_us) != 0U) continue;
            last_result = write_realtime(0xF8U, timestamp_us);
            if (last_result == 0) {
                ++ticks_sent;
                ++transport_tick;

                if ((transport_tick % MIDI_CLOCKS_PER_STEP) == 0U) {
                    step = (transport_tick / MIDI_CLOCKS_PER_STEP)
                         % MIDI_OUTPUT_STEP_COUNT;
                    if (notes_enabled != 0) {
                        last_result = write_step_start(
                            &sequence, step, transport_tick, timestamp_us,
                            (uint8_t)(channel - 1U),
                            &active_note, &note_active, &note_off_tick);
                    }
                } else if (note_active != 0U
                           && (int32_t)(transport_tick - note_off_tick) >= 0) {
                    last_result = write_note_off(
                        (uint8_t)(channel - 1U), active_note, timestamp_us);
                    if (last_result == 0) note_active = 0U;
                }

                now_us = sceKernelGetSystemTimeLow();
                schedule_advance(&schedule, now_us);
            }
            if (last_result < 0) {
                running = 0;
                note_active = 0U;
            }
            (void)publish_snapshot(
                running, last_result, bpm, ticks_sent,
                schedule.dropped_ticks, step, channel, notes_enabled);
        } else if (wait_result < 0) {
            running = 0;
            note_active = 0U;
            last_result = wait_result;
            (void)publish_snapshot(
                running, last_result, bpm, ticks_sent,
                schedule.dropped_ticks, step, channel, notes_enabled);
        }
    }
}

static int submit_command(
    MidiCommand command,
    unsigned int bpm,
    const MidiOutputSequence *sequence)
{
    SceUInt timeout_us = MIDI_COMMAND_TIMEOUT_US;
    int result = lock_sema(g_midi.control_sema);

    if (result < 0) return result;
    if (command == MIDI_COMMAND_SET_BPM) {
        g_midi.requested_bpm = bpm;
    } else if (command == MIDI_COMMAND_SET_CHANNEL) {
        g_midi.requested_channel = bpm;
    } else if (command == MIDI_COMMAND_SET_NOTES_ENABLED) {
        g_midi.requested_notes_enabled = bpm != 0U;
    } else if (command == MIDI_COMMAND_SET_SEQUENCE) {
        if (sequence == NULL) {
            return unlock_sema(
                g_midi.control_sema,
                PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT));
        }
        g_midi.requested_sequence = *sequence;
    }
    g_midi.command = command;
    result = sceKernelSignalSema(g_midi.command_sema, 1);
    if (result >= 0) {
        result = sceKernelWaitSema(g_midi.acknowledgement_sema, 1, &timeout_us);
    }
    if (result >= 0) {
        MidiOutputSnapshot snapshot;

        result = MidiOutput_GetSnapshot(&snapshot);
        if (result >= 0) result = snapshot.last_result;
    }
    return unlock_sema(g_midi.control_sema, result);
}

static int delete_sema(SceUID *sema)
{
    int result = 0;
    if (*sema >= 0) {
        result = sceKernelDeleteSema(*sema);
        if (result >= 0) *sema = -1;
    }
    return result;
}

static int create_thread_state(void)
{
    int result;

    g_midi.command_sema = sceKernelCreateSema("Psp303MidiCmd", 0, 0, 1, NULL);
    if (g_midi.command_sema < 0) return g_midi.command_sema;
    g_midi.acknowledgement_sema = sceKernelCreateSema("Psp303MidiAck", 0, 0, 1, NULL);
    if (g_midi.acknowledgement_sema < 0) {
        result = g_midi.acknowledgement_sema;
        goto fail;
    }
    g_midi.state_sema = sceKernelCreateSema("Psp303MidiState", 0, 1, 1, NULL);
    if (g_midi.state_sema < 0) {
        result = g_midi.state_sema;
        goto fail;
    }
    g_midi.control_sema = sceKernelCreateSema("Psp303MidiCtl", 0, 1, 1, NULL);
    if (g_midi.control_sema < 0) {
        result = g_midi.control_sema;
        goto fail;
    }
    g_midi.thread = sceKernelCreateThread(
        "Psp303MidiClock",
        midi_thread,
        MIDI_THREAD_PRIORITY,
        MIDI_THREAD_STACK_SIZE,
        PSP_THREAD_ATTR_USER,
        NULL);
    if (g_midi.thread < 0) {
        result = g_midi.thread;
        goto fail;
    }
    result = sceKernelStartThread(g_midi.thread, 0, NULL);
    if (result < 0) {
        (void)sceKernelDeleteThread(g_midi.thread);
        g_midi.thread = -1;
        goto fail_result;
    }
    return 0;

fail:
fail_result:
    (void)delete_sema(&g_midi.control_sema);
    (void)delete_sema(&g_midi.state_sema);
    (void)delete_sema(&g_midi.acknowledgement_sema);
    (void)delete_sema(&g_midi.command_sema);
    return result;
}

int MidiOutput_Init(
    const char *driver_path,
    unsigned int bpm,
    const MidiOutputSequence *sequence)
{
    int result;

    if (driver_path == NULL || sequence == NULL || bpm < 40U || bpm > 300U) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    memset(&g_midi.snapshot, 0, sizeof(g_midi.snapshot));
    g_midi.requested_bpm = bpm;
    g_midi.requested_channel = 1U;
    g_midi.requested_notes_enabled = 1;
    g_midi.requested_sequence = *sequence;
    result = UsbMidi_Init(driver_path);
    if (result >= 0) result = UsbMidi_Start();
    if (result < 0) {
        (void)UsbMidi_Shutdown();
        g_midi.snapshot.last_result = result;
        return result;
    }
    result = create_thread_state();
    if (result < 0) {
        (void)UsbMidi_Shutdown();
        g_midi.snapshot.last_result = result;
        return result;
    }
    g_midi.snapshot.initialized = 1;
    g_midi.snapshot.bpm = bpm;
    g_midi.snapshot.channel = 1U;
    g_midi.snapshot.notes_enabled = 1;
    return 0;
}

int MidiOutput_SetBpm(unsigned int bpm)
{
    if (bpm < 40U || bpm > 300U) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    return submit_command(MIDI_COMMAND_SET_BPM, bpm, NULL);
}

int MidiOutput_SetChannel(unsigned int channel)
{
    if (channel < 1U || channel > 16U) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    return submit_command(MIDI_COMMAND_SET_CHANNEL, channel, NULL);
}

int MidiOutput_SetNotesEnabled(int enabled)
{
    return submit_command(
        MIDI_COMMAND_SET_NOTES_ENABLED, enabled != 0 ? 1U : 0U, NULL);
}

int MidiOutput_SetSequence(const MidiOutputSequence *sequence)
{
    return submit_command(MIDI_COMMAND_SET_SEQUENCE, 0U, sequence);
}

int MidiOutput_Start(void)
{
    return submit_command(MIDI_COMMAND_START, 0U, NULL);
}

int MidiOutput_Stop(void)
{
    return submit_command(MIDI_COMMAND_STOP, 0U, NULL);
}

int MidiOutput_GetSnapshot(MidiOutputSnapshot *snapshot)
{
    UsbMidiStatus usb_status;
    int result;

    if (snapshot == NULL) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    if (g_midi.state_sema < 0) {
        *snapshot = g_midi.snapshot;
        return g_midi.snapshot.last_result;
    }
    result = lock_sema(g_midi.state_sema);
    if (result < 0) return result;
    *snapshot = g_midi.snapshot;
    result = unlock_sema(g_midi.state_sema, 0);
    if (result < 0) return result;

    result = UsbMidi_GetStatus(&usb_status);
    if (result >= 0) {
        snapshot->usb_active = usb_status.active;
        snapshot->cable_connected = usb_status.cable_connected;
        snapshot->link_established = usb_status.link_established;
    }
    return result;
}

int MidiOutput_Shutdown(void)
{
    int result = 0;
    int cleanup_result;

    if (g_midi.thread >= 0) {
        (void)MidiOutput_Stop();
        result = submit_command(MIDI_COMMAND_EXIT, 0U, NULL);
        if (result < 0) return result;
        result = sceKernelWaitThreadEnd(g_midi.thread, NULL);
        if (result < 0) return result;
        result = sceKernelDeleteThread(g_midi.thread);
        if (result < 0) return result;
        g_midi.thread = -1;
        cleanup_result = delete_sema(&g_midi.control_sema);
        if (cleanup_result < 0) result = cleanup_result;
        cleanup_result = delete_sema(&g_midi.state_sema);
        if (cleanup_result < 0) result = cleanup_result;
        cleanup_result = delete_sema(&g_midi.acknowledgement_sema);
        if (cleanup_result < 0) result = cleanup_result;
        cleanup_result = delete_sema(&g_midi.command_sema);
        if (cleanup_result < 0) result = cleanup_result;
    }
    if (result < 0) return result;
    result = UsbMidi_Shutdown();
    if (result >= 0) memset(&g_midi.snapshot, 0, sizeof(g_midi.snapshot));
    return result;
}
