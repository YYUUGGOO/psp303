#ifndef PSP_USB_MIDI_H
#define PSP_USB_MIDI_H

#include <stdbool.h>
#include <stdint.h>

#define PSP_USB_MIDI_VERSION_MAJOR 1U
#define PSP_USB_MIDI_VERSION_MINOR 0U
#define PSP_USB_MIDI_MAX_BATCH_EVENTS 8

/** Public lifecycle state for the manually loaded USB MIDI device. */
typedef enum UsbMidiState {
    USB_MIDI_STATE_UNINITIALIZED = 0,
    USB_MIDI_STATE_INITIALIZED,
    USB_MIDI_STATE_REGISTERED,
    USB_MIDI_STATE_STARTED,
    USB_MIDI_STATE_ACTIVE,
    USB_MIDI_STATE_CONNECTED,
    USB_MIDI_STATE_DISCONNECTED,
    USB_MIDI_STATE_STOPPING,
    USB_MIDI_STATE_STOPPED
} UsbMidiState;

/** One complete, timestamped, non-SysEx MIDI message. */
typedef struct UsbMidiEvent {
    uint32_t timestamp_us;
    uint8_t cable;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
} UsbMidiEvent;

_Static_assert(sizeof(UsbMidiEvent) == 8U, "public MIDI event ABI changed");

/** Stable connection snapshot returned to user-mode applications. */
typedef struct UsbMidiStatus {
    uint32_t size;
    UsbMidiState state;
    int last_result;
    uint32_t raw_usb_state;
    uint8_t active;
    uint8_t cable_connected;
    uint8_t link_established;
    uint8_t reserved;
} UsbMidiStatus;

/** Load and start the bundled kernel PRX and register the USB MIDI driver. */
int UsbMidi_Init(const char *driver_path);

/** Atomically start the USB bus, MIDI driver, and device activation. */
int UsbMidi_Start(void);

/** Deactivate and stop USB while keeping the PRX loaded for later reuse. */
int UsbMidi_Stop(void);

/** Stop USB, unregister the driver, and unload the kernel PRX. */
int UsbMidi_Shutdown(void);

/**
 * Nonblockingly enqueue one atomic batch of complete non-SysEx messages.
 *
 * count must be between 1 and PSP_USB_MIDI_MAX_BATCH_EVENTS. The fixed queue
 * holds three maximum-size batches. A full queue returns a negative PSP error
 * without partially enqueuing the batch. timestamp_us is metadata only.
 */
int UsbMidi_Write(const UsbMidiEvent *events, int count);

/**
 * Nonblockingly read up to PSP_USB_MIDI_MAX_BATCH_EVENTS host messages.
 *
 * Messages are returned in wire order. The fixed receive queue holds ten
 * events; new host events are dropped while it is full.
 *
 * @return event count, zero when empty, or a negative PSP error.
 */
int UsbMidi_Read(UsbMidiEvent *events, int max_events);

/** Copy the current lifecycle and USB connection state. */
int UsbMidi_GetStatus(UsbMidiStatus *status);

/** Return true only when the host has established the USB MIDI link. */
bool UsbMidi_IsConnected(void);

#endif
