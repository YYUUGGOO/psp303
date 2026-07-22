#ifndef USB_MIDI_IPC_H
#define USB_MIDI_IPC_H

#include <pspkerneltypes.h>

#include "usb_midi_ipc_protocol.h"

/** Validate user-created pipes and consume the bounded startup hello. */
int UsbMidiIpc_Configure(SceSize args, void *argp);

/** Publish the matching acknowledgement after USB registration succeeds. */
int UsbMidiIpc_Acknowledge(void);

/**
 * Nonblockingly consume one queued atomic transmit batch.
 *
 * @return 1 when an event was copied, 0 when the pipe is empty, or a negative
 *         kernel/validation error.
 */
int UsbMidiIpc_TryReceiveTxBatch(UsbMidiIpcMidiBatch *batch);

/** Nonblockingly publish one validated host event to the user queue. */
int UsbMidiIpc_SendRxEvent(
    uint32_t timestamp_us,
    uint8_t cable,
    uint8_t status,
    uint8_t data1,
    uint8_t data2);

/** Return the facade-owned event flag borrowed by the USB worker. */
SceUID UsbMidiIpc_GetEventFlag(void);

/** Forget borrowed pipe UIDs; the user facade remains their sole owner. */
void UsbMidiIpc_Reset(void);

#endif
