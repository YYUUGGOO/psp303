#include "usb_midi_psp_transport.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(UsbMidiPspEndpointPrefix) == 7U, "endpoint prefix must be 7 bytes");
_Static_assert(sizeof(UsbMidiPspDeviceRecord) == 20U, "6.61 device source record must be 20 bytes");
_Static_assert(
    offsetof(UsbMidiPspDeviceRecord, descriptor) == 0U,
    "device descriptor must begin its source record");
_Static_assert(
    offsetof(UsbMidiPspConfigurationRecord, descriptor) == 0U,
    "configuration descriptor must begin its source record");
_Static_assert(
    offsetof(UsbMidiPspConfigurationRecord, interface_groups) == 12U,
    "6.61 configuration interface-group pointer offset must be 12");

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(
    sizeof(UsbMidiPspConfigurationRecord) == 24U,
    "6.61 configuration source record must be 24 bytes");
_Static_assert(sizeof(UsbMidiPspEndpointRecord) == 16U, "6.61 endpoint record must be 16 bytes");
_Static_assert(
    offsetof(UsbMidiPspEndpointRecord, extension) == 8U,
    "6.61 endpoint extension pointer offset must be 8");
_Static_assert(
    offsetof(UsbMidiPspEndpointRecord, extension_length) == 12U,
    "6.61 endpoint extension length offset must be 12");
_Static_assert(sizeof(UsbMidiPspInterfaceRecord) == 24U, "6.61 interface record must be 24 bytes");
_Static_assert(
    offsetof(UsbMidiPspInterfaceRecord, endpoints) == 12U,
    "6.61 interface endpoint pointer offset must be 12");
_Static_assert(
    offsetof(UsbMidiPspInterfaceRecord, extension) == 16U,
    "6.61 interface extension pointer offset must be 16");
_Static_assert(
    offsetof(UsbMidiPspInterfaceRecord, extension_length) == 20U,
    "6.61 interface extension length offset must be 20");
_Static_assert(sizeof(UsbMidiPspInterfaceGroup) == 12U, "6.61 interface group must be 12 bytes");
_Static_assert(
    offsetof(UsbMidiPspInterfaceGroup, first_alternate) == 0U,
    "6.61 first alternate pointer offset must be 0");
_Static_assert(
    offsetof(UsbMidiPspInterfaceGroup, alternate_count) == 8U,
    "6.61 alternate count offset must be 8");
_Static_assert(sizeof(UsbMidiPspConfiguration) == 16U, "6.61 configuration link must be 16 bytes");
#endif

static int append_bytes(
    uint8_t *output,
    size_t output_capacity,
    size_t *offset,
    const void *source,
    size_t length)
{
    if (output == NULL || offset == NULL || source == NULL || *offset > output_capacity
        || length > output_capacity - *offset) {
        return -1;
    }
    memcpy(output + *offset, source, length);
    *offset += length;
    return 0;
}

static int bytes_are_zero(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return 0;
        }
    }
    return 1;
}

static void init_endpoint(
    UsbMidiPspEndpointRecord *record,
    uint8_t extension[USB_MIDI_PSP_ENDPOINT_EXTENSION_LENGTH],
    const UsbMidiEndpointDescriptor *endpoint,
    const UsbMidiCsEndpointDescriptor *class_endpoint)
{
    record->descriptor.bLength = endpoint->bLength;
    record->descriptor.bDescriptorType = endpoint->bDescriptorType;
    record->descriptor.bEndpointAddress = endpoint->bEndpointAddress;
    record->descriptor.bmAttributes = endpoint->bmAttributes;
    record->descriptor.wMaxPacketSize = endpoint->wMaxPacketSize;
    record->descriptor.bInterval = endpoint->bInterval;
    record->alignment_padding = 0;
    extension[0] = endpoint->bRefresh;
    extension[1] = endpoint->bSynchAddress;
    memcpy(extension + 2, class_endpoint, sizeof(*class_endpoint));
    record->extension = extension;
    record->extension_length = USB_MIDI_PSP_ENDPOINT_EXTENSION_LENGTH;
}

