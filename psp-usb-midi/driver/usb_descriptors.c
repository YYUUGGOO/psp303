#include "usb_midi_descriptors.h"

#include <stddef.h>

enum {
    USB_DESCRIPTOR_DEVICE = 0x01,
    USB_DESCRIPTOR_CONFIGURATION = 0x02,
    USB_DESCRIPTOR_INTERFACE = 0x04,
    USB_DESCRIPTOR_ENDPOINT = 0x05,
    USB_DESCRIPTOR_CS_INTERFACE = 0x24,
    USB_DESCRIPTOR_CS_ENDPOINT = 0x25,
    USB_CLASS_AUDIO = 0x01,
    USB_SUBCLASS_AUDIO_CONTROL = 0x01,
    USB_SUBCLASS_MIDI_STREAMING = 0x03,
    USB_MS_HEADER = 0x01,
    USB_MIDI_IN_JACK = 0x02,
    USB_MIDI_OUT_JACK = 0x03,
    USB_JACK_EMBEDDED = 0x01,
    USB_JACK_EXTERNAL = 0x02,
    USB_ENDPOINT_BULK = 0x02,
    USB_ENDPOINT_OUT = 0x02,
    USB_ENDPOINT_IN = 0x81,
    USB_MIDI_EMBEDDED_IN_ID = 0x01,
    USB_MIDI_EXTERNAL_IN_ID = 0x02,
    USB_MIDI_EMBEDDED_OUT_ID = 0x03,
    USB_MIDI_EXTERNAL_OUT_ID = 0x04
};

_Static_assert(sizeof(UsbMidiDeviceDescriptor) == 18U, "USB device descriptor must be 18 bytes");
_Static_assert(sizeof(UsbMidiConfigurationHeader) == 9U, "USB configuration header must be 9 bytes");
_Static_assert(sizeof(UsbMidiInterfaceDescriptor) == 9U, "USB interface descriptor must be 9 bytes");
_Static_assert(sizeof(UsbMidiAcHeaderDescriptor) == 9U, "AudioControl header must be 9 bytes");
_Static_assert(sizeof(UsbMidiMsHeaderDescriptor) == 7U, "MIDIStreaming header must be 7 bytes");
_Static_assert(sizeof(UsbMidiInJackDescriptor) == 6U, "MIDI IN jack must be 6 bytes");
_Static_assert(sizeof(UsbMidiOutJackDescriptor) == 9U, "MIDI OUT jack must be 9 bytes");
_Static_assert(sizeof(UsbMidiEndpointDescriptor) == 9U, "MIDI bulk endpoint must be 9 bytes");
_Static_assert(sizeof(UsbMidiCsEndpointDescriptor) == 5U, "Class endpoint must be 5 bytes");
_Static_assert(
    sizeof(UsbMidiConfigurationDescriptor) == USB_MIDI_CONFIGURATION_TOTAL_LENGTH,
    "USB MIDI configuration must be 101 bytes");
_Static_assert(
    sizeof(UsbMidiConfigurationDescriptor) - offsetof(UsbMidiConfigurationDescriptor, midi_streaming_header)
        == USB_MIDI_STREAMING_TOTAL_LENGTH,
    "MIDIStreaming descriptor block must be 65 bytes");

#define INTERFACE_DESCRIPTOR(number, endpoints, subclass) \
    {9, USB_DESCRIPTOR_INTERFACE, number, 0, endpoints, USB_CLASS_AUDIO, subclass, 0, 0}

