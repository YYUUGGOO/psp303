#include "usb_midi_packets.h"

#include <stddef.h>

_Static_assert(sizeof(UsbMidiPacket) == 4U, "USB-MIDI event packet must be four bytes");

static int encode_channel_voice(
    uint8_t cable,
    uint8_t status,
    uint8_t data1,
    uint8_t data2,
    UsbMidiPacket *packet)
{
    uint8_t code_index_number = (uint8_t)(status >> 4U);

    if (data1 > 0x7FU) {
        return -1;
    }
    if (code_index_number == 0x0CU || code_index_number == 0x0DU) {
        data2 = 0U;
    } else if (data2 > 0x7FU) {
        return -1;
    }

    packet->header = (uint8_t)((cable << 4U) | code_index_number);
    packet->midi0 = status;
    packet->midi1 = data1;
    packet->midi2 = data2;
    return 0;
}

static int encode_system_message(
    uint8_t cable,
    uint8_t status,
    uint8_t data1,
    uint8_t data2,
    UsbMidiPacket *packet)
{
    uint8_t code_index_number;

    switch (status) {
        case 0xF1U:
        case 0xF3U:
            if (data1 > 0x7FU) {
                return -1;
            }
            code_index_number = 0x02U;
            data2 = 0U;
            break;
        case 0xF2U:
            if (data1 > 0x7FU || data2 > 0x7FU) {
                return -1;
            }
            code_index_number = 0x03U;
            break;
        case 0xF6U:
            code_index_number = 0x05U;
            data1 = 0U;
            data2 = 0U;
            break;
        case 0xF8U:
        case 0xFAU:
        case 0xFBU:
        case 0xFCU:
        case 0xFEU:
        case 0xFFU:
            code_index_number = 0x0FU;
            data1 = 0U;
            data2 = 0U;
            break;
        default:
            return -1;
    }

    packet->header = (uint8_t)((cable << 4U) | code_index_number);
    packet->midi0 = status;
    packet->midi1 = data1;
    packet->midi2 = data2;
    return 0;
}

int UsbMidiPacket_EncodeShort(
    uint8_t cable,
    uint8_t status,
    uint8_t data1,
    uint8_t data2,
    UsbMidiPacket *packet)
{
    if (packet == NULL || cable > 0x0FU) {
        return -1;
    }
    if (status >= 0x80U && status <= 0xEFU) {
        return encode_channel_voice(cable, status, data1, data2, packet);
    }
    if (status >= 0xF0U) {
        return encode_system_message(cable, status, data1, data2, packet);
    }
    return -1;
}

int UsbMidiPacket_DecodeShort(
    const UsbMidiPacket *packet,
    UsbMidiShortMessage *message)
{
    uint8_t code_index_number;
    uint8_t length;

    if (packet == NULL || message == NULL) {
        return -1;
    }

    code_index_number = (uint8_t)(packet->header & 0x0FU);
    switch (code_index_number) {
        case 0x08U:
        case 0x09U:
        case 0x0AU:
        case 0x0BU:
        case 0x0EU:
            if ((packet->midi0 >> 4U) != code_index_number
                || packet->midi1 > 0x7FU || packet->midi2 > 0x7FU) {
                return -1;
            }
            length = 3U;
            break;
        case 0x0CU:
        case 0x0DU:
            if ((packet->midi0 >> 4U) != code_index_number
                || packet->midi1 > 0x7FU || packet->midi2 != 0U) {
                return -1;
            }
            length = 2U;
            break;
        case 0x02U:
            if ((packet->midi0 != 0xF1U && packet->midi0 != 0xF3U)
                || packet->midi1 > 0x7FU || packet->midi2 != 0U) {
                return -1;
            }
            length = 2U;
            break;
        case 0x03U:
            if (packet->midi0 != 0xF2U
                || packet->midi1 > 0x7FU || packet->midi2 > 0x7FU) {
                return -1;
            }
            length = 3U;
            break;
        case 0x05U:
            if (packet->midi0 != 0xF6U
                || packet->midi1 != 0U || packet->midi2 != 0U) {
                return -1;
            }
            length = 1U;
            break;
        case 0x0FU:
            if ((packet->midi0 != 0xF8U && packet->midi0 != 0xFAU
                    && packet->midi0 != 0xFBU && packet->midi0 != 0xFCU
                    && packet->midi0 != 0xFEU && packet->midi0 != 0xFFU)
                || packet->midi1 != 0U || packet->midi2 != 0U) {
                return -1;
            }
            length = 1U;
            break;
        default:
            return -1;
    }

    message->cable = (uint8_t)(packet->header >> 4U);
    message->status = packet->midi0;
    message->data1 = packet->midi1;
    message->data2 = packet->midi2;
    message->length = length;
    return 0;
}
