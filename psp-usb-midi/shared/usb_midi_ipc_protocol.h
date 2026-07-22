#ifndef USB_MIDI_IPC_PROTOCOL_H
#define USB_MIDI_IPC_PROTOCOL_H

#include <stdint.h>

#define USB_MIDI_IPC_MAGIC 0x554D4950U
#define USB_MIDI_IPC_VERSION 4U
#define USB_MIDI_IPC_PIPE_BYTES 256U
#define USB_MIDI_IPC_MAX_BATCH_EVENTS 8U
#define USB_MIDI_IPC_EVENT_TX_AVAILABLE (1U << 5)

typedef enum UsbMidiIpcMessageKind {
    USB_MIDI_IPC_MESSAGE_HELLO = 1,
    USB_MIDI_IPC_MESSAGE_ACK = 2,
    USB_MIDI_IPC_MESSAGE_MIDI_TX_BATCH = 3,
    USB_MIDI_IPC_MESSAGE_MIDI_RX = 4
} UsbMidiIpcMessageKind;

typedef struct UsbMidiIpcStartup {
    uint32_t size;
    uint32_t magic;
    uint32_t version;
    int32_t user_to_kernel_pipe;
    int32_t kernel_to_user_pipe;
    int32_t event_flag;
} UsbMidiIpcStartup;

typedef struct UsbMidiIpcMessage {
    uint32_t magic;
    uint32_t version;
    uint32_t kind;
    uint32_t sequence;
} UsbMidiIpcMessage;

/** Fixed wire envelope for one complete user-to-kernel MIDI event. */
typedef struct UsbMidiIpcMidiEvent {
    uint32_t magic;
    uint32_t version;
    uint32_t kind;
    uint32_t sequence;
    uint32_t timestamp_us;
    uint8_t cable;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
} UsbMidiIpcMidiEvent;

typedef struct UsbMidiIpcEventData {
    uint32_t timestamp_us;
    uint8_t cable;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
} UsbMidiIpcEventData;

/** One atomic, fixed-capacity user-to-kernel transmit batch. */
typedef struct UsbMidiIpcMidiBatch {
    uint32_t magic;
    uint32_t version;
    uint32_t kind;
    uint32_t sequence;
    uint32_t count;
    UsbMidiIpcEventData events[USB_MIDI_IPC_MAX_BATCH_EVENTS];
} UsbMidiIpcMidiBatch;

_Static_assert(sizeof(UsbMidiIpcStartup) == 24U, "IPC startup ABI changed");
_Static_assert(sizeof(UsbMidiIpcMessage) == 16U, "IPC message ABI changed");
_Static_assert(sizeof(UsbMidiIpcMidiEvent) == 24U, "IPC MIDI event ABI changed");
_Static_assert(sizeof(UsbMidiIpcEventData) == 8U, "IPC event data ABI changed");
_Static_assert(sizeof(UsbMidiIpcMidiBatch) == 84U, "IPC MIDI batch ABI changed");

#endif