#define MIDI_CONFIGURATION(packet_size) \
    { \
        .configuration = { \
            9, USB_DESCRIPTOR_CONFIGURATION, USB_MIDI_CONFIGURATION_TOTAL_LENGTH, \
            2, 1, 0, 0x80, 0x32 \
        }, \
        .audio_control_interface = INTERFACE_DESCRIPTOR(0, 0, USB_SUBCLASS_AUDIO_CONTROL), \
        .audio_control_header = { \
            9, USB_DESCRIPTOR_CS_INTERFACE, USB_MS_HEADER, 0x0100, 9, 1, 1 \
        }, \
        .midi_streaming_interface = INTERFACE_DESCRIPTOR(1, 2, USB_SUBCLASS_MIDI_STREAMING), \
        .midi_streaming_header = { \
            7, USB_DESCRIPTOR_CS_INTERFACE, USB_MS_HEADER, 0x0100, USB_MIDI_STREAMING_TOTAL_LENGTH \
        }, \
        .embedded_in_jack = { \
            6, USB_DESCRIPTOR_CS_INTERFACE, USB_MIDI_IN_JACK, USB_JACK_EMBEDDED, \
            USB_MIDI_EMBEDDED_IN_ID, 0 \
        }, \
        .external_in_jack = { \
            6, USB_DESCRIPTOR_CS_INTERFACE, USB_MIDI_IN_JACK, USB_JACK_EXTERNAL, \
            USB_MIDI_EXTERNAL_IN_ID, 0 \
        }, \
        .embedded_out_jack = { \
            9, USB_DESCRIPTOR_CS_INTERFACE, USB_MIDI_OUT_JACK, USB_JACK_EMBEDDED, \
            USB_MIDI_EMBEDDED_OUT_ID, 1, USB_MIDI_EXTERNAL_IN_ID, 1, 0 \
        }, \
        .external_out_jack = { \
            9, USB_DESCRIPTOR_CS_INTERFACE, USB_MIDI_OUT_JACK, USB_JACK_EXTERNAL, \
            USB_MIDI_EXTERNAL_OUT_ID, 1, USB_MIDI_EMBEDDED_IN_ID, 1, 0 \
        }, \
        .bulk_in_endpoint = { \
            9, USB_DESCRIPTOR_ENDPOINT, USB_ENDPOINT_IN, USB_ENDPOINT_BULK, \
            packet_size, 0, 0, 0 \
        }, \
        .bulk_in_class = { \
            5, USB_DESCRIPTOR_CS_ENDPOINT, USB_MS_HEADER, 1, USB_MIDI_EMBEDDED_OUT_ID \
        }, \
        .bulk_out_endpoint = { \
            9, USB_DESCRIPTOR_ENDPOINT, USB_ENDPOINT_OUT, USB_ENDPOINT_BULK, \
            packet_size, 0, 0, 0 \
        }, \
        .bulk_out_class = { \
            5, USB_DESCRIPTOR_CS_ENDPOINT, USB_MS_HEADER, 1, USB_MIDI_EMBEDDED_IN_ID \
        } \
    }

const UsbMidiDeviceDescriptor g_usb_midi_device_descriptor = {
    18,
    USB_DESCRIPTOR_DEVICE,
    0x0200,
    0,
    0,
    0,
    64,
    0,
    0,
    0x0100,
    0,
    0,
    0,
    1
};

const UsbMidiConfigurationDescriptor g_usb_midi_full_speed_configuration =
    MIDI_CONFIGURATION(USB_MIDI_FULL_SPEED_PACKET_SIZE);

const UsbMidiConfigurationDescriptor g_usb_midi_high_speed_configuration =
    MIDI_CONFIGURATION(USB_MIDI_HIGH_SPEED_PACKET_SIZE);

static int validate_descriptor_chain(const UsbMidiConfigurationDescriptor *configuration)
{
    const uint8_t *bytes = (const uint8_t *)configuration;
    size_t descriptor_count = 0U;
    size_t offset = 0U;

    while (offset < sizeof(*configuration)) {
        size_t remaining = sizeof(*configuration) - offset;
        uint8_t length;

        if (remaining < 2U) {
            return -1;
        }
        length = bytes[offset];
        if (length < 2U || (size_t)length > remaining) {
            return -1;
        }
        offset += length;
        ++descriptor_count;
    }

    return offset == sizeof(*configuration) && descriptor_count == 13U ? 0 : -1;
}

static int validate_in_jack(
    const UsbMidiInJackDescriptor *jack,
    uint8_t expected_type,
    uint8_t expected_id)
{
    return jack->bLength == sizeof(*jack)
        && jack->bDescriptorType == USB_DESCRIPTOR_CS_INTERFACE
        && jack->bDescriptorSubtype == USB_MIDI_IN_JACK
        && jack->bJackType == expected_type
        && jack->bJackID == expected_id
        && jack->iJack == 0
        ? 0
        : -1;
}

