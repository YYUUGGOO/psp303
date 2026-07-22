#ifndef USB_MIDI_STATE_H
#define USB_MIDI_STATE_H

#include "psp_usb_midi.h"

/** Apply one legal public state transition. */
int UsbMidiState_Transition(UsbMidiState *state, UsbMidiState next_state);

#endif