int UsbMidiPspTransport_Init(
    UsbMidiPspDescriptorSet *set,
    const UsbMidiConfigurationDescriptor *configuration,
    uint16_t expected_packet_size)
{
    uint8_t *cursor;

    if (set == NULL || configuration == NULL
        || UsbMidiDescriptors_Validate(configuration, expected_packet_size) < 0) {
        return -1;
    }

    memset(set, 0, sizeof(*set));
    set->device.descriptor = g_usb_midi_device_descriptor;
    set->configuration.descriptor = configuration->configuration;
    set->configuration.interface_groups = set->interface_groups;

    set->interfaces[0].descriptor = configuration->audio_control_interface;
    set->interfaces[0].extension = set->audio_control_extension;
    set->interfaces[0].extension_length = USB_MIDI_PSP_AC_EXTENSION_LENGTH;
    memcpy(
        set->audio_control_extension,
        &configuration->audio_control_header,
        USB_MIDI_PSP_AC_EXTENSION_LENGTH);

    set->interfaces[1].descriptor = configuration->midi_streaming_interface;
    set->interfaces[1].endpoints = set->endpoints;
    set->interfaces[1].extension = set->midi_streaming_extension;
    set->interfaces[1].extension_length = USB_MIDI_PSP_MS_EXTENSION_LENGTH;
    cursor = set->midi_streaming_extension;
    memcpy(cursor, &configuration->midi_streaming_header, sizeof(configuration->midi_streaming_header));
    cursor += sizeof(configuration->midi_streaming_header);
    memcpy(cursor, &configuration->embedded_in_jack, sizeof(configuration->embedded_in_jack));
    cursor += sizeof(configuration->embedded_in_jack);
    memcpy(cursor, &configuration->external_in_jack, sizeof(configuration->external_in_jack));
    cursor += sizeof(configuration->external_in_jack);
    memcpy(cursor, &configuration->embedded_out_jack, sizeof(configuration->embedded_out_jack));
    cursor += sizeof(configuration->embedded_out_jack);
    memcpy(cursor, &configuration->external_out_jack, sizeof(configuration->external_out_jack));
    /* memset above leaves interfaces[2] as Sony's source-record sentinel. */

    init_endpoint(
        &set->endpoints[0],
        set->endpoint_extensions[0],
        &configuration->bulk_in_endpoint,
        &configuration->bulk_in_class);
    init_endpoint(
        &set->endpoints[1],
        set->endpoint_extensions[1],
        &configuration->bulk_out_endpoint,
        &configuration->bulk_out_class);
    /* memset above leaves endpoints[2] as the firmware-required terminator. */

    set->interface_groups[0].first_alternate = &set->interfaces[0];
    set->interface_groups[0].alternate_count = 1;
    set->interface_groups[1].first_alternate = &set->interfaces[1];
    set->interface_groups[1].alternate_count = 1;

    set->link.configuration = &set->configuration.descriptor;
    set->link.interface_groups = set->interface_groups;
    set->link.interfaces = set->interfaces;
    set->link.endpoints = set->endpoints;

    return UsbMidiPspTransport_Validate(set, configuration);
}

int UsbMidiPspTransport_Compose(
    const UsbMidiPspDescriptorSet *set,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    size_t offset = 0U;
    uint32_t group_index;

    if (set == NULL || output == NULL || output_length == NULL
        || set->link.configuration != &set->configuration.descriptor
        || set->link.interface_groups != set->interface_groups
        || set->configuration.interface_groups == NULL) {
        return -1;
    }
    if (append_bytes(
            output,
            output_capacity,
            &offset,
            &set->configuration.descriptor,
            set->configuration.descriptor.bLength) < 0) {
        return -1;
    }

    for (group_index = 0; group_index < set->configuration.descriptor.bNumInterfaces; ++group_index) {
        const UsbMidiPspInterfaceGroup *group =
            &set->configuration.interface_groups[group_index];
        uint32_t alternate_index;

        if (group->first_alternate == NULL || group->alternate_count == 0U
            || group->alternate_count > 2U) {
            return -1;
        }
        for (alternate_index = 0; alternate_index < group->alternate_count; ++alternate_index) {
            const UsbMidiPspInterfaceRecord *interface_record =
                &group->first_alternate[alternate_index];
            uint8_t endpoint_index;

            if (interface_record == NULL
                || interface_record->descriptor.bLength != sizeof(UsbMidiInterfaceDescriptor)
                || append_bytes(
                    output,
                    output_capacity,
                    &offset,
                    &interface_record->descriptor,
                    interface_record->descriptor.bLength) < 0
                || append_bytes(
                    output,
                    output_capacity,
                    &offset,
                    interface_record->extension,
                    interface_record->extension_length) < 0) {
                return -1;
            }

            for (endpoint_index = 0;
                 endpoint_index < interface_record->descriptor.bNumEndpoints;
                 ++endpoint_index) {
                const UsbMidiPspEndpointRecord *endpoint =
                    &interface_record->endpoints[endpoint_index];

                if (endpoint->descriptor.bLength < sizeof(UsbMidiPspEndpointPrefix)
                    || append_bytes(
                        output,
                        output_capacity,
                        &offset,
                        &endpoint->descriptor,
                        sizeof(endpoint->descriptor)) < 0
                    || append_bytes(
                        output,
                        output_capacity,
                        &offset,
                        endpoint->extension,
                        endpoint->extension_length) < 0) {
                    return -1;
                }
            }
        }
    }

    *output_length = offset;
    return offset == set->configuration.descriptor.wTotalLength ? 0 : -1;
}

