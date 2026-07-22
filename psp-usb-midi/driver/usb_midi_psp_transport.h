#ifndef USB_MIDI_PSP_TRANSPORT_H
#define USB_MIDI_PSP_TRANSPORT_H

#include "usb_midi_descriptors.h"

#include <stddef.h>
#include <stdint.h>

enum {
    USB_MIDI_PSP_INTERFACE_COUNT = 2,
    USB_MIDI_PSP_INTERFACE_RECORD_COUNT = USB_MIDI_PSP_INTERFACE_COUNT + 1,
    USB_MIDI_PSP_ENDPOINT_COUNT = 2,
    USB_MIDI_PSP_ENDPOINT_RECORD_COUNT = USB_MIDI_PSP_ENDPOINT_COUNT + 1,
    USB_MIDI_PSP_AC_EXTENSION_LENGTH = 9,
    USB_MIDI_PSP_MS_EXTENSION_LENGTH = 37,
    USB_MIDI_PSP_ENDPOINT_EXTENSION_LENGTH = 7
};

/*
 * Sony 6.61 stores only the seven-byte standard endpoint prefix inline.
 * Bytes beyond that prefix, including bRefresh/bSynchAddress for a nine-byte
 * Audio-class endpoint, live in the explicit extension buffer.
 */
typedef struct USB_MIDI_PACKED UsbMidiPspEndpointPrefix {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} UsbMidiPspEndpointPrefix;

typedef struct UsbMidiPspEndpointRecord {
    UsbMidiPspEndpointPrefix descriptor;
    uint8_t alignment_padding;
    uint8_t *extension;
    uint32_t extension_length;
} UsbMidiPspEndpointRecord;

typedef struct UsbMidiPspInterfaceRecord {
    UsbMidiInterfaceDescriptor descriptor;
    uint8_t alignment_padding[3];
    UsbMidiPspEndpointRecord *endpoints;
    uint8_t *extension;
    uint32_t extension_length;
} UsbMidiPspInterfaceRecord;

typedef struct UsbMidiPspInterfaceGroup {
    /* Firmware advances this source pointer by 24 bytes per alternate. */
    UsbMidiPspInterfaceRecord *first_alternate;
    uint32_t reserved;
    uint32_t alternate_count;
} UsbMidiPspInterfaceGroup;

typedef struct UsbMidiPspConfiguration {
    UsbMidiConfigurationHeader *configuration;
    UsbMidiPspInterfaceGroup *interface_groups;
    UsbMidiPspInterfaceRecord *interfaces;
    UsbMidiPspEndpointRecord *endpoints;
} UsbMidiPspConfiguration;

/*
 * Sony 6.61 copies these source records at their internal widths, not merely
 * at the public USB descriptor lengths. bLength remains 18 and 9; the wider
 * records contain firmware-private padding and pointer fields.
 */
typedef struct USB_MIDI_PACKED UsbMidiPspDeviceRecord {
    UsbMidiDeviceDescriptor descriptor;
    uint8_t padding[2];
} UsbMidiPspDeviceRecord;

typedef struct USB_MIDI_PACKED UsbMidiPspConfigurationRecord {
    UsbMidiConfigurationHeader descriptor;
    uint8_t alignment_padding[3];
    /* Sony 6.61 reads this pointer directly from source-record offset 12. */
    UsbMidiPspInterfaceGroup *interface_groups;
    uint8_t trailing_padding[8];
} UsbMidiPspConfigurationRecord;

typedef struct UsbMidiPspDescriptorSet {
    UsbMidiPspDeviceRecord device;
    /* Preserve PSPDEV UsbData's proven link-at-offset-20 leading layout. */
    UsbMidiPspConfiguration link;
    UsbMidiPspConfigurationRecord configuration;
    UsbMidiPspInterfaceGroup interface_groups[USB_MIDI_PSP_INTERFACE_COUNT];
    /* Match Sony usbmic.prx: live records followed by one zero record. */
    UsbMidiPspInterfaceRecord interfaces[USB_MIDI_PSP_INTERFACE_RECORD_COUNT];
    /* Sony 6.61 walks this array until a record with bLength == 0. */
    UsbMidiPspEndpointRecord endpoints[USB_MIDI_PSP_ENDPOINT_RECORD_COUNT];
    uint8_t audio_control_extension[USB_MIDI_PSP_AC_EXTENSION_LENGTH];
    uint8_t midi_streaming_extension[USB_MIDI_PSP_MS_EXTENSION_LENGTH];
    uint8_t endpoint_extensions
        [USB_MIDI_PSP_ENDPOINT_COUNT][USB_MIDI_PSP_ENDPOINT_EXTENSION_LENGTH];
} UsbMidiPspDescriptorSet;

/** Build Sony 6.61 pointer/length records from the canonical MIDI chain. */
int UsbMidiPspTransport_Init(
    UsbMidiPspDescriptorSet *set,
    const UsbMidiConfigurationDescriptor *configuration,
    uint16_t expected_packet_size);

/** Validate pointers, counts, lengths, and byte-for-byte reconstruction. */
int UsbMidiPspTransport_Validate(
    const UsbMidiPspDescriptorSet *set,
    const UsbMidiConfigurationDescriptor *configuration);

/**
 * Reconstruct the configuration response using the 6.61 traversal rules.
 * This is a host-testable model; it performs no USB operation.
 */
int UsbMidiPspTransport_Compose(
    const UsbMidiPspDescriptorSet *set,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

#endif
