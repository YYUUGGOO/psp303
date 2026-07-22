#include "usb_midi_state.h"

#include <stddef.h>

static int transition_is_allowed(UsbMidiState from, UsbMidiState to)
{
    if (from == to) {
        return 1;
    }

    switch (from) {
        case USB_MIDI_STATE_UNINITIALIZED:
            return to == USB_MIDI_STATE_INITIALIZED
                || to == USB_MIDI_STATE_STOPPED;
        case USB_MIDI_STATE_INITIALIZED:
            return to == USB_MIDI_STATE_REGISTERED
                || to == USB_MIDI_STATE_UNINITIALIZED
                || to == USB_MIDI_STATE_STOPPING;
        case USB_MIDI_STATE_REGISTERED:
            return to == USB_MIDI_STATE_STARTED
                || to == USB_MIDI_STATE_STOPPING
                || to == USB_MIDI_STATE_STOPPED;
        case USB_MIDI_STATE_STARTED:
            return to == USB_MIDI_STATE_ACTIVE
                || to == USB_MIDI_STATE_REGISTERED
                || to == USB_MIDI_STATE_STOPPING;
        case USB_MIDI_STATE_ACTIVE:
            return to == USB_MIDI_STATE_CONNECTED
                || to == USB_MIDI_STATE_DISCONNECTED
                || to == USB_MIDI_STATE_STARTED
                || to == USB_MIDI_STATE_STOPPING;
        case USB_MIDI_STATE_CONNECTED:
            return to == USB_MIDI_STATE_DISCONNECTED
                || to == USB_MIDI_STATE_ACTIVE
                || to == USB_MIDI_STATE_STOPPING;
        case USB_MIDI_STATE_DISCONNECTED:
            return to == USB_MIDI_STATE_CONNECTED
                || to == USB_MIDI_STATE_ACTIVE
                || to == USB_MIDI_STATE_STOPPING;
        case USB_MIDI_STATE_STOPPING:
            return to == USB_MIDI_STATE_INITIALIZED
                || to == USB_MIDI_STATE_REGISTERED
                || to == USB_MIDI_STATE_STARTED
                || to == USB_MIDI_STATE_ACTIVE
                || to == USB_MIDI_STATE_CONNECTED
                || to == USB_MIDI_STATE_DISCONNECTED
                || to == USB_MIDI_STATE_STOPPED;
        case USB_MIDI_STATE_STOPPED:
            return to == USB_MIDI_STATE_INITIALIZED;
        default:
            return 0;
    }
}

int UsbMidiState_Transition(UsbMidiState *state, UsbMidiState next_state)
{
    if (state == NULL || !transition_is_allowed(*state, next_state)) {
        return -1;
    }
    *state = next_state;
    return 0;
}