int UsbMidiPspTransport_Validate(
    const UsbMidiPspDescriptorSet *set,
    const UsbMidiConfigurationDescriptor *configuration)
{
    uint8_t composed[USB_MIDI_CONFIGURATION_TOTAL_LENGTH];
    size_t composed_length = 0U;

    if (set == NULL || configuration == NULL
        || set->configuration.descriptor.bNumInterfaces != USB_MIDI_PSP_INTERFACE_COUNT
        || !bytes_are_zero(set->device.padding, sizeof(set->device.padding))
        || !bytes_are_zero(
            set->configuration.alignment_padding,
            sizeof(set->configuration.alignment_padding))
        || set->configuration.interface_groups != set->interface_groups
        || !bytes_are_zero(
            set->configuration.trailing_padding,
            sizeof(set->configuration.trailing_padding))
        || set->link.interfaces != set->interfaces
        || set->link.endpoints != set->endpoints
        || set->interface_groups[0].first_alternate != &set->interfaces[0]
        || set->interface_groups[1].first_alternate != &set->interfaces[1]
        || set->interface_groups[0].reserved != 0U
        || set->interface_groups[1].reserved != 0U
        || set->interface_groups[0].alternate_count != 1U
        || set->interface_groups[1].alternate_count != 1U
        || set->interfaces[0].endpoints != NULL
        || set->interfaces[0].extension != set->audio_control_extension
        || set->interfaces[0].extension_length != USB_MIDI_PSP_AC_EXTENSION_LENGTH
        || set->interfaces[1].endpoints != set->endpoints
        || set->interfaces[1].extension != set->midi_streaming_extension
        || set->interfaces[1].extension_length != USB_MIDI_PSP_MS_EXTENSION_LENGTH
        || set->interfaces[USB_MIDI_PSP_INTERFACE_COUNT].descriptor.bLength != 0U
        || set->interfaces[USB_MIDI_PSP_INTERFACE_COUNT].endpoints != NULL
        || set->interfaces[USB_MIDI_PSP_INTERFACE_COUNT].extension != NULL
        || set->interfaces[USB_MIDI_PSP_INTERFACE_COUNT].extension_length != 0U
        || set->endpoints[0].extension != set->endpoint_extensions[0]
        || set->endpoints[1].extension != set->endpoint_extensions[1]
        || (set->endpoints[0].descriptor.bEndpointAddress & 0x0FU) != 1U
        || (set->endpoints[1].descriptor.bEndpointAddress & 0x0FU) != 2U
        || set->endpoints[0].extension_length != USB_MIDI_PSP_ENDPOINT_EXTENSION_LENGTH
        || set->endpoints[1].extension_length != USB_MIDI_PSP_ENDPOINT_EXTENSION_LENGTH
        || set->endpoints[USB_MIDI_PSP_ENDPOINT_COUNT].descriptor.bLength != 0U
        || set->endpoints[USB_MIDI_PSP_ENDPOINT_COUNT].extension != NULL
        || set->endpoints[USB_MIDI_PSP_ENDPOINT_COUNT].extension_length != 0U
        || UsbMidiPspTransport_Compose(
            set,
            composed,
            sizeof(composed),
            &composed_length) < 0
        || composed_length != sizeof(*configuration)
        || memcmp(composed, configuration, sizeof(*configuration)) != 0) {
        return -1;
    }
    return 0;
}
