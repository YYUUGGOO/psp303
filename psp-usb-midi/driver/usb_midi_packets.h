#ifndef USB_MIDI_PACKETS_H
#define USB_MIDI_PACKETS_H

#include <stdint.h>

/** One USB-MIDI 1.0 event packet. */
typedef struct UsbMidiPacket {
    uint8_t header;
    uint8_t midi0;
    uint8_t midi1;
    uint8_t midi2;
} UsbMidiPacket;

/** A validated complete non-SysEx MIDI message. */
typedef struct UsbMidiShortMessage {
    uint8_t cable;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t length;
} UsbMidiShortMessage;

/**
 * Encode one complete non-SysEx MIDI message into a USB-MIDI event packet.
 *
 * Unused data bytes are canonicalized to zero. SysEx is intentionally deferred
 * until the receive/transmit stream state machine is introduced.
 *
 * @return 0 on success, otherwise -1 for an invalid cable, status, or data byte.
 */
int UsbMidiPacket_EncodeShort(
    uint8_t cable,
    uint8_t status,
    uint8_t data1,
    uint8_t data2,
    UsbMidiPacket *packet);

/**
 * Decode one complete non-SysEx USB-MIDI event packet.
 *
 * The CIN, status byte, data-byte ranges, and unused zero bytes must all agree.
 * SysEx and reserved CIN values are rejected because they require stream state.
 *
 * @return 0 on success, otherwise -1 for a malformed or unsupported packet.
 */
int UsbMidiPacket_DecodeShort(
    const UsbMidiPacket *packet,
    UsbMidiShortMessage *message);

#endif
