#include "usb_midi_ipc.h"

#include "usb_midi_ipc_protocol.h"
#include "usb_midi_packets.h"

#include <pspkernel.h>
#include <pspkerror.h>

#include <stddef.h>

#define PSP_ERROR_AS_INT(error_code) ((int)(unsigned int)(error_code))

static SceUID g_user_to_kernel_pipe = -1;
static SceUID g_kernel_to_user_pipe = -1;
static SceUID g_event_flag = -1;
static uint32_t g_sequence = 0U;
static uint32_t g_next_rx_sequence = 0U;
static uint32_t g_next_tx_sequence = 0U;

int UsbMidiIpc_Configure(SceSize args, void *argp)
{
    const UsbMidiIpcStartup *startup = argp;
    UsbMidiIpcMessage message;
    int result;

    if (args != sizeof(*startup) || startup == NULL
        || startup->size != sizeof(*startup)
        || startup->magic != USB_MIDI_IPC_MAGIC
        || startup->version != USB_MIDI_IPC_VERSION
        || startup->user_to_kernel_pipe < 0
        || startup->kernel_to_user_pipe < 0
        || startup->event_flag < 0) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }

    result = sceKernelTryReceiveMsgPipe(
        startup->user_to_kernel_pipe,
        &message,
        sizeof(message),
        0,
        NULL);
    if (result < 0) {
        return result;
    }
    if (message.magic != USB_MIDI_IPC_MAGIC
        || message.version != USB_MIDI_IPC_VERSION
        || message.kind != USB_MIDI_IPC_MESSAGE_HELLO) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }

    g_user_to_kernel_pipe = startup->user_to_kernel_pipe;
    g_kernel_to_user_pipe = startup->kernel_to_user_pipe;
    g_event_flag = startup->event_flag;
    g_sequence = message.sequence;
    g_next_rx_sequence = message.sequence + 1U;
    g_next_tx_sequence = message.sequence + 1U;
    return 0;
}

int UsbMidiIpc_Acknowledge(void)
{
    UsbMidiIpcMessage message;

    if (g_user_to_kernel_pipe < 0 || g_kernel_to_user_pipe < 0) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    message.magic = USB_MIDI_IPC_MAGIC;
    message.version = USB_MIDI_IPC_VERSION;
    message.kind = USB_MIDI_IPC_MESSAGE_ACK;
    message.sequence = g_sequence;
    return sceKernelTrySendMsgPipe(
        g_kernel_to_user_pipe,
        &message,
        sizeof(message),
        0,
        NULL);
}

int UsbMidiIpc_TryReceiveTxBatch(UsbMidiIpcMidiBatch *batch)
{
    uint32_t index;
    int result;

    if (batch == NULL || g_user_to_kernel_pipe < 0
        || g_kernel_to_user_pipe < 0 || g_event_flag < 0) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    result = sceKernelTryReceiveMsgPipe(
        g_user_to_kernel_pipe,
        batch,
        sizeof(*batch),
        0,
        NULL);
    if (result == PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_MPP_EMPTY)) {
        return 0;
    }
    if (result < 0) {
        return result;
    }
    if (batch->magic != USB_MIDI_IPC_MAGIC
        || batch->version != USB_MIDI_IPC_VERSION
        || batch->kind != USB_MIDI_IPC_MESSAGE_MIDI_TX_BATCH
        || batch->sequence != g_next_tx_sequence
        || batch->count == 0U
        || batch->count > USB_MIDI_IPC_MAX_BATCH_EVENTS) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    for (index = 0U; index < batch->count; ++index) {
        UsbMidiPacket packet;
        UsbMidiIpcEventData *event = &batch->events[index];

        if (UsbMidiPacket_EncodeShort(
                event->cable,
                event->status,
                event->data1,
                event->data2,
                &packet) < 0) {
            return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
        }
        event->cable = (uint8_t)(packet.header >> 4U);
        event->status = packet.midi0;
        event->data1 = packet.midi1;
        event->data2 = packet.midi2;
    }
    ++g_next_tx_sequence;
    return 1;
}

int UsbMidiIpc_SendRxEvent(
    uint32_t timestamp_us,
    uint8_t cable,
    uint8_t status,
    uint8_t data1,
    uint8_t data2)
{
    UsbMidiIpcMidiEvent event;
    UsbMidiPacket packet;
    int result;

    if (g_user_to_kernel_pipe < 0 || g_kernel_to_user_pipe < 0
        || g_next_rx_sequence == 0U
        || UsbMidiPacket_EncodeShort(
            cable, status, data1, data2, &packet) < 0) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    event.magic = USB_MIDI_IPC_MAGIC;
    event.version = USB_MIDI_IPC_VERSION;
    event.kind = USB_MIDI_IPC_MESSAGE_MIDI_RX;
    event.sequence = g_next_rx_sequence;
    event.timestamp_us = timestamp_us;
    event.cable = (uint8_t)(packet.header >> 4U);
    event.status = packet.midi0;
    event.data1 = packet.midi1;
    event.data2 = packet.midi2;
    result = sceKernelTrySendMsgPipe(
        g_kernel_to_user_pipe,
        &event,
        sizeof(event),
        0,
        NULL);
    if (result == PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_MPP_FULL)) {
        return 0;
    }
    if (result == 0) {
        ++g_next_rx_sequence;
    }
    return result;
}

SceUID UsbMidiIpc_GetEventFlag(void)
{
    return g_event_flag;
}

void UsbMidiIpc_Reset(void)
{
    g_user_to_kernel_pipe = -1;
    g_kernel_to_user_pipe = -1;
    g_event_flag = -1;
    g_sequence = 0U;
    g_next_rx_sequence = 0U;
    g_next_tx_sequence = 0U;
}
