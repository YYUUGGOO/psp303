#ifndef USB_MIDI_TX_H
#define USB_MIDI_TX_H

#include <pspusbbus.h>

/** Create the bounded bidirectional MIDI worker for the bulk endpoints. */
int UsbMidiTx_Start(
    struct UsbEndpoint *bulk_in_endpoint,
    struct UsbEndpoint *bulk_out_endpoint);

/** Notify the worker that the host selected the configuration at this bus speed. */
int UsbMidiTx_OnAttach(int speed);

/** Notify the worker that the USB configuration was removed. */
int UsbMidiTx_OnDetach(void);

/** Cancel any request, join the worker, and release its kernel objects. */
int UsbMidiTx_Stop(void);

#endif