static int validate_out_jack(
    const UsbMidiOutJackDescriptor *jack,
    uint8_t expected_type,
    uint8_t expected_id,
    uint8_t expected_source_id)
{
    return jack->bLength == sizeof(*jack)
        && jack->bDescriptorType == USB_DESCRIPTOR_CS_INTERFACE
        && jack->bDescriptorSubtype == USB_MIDI_OUT_JACK
        && jack->bJackType == expected_type
        && jack->bJackID == expected_id
        && jack->bNrInputPins == 1
        && jack->baSourceID1 == expected_source_id
        && jack->baSourcePin1 == 1
        && jack->iJack == 0
        ? 0
        : -1;
}

static int validate_endpoint(
    const UsbMidiEndpointDescriptor *endpoint,
    const UsbMidiCsEndpointDescriptor *class_endpoint,
    uint8_t expected_address,
    uint8_t expected_jack_id,
    uint16_t expected_packet_size)
{
    return endpoint->bLength == sizeof(*endpoint)
        && endpoint->bDescriptorType == USB_DESCRIPTOR_ENDPOINT
        && endpoint->bEndpointAddress == expected_address
        && endpoint->bmAttributes == USB_ENDPOINT_BULK
        && endpoint->wMaxPacketSize == expected_packet_size
        && endpoint->bInterval == 0
        && endpoint->bRefresh == 0
        && endpoint->bSynchAddress == 0
        && class_endpoint->bLength == sizeof(*class_endpoint)
        && class_endpoint->bDescriptorType == USB_DESCRIPTOR_CS_ENDPOINT
        && class_endpoint->bDescriptorSubtype == USB_MS_HEADER
        && class_endpoint->bNumEmbMIDIJack == 1
        && class_endpoint->baAssocJackID1 == expected_jack_id
        ? 0
        : -1;
}

