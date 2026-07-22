#include "psp_usb_midi.h"

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspkernel.h>

#include <stdint.h>

PSP_MODULE_INFO("UsbMidiExample", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

#define DRIVER_PATH "ms0:/PSP/GAME/UsbMidiExample/UsbMidiDriver.prx"

static volatile int g_exit_requested = 0;

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    g_exit_requested = 1;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    int callback_id;
    int result;

    (void)args;
    (void)argp;
    callback_id = sceKernelCreateCallback(
        "UsbMidiExampleExit",
        exit_callback,
        NULL);
    if (callback_id < 0) {
        return callback_id;
    }
    result = sceKernelRegisterExitCallback(callback_id);
    if (result < 0) {
        return result;
    }
    for (;;) {
        result = sceKernelSleepThreadCB();
        if (result < 0) {
            return result;
        }
    }
}

static int setup_callbacks(void)
{
    SceUID thread = sceKernelCreateThread(
        "UsbMidiExampleCallback",
        callback_thread,
        0x11,
        0x1000,
        PSP_THREAD_ATTR_USER,
        NULL);

    if (thread < 0) {
        return thread;
    }
    return sceKernelStartThread(thread, 0, NULL);
}

static int send_note(uint8_t status, uint8_t velocity)
{
    UsbMidiEvent event;

    event.timestamp_us = sceKernelGetSystemTimeLow();
    event.cable = 0U;
    event.status = status;
    event.data1 = 60U;
    event.data2 = velocity;
    return UsbMidi_Write(&event, 1);
}

static int activate_usb(void)
{
    int result = UsbMidi_Init(DRIVER_PATH);

    return result < 0 ? result : UsbMidi_Start();
}

int main(void)
{
    SceCtrlData pad;
    UsbMidiEvent received[PSP_USB_MIDI_MAX_BATCH_EVENTS];
    UsbMidiEvent last_received = {0U, 0U, 0U, 0U, 0U};
    unsigned int previous_buttons = 0U;
    unsigned int receive_count = 0U;
    int note_active = 0;
    int last_result;

    pspDebugScreenInit();
    last_result = setup_callbacks();
    if (last_result >= 0) {
        last_result = sceCtrlSetSamplingCycle(0);
    }
    if (last_result >= 0) {
        last_result = sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    }
    if (last_result >= 0) {
        last_result = activate_usb();
    }

    while (g_exit_requested == 0) {
        UsbMidiStatus status;
        unsigned int pressed;
        unsigned int released;
        int read_result = UsbMidi_GetStatus(&status);

        if (read_result < 0) {
            last_result = read_result;
            status.active = 0U;
            status.cable_connected = 0U;
            status.link_established = 0U;
            status.raw_usb_state = 0U;
        } else {
            read_result = UsbMidi_Read(
                received,
                PSP_USB_MIDI_MAX_BATCH_EVENTS);
            if (read_result < 0) {
                last_result = read_result;
            } else if (read_result > 0) {
                last_received = received[read_result - 1];
                receive_count += (unsigned int)read_result;
            }
        }

        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("PSP USB MIDI 1.0 example             \n\n");
        pspDebugScreenPrintf(
            "USB: 0x%03X active:%c cable:%c link:%c  \n",
            (unsigned int)status.raw_usb_state,
            status.active != 0U ? 'Y' : 'N',
            status.cable_connected != 0U ? 'Y' : 'N',
            status.link_established != 0U ? 'Y' : 'N');
        pspDebugScreenPrintf(
            "Last result: 0x%08X                   \n",
            (unsigned int)last_result);
        pspDebugScreenPrintf(
            "RX total:%u last:%02X %02X %02X c:%u  \n\n",
            receive_count,
            last_received.status,
            last_received.data1,
            last_received.data2,
            last_received.cable);
        pspDebugScreenPrintf("Hold CROSS: send C3 Note On          \n");
        pspDebugScreenPrintf("Release CROSS: send C3 Note Off      \n");
        pspDebugScreenPrintf("TRIANGLE: stop/start USB             \n");
        pspDebugScreenPrintf("HOME: clean shutdown and exit        \n");

        read_result = sceCtrlReadBufferPositive(&pad, 1);
        if (read_result == 1) {
            pressed = pad.Buttons & ~previous_buttons;
            released = previous_buttons & ~pad.Buttons;
            previous_buttons = pad.Buttons;

            if ((pressed & PSP_CTRL_CROSS) != 0U
                && status.link_established != 0U) {
                last_result = send_note(0x90U, 100U);
                note_active = last_result == 0;
            }
            if ((released & PSP_CTRL_CROSS) != 0U && note_active != 0) {
                last_result = send_note(0x80U, 0U);
                if (last_result == 0) {
                    note_active = 0;
                }
            }
            if ((pressed & PSP_CTRL_TRIANGLE) != 0U) {
                if (note_active != 0 && status.link_established != 0U) {
                    if (send_note(0x80U, 0U) == 0) {
                        (void)sceKernelDelayThread(50000U);
                    }
                    note_active = 0;
                }
                last_result = status.active != 0U
                    ? UsbMidi_Stop()
                    : activate_usb();
            }
        } else if (read_result < 0) {
            last_result = read_result;
        }
        if (status.link_established == 0U) {
            note_active = 0;
        }
        (void)sceKernelDelayThread(16667U);
    }

    if (note_active != 0) {
        if (send_note(0x80U, 0U) == 0) {
            (void)sceKernelDelayThread(50000U);
        }
    }
    last_result = UsbMidi_Shutdown();
    while (last_result < 0) {
        pspDebugScreenPrintf(
            "Shutdown failed: 0x%08X CROSS retries\n",
            (unsigned int)last_result);
        if (sceCtrlReadBufferPositive(&pad, 1) == 1
            && (pad.Buttons & PSP_CTRL_CROSS) != 0U) {
            last_result = UsbMidi_Shutdown();
        }
        (void)sceKernelDelayThread(16667U);
    }
    sceKernelExitGame();
    return 0;
}
