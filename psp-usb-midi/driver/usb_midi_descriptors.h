#ifndef USB_MIDI_DESCRIPTORS_H
#define USB_MIDI_DESCRIPTORS_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define USB_MIDI_PACKED __attribute__((packed))
#else
#error "A compiler with packed-structure support is required"
#endif

enum {
    USB_MIDI_CONFIGURATION_TOTAL_LENGTH = 101,
    USB_MIDI_STREAMING_TOTAL_LENGTH = 65,
    USB_MIDI_FULL_SPEED_PACKET_SIZE = 64,
    USB_MIDI_HIGH_SPEED_PACKET_SIZE = 512
};

typedef struct USB_MIDI_PACKED UsbMidiDeviceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} UsbMidiDeviceDescriptor;

typedef struct USB_MIDI_PACKED UsbMidiConfigurationHeader {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} UsbMidiConfigurationHeader;

typedef struct USB_MIDI_PACKED UsbMidiInterfaceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} UsbMidiInterfaceDescriptor;

typedef struct USB_MIDI_PACKED UsbMidiAcHeaderDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint16_t bcdADC;
    uint16_t wTotalLength;
    uint8_t bInCollection;
    uint8_t baInterfaceNr1;
} UsbMidiAcHeaderDescriptor;

typedef struct USB_MIDI_PACKED UsbMidiMsHeaderDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint16_t bcdMSC;
    uint16_t wTotalLength;
} UsbMidiMsHeaderDescriptor;

typedef struct USB_MIDI_PACKED UsbMidiInJackDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bJackType;
    uint8_t bJackID;
    uint8_t iJack;
} UsbMidiInJackDescriptor;

typedef struct USB_MIDI_PACKED UsbMidiOutJackDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bJackType;
    uint8_t bJackID;
    uint8_t bNrInputPins;
    uint8_t baSourceID1;
    uint8_t baSourcePin1;
    uint8_t iJack;
} UsbMidiOutJackDescriptor;

typedef struct USB_MIDI_PACKED UsbMidiEndpointDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    uint8_t bRefresh;
    uint8_t bSynchAddress;
} UsbMidiEndpointDescriptor;

typedef struct USB_MIDI_PACKED UsbMidiCsEndpointDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bDescriptorSubtype;
    uint8_t bNumEmbMIDIJack;
    uint8_t baAssocJackID1;
} UsbMidiCsEndpointDescriptor;

typedef struct USB_MIDI_PACKED UsbMidiConfigurationDescriptor {
    UsbMidiConfigurationHeader configuration;
    UsbMidiInterfaceDescriptor audio_control_interface;
    UsbMidiAcHeaderDescriptor audio_control_header;
    UsbMidiInterfaceDescriptor midi_streaming_interface;
    UsbMidiMsHeaderDescriptor midi_streaming_header;
    UsbMidiInJackDescriptor embedded_in_jack;
    UsbMidiInJackDescriptor external_in_jack;
    UsbMidiOutJackDescriptor embedded_out_jack;
    UsbMidiOutJackDescriptor external_out_jack;
    UsbMidiEndpointDescriptor bulk_in_endpoint;
    UsbMidiCsEndpointDescriptor bulk_in_class;
    UsbMidiEndpointDescriptor bulk_out_endpoint;
    UsbMidiCsEndpointDescriptor bulk_out_class;
} UsbMidiConfigurationDescriptor;

extern const UsbMidiDeviceDescriptor g_usb_midi_device_descriptor;
extern const UsbMidiConfigurationDescriptor g_usb_midi_full_speed_configuration;
extern const UsbMidiConfigurationDescriptor g_usb_midi_high_speed_configuration;

/**
 * Validate every descriptor length, type, topology link, endpoint, and total.
 *
 * @param configuration Descriptor set to validate.
 * @param expected_packet_size Expected bulk maximum packet size (64 or 512).
 * @return 0 when valid, otherwise -1.
 */
int UsbMidiDescriptors_Validate(
    const UsbMidiConfigurationDescriptor *configuration,
    uint16_t expected_packet_size);

#endif