int UsbMidiDescriptors_Validate(
    const UsbMidiConfigurationDescriptor *configuration,
    uint16_t expected_packet_size)
{
    if (configuration == NULL) {
        return -1;
    }
    if (expected_packet_size != USB_MIDI_FULL_SPEED_PACKET_SIZE
        && expected_packet_size != USB_MIDI_HIGH_SPEED_PACKET_SIZE) {
        return -1;
    }
    if (g_usb_midi_device_descriptor.bLength != sizeof(UsbMidiDeviceDescriptor)
        || g_usb_midi_device_descriptor.bDescriptorType != USB_DESCRIPTOR_DEVICE
        || g_usb_midi_device_descriptor.bcdUSB != 0x0200
        || g_usb_midi_device_descriptor.bDeviceClass != 0
        || g_usb_midi_device_descriptor.bDeviceSubClass != 0
        || g_usb_midi_device_descriptor.bDeviceProtocol != 0
        || g_usb_midi_device_descriptor.bMaxPacketSize0 != 64
        || g_usb_midi_device_descriptor.idVendor != 0
        || g_usb_midi_device_descriptor.idProduct != 0
        || g_usb_midi_device_descriptor.bcdDevice != 0x0100
        || g_usb_midi_device_descriptor.iManufacturer != 0
        || g_usb_midi_device_descriptor.iProduct != 0
        || g_usb_midi_device_descriptor.iSerialNumber != 0
        || g_usb_midi_device_descriptor.bNumConfigurations != 1) {
        return -1;
    }
    if (configuration->configuration.bLength != sizeof(UsbMidiConfigurationHeader)
        || configuration->configuration.bDescriptorType != USB_DESCRIPTOR_CONFIGURATION
        || configuration->configuration.wTotalLength != sizeof(*configuration)
        || configuration->configuration.bNumInterfaces != 2
        || configuration->configuration.bConfigurationValue != 1
        || configuration->configuration.iConfiguration != 0
        || configuration->configuration.bmAttributes != 0x80
        || configuration->configuration.bMaxPower != 0x32) {
        return -1;
    }
    if (configuration->audio_control_interface.bLength != sizeof(UsbMidiInterfaceDescriptor)
        || configuration->audio_control_interface.bDescriptorType != USB_DESCRIPTOR_INTERFACE
        || configuration->audio_control_interface.bInterfaceNumber != 0
        || configuration->audio_control_interface.bAlternateSetting != 0
        || configuration->audio_control_interface.bNumEndpoints != 0
        || configuration->audio_control_interface.bInterfaceClass != USB_CLASS_AUDIO
        || configuration->audio_control_interface.bInterfaceSubClass != USB_SUBCLASS_AUDIO_CONTROL
        || configuration->audio_control_interface.bInterfaceProtocol != 0
        || configuration->audio_control_interface.iInterface != 0) {
        return -1;
    }
    if (configuration->audio_control_header.bLength != sizeof(UsbMidiAcHeaderDescriptor)
        || configuration->audio_control_header.bDescriptorType != USB_DESCRIPTOR_CS_INTERFACE
        || configuration->audio_control_header.bDescriptorSubtype != USB_MS_HEADER
        || configuration->audio_control_header.bcdADC != 0x0100
        || configuration->audio_control_header.wTotalLength != sizeof(UsbMidiAcHeaderDescriptor)
        || configuration->audio_control_header.bInCollection != 1
        || configuration->audio_control_header.baInterfaceNr1 != 1) {
        return -1;
    }
    if (configuration->midi_streaming_interface.bLength != sizeof(UsbMidiInterfaceDescriptor)
        || configuration->midi_streaming_interface.bDescriptorType != USB_DESCRIPTOR_INTERFACE
        || configuration->midi_streaming_interface.bInterfaceNumber != 1
        || configuration->midi_streaming_interface.bAlternateSetting != 0
        || configuration->midi_streaming_interface.bNumEndpoints != 2
        || configuration->midi_streaming_interface.bInterfaceClass != USB_CLASS_AUDIO
        || configuration->midi_streaming_interface.bInterfaceSubClass != USB_SUBCLASS_MIDI_STREAMING
        || configuration->midi_streaming_interface.bInterfaceProtocol != 0
        || configuration->midi_streaming_interface.iInterface != 0) {
        return -1;
    }
    if (configuration->midi_streaming_header.bLength != sizeof(UsbMidiMsHeaderDescriptor)
        || configuration->midi_streaming_header.bDescriptorType != USB_DESCRIPTOR_CS_INTERFACE
        || configuration->midi_streaming_header.bDescriptorSubtype != USB_MS_HEADER
        || configuration->midi_streaming_header.bcdMSC != 0x0100
        || configuration->midi_streaming_header.wTotalLength != USB_MIDI_STREAMING_TOTAL_LENGTH) {
        return -1;
    }
    if (validate_in_jack(
            &configuration->embedded_in_jack,
            USB_JACK_EMBEDDED,
            USB_MIDI_EMBEDDED_IN_ID) < 0
        || validate_in_jack(
            &configuration->external_in_jack,
            USB_JACK_EXTERNAL,
            USB_MIDI_EXTERNAL_IN_ID) < 0
        || validate_out_jack(
            &configuration->embedded_out_jack,
            USB_JACK_EMBEDDED,
            USB_MIDI_EMBEDDED_OUT_ID,
            USB_MIDI_EXTERNAL_IN_ID) < 0
        || validate_out_jack(
            &configuration->external_out_jack,
            USB_JACK_EXTERNAL,
            USB_MIDI_EXTERNAL_OUT_ID,
            USB_MIDI_EMBEDDED_IN_ID) < 0) {
        return -1;
    }
    if (validate_endpoint(
            &configuration->bulk_out_endpoint,
            &configuration->bulk_out_class,
            USB_ENDPOINT_OUT,
            USB_MIDI_EMBEDDED_IN_ID,
            expected_packet_size) < 0) {
        return -1;
    }
    if (validate_endpoint(
            &configuration->bulk_in_endpoint,
            &configuration->bulk_in_class,
            USB_ENDPOINT_IN,
            USB_MIDI_EMBEDDED_OUT_ID,
            expected_packet_size) < 0) {
        return -1;
    }

    return validate_descriptor_chain(configuration);
}
