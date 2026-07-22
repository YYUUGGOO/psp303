#include "usb_midi_tx.h"

#include "usb_midi_descriptors.h"
#include "usb_midi_ipc.h"
#include "usb_midi_packets.h"

#include <pspkerror.h>
#include <pspthreadman_kernel.h>
#include <psputils.h>

#include <stddef.h>
#include <string.h>

#define PSP_ERROR_AS_INT(error_code) ((int)(unsigned int)(error_code))

enum {
    USB_MIDI_TX_EVENT_ATTACH = 1U << 0,
    USB_MIDI_TX_EVENT_DETACH = 1U << 1,
    USB_MIDI_TX_EVENT_COMPLETE = 1U << 2,
    USB_MIDI_TX_EVENT_STOP = 1U << 3,
    USB_MIDI_RX_EVENT_COMPLETE = 1U << 4,
    USB_MIDI_ATTACH_SETTLE_US = 2000000U,
    USB_MIDI_TX_REQUEST_TIMEOUT_US = 2000000U,
    USB_MIDI_TX_PACKET_COUNT = USB_MIDI_IPC_MAX_BATCH_EVENTS,
    USB_MIDI_TX_QUEUED_BATCH_LIMIT =
        USB_MIDI_IPC_PIPE_BYTES / sizeof(UsbMidiIpcMidiBatch),
    USB_MIDI_RX_PACKET_COUNT = 128,
    USB_MIDI_RX_ZERO_LENGTH_LIMIT = 4,
    USB_MIDI_PSP_SPEED_FULL = 1,
    USB_MIDI_PSP_SPEED_HIGH = 2
};

static SceUID g_tx_event = -1;
static SceUID g_tx_thread = -1;
static struct UsbEndpoint *g_bulk_in_endpoint = NULL;
static struct UsbEndpoint *g_bulk_out_endpoint = NULL;
static struct UsbdDeviceReq g_tx_request;
static struct UsbdDeviceReq g_rx_request;
static UsbMidiPacket g_tx_packets[USB_MIDI_TX_PACKET_COUNT]
    __attribute__((aligned(64)));
static UsbMidiPacket g_rx_packets[USB_MIDI_RX_PACKET_COUNT]
    __attribute__((aligned(64)));
_Static_assert(
    sizeof(g_tx_packets) == USB_MIDI_IPC_MAX_BATCH_EVENTS * sizeof(UsbMidiPacket),
    "transmit buffer must hold one complete IPC batch");
_Static_assert(
    sizeof(g_rx_packets) == USB_MIDI_HIGH_SPEED_PACKET_SIZE,
    "receive buffer must hold one high-speed endpoint packet");
static volatile int g_tx_started = 0;
static volatile int g_tx_attached = 0;
static volatile int g_tx_stopping = 0;
static volatile int g_tx_request_pending = 0;
static volatile int g_rx_request_pending = 0;
static volatile int g_tx_cancel_requested = 0;
static volatile int g_rx_cancel_requested = 0;
static volatile int g_tx_last_result = 0;
static volatile uint32_t g_tx_attach_generation = 0U;
static volatile int g_rx_transfer_size = 0;

static int tx_request_complete(struct UsbdDeviceReq *request, int arg2, int arg3)
{
    (void)arg2;
    (void)arg3;
    if (request != &g_tx_request) {
        return -1;
    }
    g_tx_request_pending = 0;
    if (g_tx_event < 0) {
        return -1;
    }
    return sceKernelSetEventFlag(g_tx_event, USB_MIDI_TX_EVENT_COMPLETE);
}

static int rx_request_complete(struct UsbdDeviceReq *request, int arg2, int arg3)
{
    (void)arg2;
    (void)arg3;
    if (request != &g_rx_request) {
        return -1;
    }
    g_rx_request_pending = 0;
    if (g_tx_event < 0) {
        return -1;
    }
    return sceKernelSetEventFlag(g_tx_event, USB_MIDI_RX_EVENT_COMPLETE);
}

static void set_request_callback(
    struct UsbdDeviceReq *request,
    int (*function)(struct UsbdDeviceReq *, int, int))
{
    union CallbackPointer {
        int (*function)(struct UsbdDeviceReq *, int, int);
        void *opaque;
    } callback;

    callback.function = function;
    request->func = callback.opaque;
}

static int cancel_pending_tx_request(void)
{
    int result;

    if (g_tx_request_pending == 0 || g_bulk_in_endpoint == NULL
        || g_tx_cancel_requested != 0) {
        return 0;
    }
    g_tx_cancel_requested = 1;
    result = sceUsbbdReqCancelAll(g_bulk_in_endpoint);
    if (result < 0) {
        g_tx_cancel_requested = 0;
    }
    return result;
}

static int cancel_pending_rx_request(void)
{
    int result;

    if (g_rx_request_pending == 0 || g_bulk_out_endpoint == NULL
        || g_rx_cancel_requested != 0) {
        return 0;
    }
    g_rx_cancel_requested = 1;
    result = sceUsbbdReqCancelAll(g_bulk_out_endpoint);
    if (result < 0) {
        g_rx_cancel_requested = 0;
    }
    return result;
}

static int wait_for_tx_request(int expected_size)
{
    SceUInt timeout = USB_MIDI_TX_REQUEST_TIMEOUT_US;
    uint32_t matched = 0U;
    int result = sceKernelWaitEventFlag(
        g_tx_event,
        USB_MIDI_TX_EVENT_COMPLETE | USB_MIDI_TX_EVENT_DETACH | USB_MIDI_TX_EVENT_STOP,
        PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
        &matched,
        &timeout);

    if (result < 0 || (matched & USB_MIDI_TX_EVENT_COMPLETE) == 0U) {
        int cancel_result = cancel_pending_tx_request();

        if (cancel_result < 0) {
            return cancel_result;
        }
        if (g_tx_request_pending != 0) {
            timeout = USB_MIDI_TX_REQUEST_TIMEOUT_US;
            result = sceKernelWaitEventFlag(
                g_tx_event,
                USB_MIDI_TX_EVENT_COMPLETE,
                PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
                &matched,
                &timeout);
            if (result < 0) {
                g_tx_cancel_requested = 0;
                return result;
            }
        }
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
    }

    if (g_tx_request.retcode != 0
        || g_tx_request.recvsize != expected_size) {
        return g_tx_request.retcode != 0
            ? g_tx_request.retcode
            : PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
    }
    return 0;
}

static int send_packets(int packet_count)
{
    int result;
    int request_size;

    if (g_bulk_in_endpoint == NULL || g_tx_request_pending != 0
        || g_tx_attached == 0 || g_tx_stopping != 0
        || packet_count < 1 || packet_count > USB_MIDI_TX_PACKET_COUNT) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
    }
    request_size = packet_count * (int)sizeof(g_tx_packets[0]);

    memset(&g_tx_request, 0, sizeof(g_tx_request));
    g_tx_request.endp = g_bulk_in_endpoint;
    g_tx_request.data = g_tx_packets;
    g_tx_request.size = request_size;
    set_request_callback(&g_tx_request, tx_request_complete);
    result = sceKernelClearEventFlag(g_tx_event, ~USB_MIDI_TX_EVENT_COMPLETE);
    if (result < 0) {
        return result;
    }
    sceKernelDcacheWritebackRange(g_tx_packets, (unsigned int)request_size);
    g_tx_cancel_requested = 0;
    g_tx_request_pending = 1;
    result = sceUsbbdReqSend(&g_tx_request);
    if (result < 0) {
        g_tx_request_pending = 0;
        return result;
    }
    return wait_for_tx_request(request_size);
}

static int drain_queued_public_events(void)
{
    unsigned int batch_count;

    for (batch_count = 0U;
         batch_count < USB_MIDI_TX_QUEUED_BATCH_LIMIT;
         ++batch_count) {
        UsbMidiIpcMidiBatch batch;
        uint32_t index;
        int result = UsbMidiIpc_TryReceiveTxBatch(&batch);

        if (result <= 0) {
            return result;
        }
        for (index = 0U; index < batch.count; ++index) {
            UsbMidiIpcEventData *event = &batch.events[index];

            if (UsbMidiPacket_EncodeShort(
                    event->cable,
                    event->status,
                    event->data1,
                    event->data2,
                    &g_tx_packets[index]) < 0) {
                return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
            }
        }
        result = send_packets((int)batch.count);
        if (result < 0) {
            return result;
        }
    }
    return 0;
}

static int receive_events(int transfer_size)
{
    unsigned int zero_length_count = 0U;
    int result;

    if ((transfer_size != USB_MIDI_FULL_SPEED_PACKET_SIZE
            && transfer_size != USB_MIDI_HIGH_SPEED_PACKET_SIZE)
        || g_bulk_out_endpoint == NULL || g_rx_request_pending != 0
        || g_tx_attached == 0 || g_tx_stopping != 0) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
    }

    for (;;) {
        uint32_t matched = 0U;

        memset(g_rx_packets, 0, (size_t)transfer_size);
        memset(&g_rx_request, 0, sizeof(g_rx_request));
        g_rx_request.endp = g_bulk_out_endpoint;
        g_rx_request.data = g_rx_packets;
        g_rx_request.size = transfer_size;
        set_request_callback(&g_rx_request, rx_request_complete);
        result = sceKernelClearEventFlag(g_tx_event, ~USB_MIDI_RX_EVENT_COMPLETE);
        if (result < 0) {
            return result;
        }
        sceKernelDcacheInvalidateRange(g_rx_packets, (unsigned int)transfer_size);
        g_rx_cancel_requested = 0;
        g_rx_request_pending = 1;
        result = sceUsbbdReqRecv(&g_rx_request);
        if (result < 0) {
            g_rx_request_pending = 0;
            return result;
        }

wait_for_rx_completion:
        matched = 0U;
        result = sceKernelWaitEventFlag(
            g_tx_event,
            USB_MIDI_RX_EVENT_COMPLETE | USB_MIDI_TX_EVENT_DETACH
                | USB_MIDI_TX_EVENT_STOP | USB_MIDI_IPC_EVENT_TX_AVAILABLE,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
            &matched,
            NULL);
        if (result >= 0
            && (matched & USB_MIDI_IPC_EVENT_TX_AVAILABLE) != 0U
            && g_tx_attached != 0 && g_tx_stopping == 0) {
            result = drain_queued_public_events();
            if (result < 0) {
                return result;
            }
            if ((matched & (USB_MIDI_RX_EVENT_COMPLETE
                    | USB_MIDI_TX_EVENT_DETACH | USB_MIDI_TX_EVENT_STOP)) == 0U) {
                goto wait_for_rx_completion;
            }
        }
        if (result < 0
            || (matched & USB_MIDI_RX_EVENT_COMPLETE) == 0U
            || (matched & (USB_MIDI_TX_EVENT_DETACH
                    | USB_MIDI_TX_EVENT_STOP)) != 0U) {
            SceUInt timeout = USB_MIDI_TX_REQUEST_TIMEOUT_US;
            int cancel_result = cancel_pending_rx_request();

            if (cancel_result < 0) {
                return cancel_result;
            }
            if (g_rx_request_pending != 0) {
                result = sceKernelWaitEventFlag(
                    g_tx_event,
                    USB_MIDI_RX_EVENT_COMPLETE,
                    PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
                    &matched,
                    &timeout);
                if (result < 0) {
                    g_rx_cancel_requested = 0;
                    return result;
                }
            }
            return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
        }

        if (g_rx_request.retcode != 0) {
            return g_rx_request.retcode;
        }
        if (g_rx_request.recvsize == 0) {
            ++zero_length_count;
            if (zero_length_count > USB_MIDI_RX_ZERO_LENGTH_LIMIT) {
                return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
            }
            continue;
        }
        zero_length_count = 0U;
        if (g_rx_request.recvsize < 0
            || g_rx_request.recvsize > transfer_size
            || (g_rx_request.recvsize % (int)sizeof(g_rx_packets[0])) != 0) {
            return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
        }

        {
            int packet_count = g_rx_request.recvsize / (int)sizeof(g_rx_packets[0]);
            int packet_index;

            sceKernelDcacheInvalidateRange(g_rx_packets, (unsigned int)transfer_size);
            for (packet_index = 0; packet_index < packet_count; ++packet_index) {
                UsbMidiShortMessage message;

                if (UsbMidiPacket_DecodeShort(&g_rx_packets[packet_index], &message) < 0) {
                    continue;
                }
                result = UsbMidiIpc_SendRxEvent(
                    sceKernelGetSystemTimeLow(),
                    message.cable,
                    message.status,
                    message.data1,
                    message.data2);
                if (result < 0) {
                    return result;
                }
            }
        }
    }
}

static int tx_worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    while (g_tx_stopping == 0) {
        uint32_t matched = 0U;
        int result = sceKernelWaitEventFlag(
            g_tx_event,
            USB_MIDI_TX_EVENT_ATTACH | USB_MIDI_TX_EVENT_DETACH
                | USB_MIDI_TX_EVENT_STOP | USB_MIDI_IPC_EVENT_TX_AVAILABLE,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
            &matched,
            NULL);

        if (result < 0) {
            g_tx_last_result = result;
            break;
        }
        if ((matched & USB_MIDI_TX_EVENT_STOP) != 0U || g_tx_stopping != 0) {
            break;
        }
        if ((matched & USB_MIDI_TX_EVENT_DETACH) != 0U) {
            continue;
        }
        if ((matched & USB_MIDI_IPC_EVENT_TX_AVAILABLE) != 0U
            && (matched & USB_MIDI_TX_EVENT_ATTACH) == 0U
            && g_tx_attached != 0 && g_tx_stopping == 0) {
            g_tx_last_result = drain_queued_public_events();
            if (g_tx_last_result < 0) {
                break;
            }
        }
        if ((matched & USB_MIDI_TX_EVENT_ATTACH) != 0U
            && g_tx_attached != 0) {
            uint32_t attach_generation = g_tx_attach_generation;

            result = sceKernelDelayThread(USB_MIDI_ATTACH_SETTLE_US);
            if (result < 0) {
                g_tx_last_result = result;
                break;
            }
            if (g_tx_attached != 0 && g_tx_stopping == 0
                && attach_generation == g_tx_attach_generation) {
                g_tx_last_result = drain_queued_public_events();
                if (g_tx_last_result == 0 && g_tx_attached != 0
                    && g_tx_stopping == 0
                    && attach_generation == g_tx_attach_generation) {
                    g_tx_last_result = receive_events(g_rx_transfer_size);
                }
            }
        }
    }

    /* Transfer failures must never become a USB-driver stop failure. */
    return sceKernelExitThread(0);
}

int UsbMidiTx_Start(
    struct UsbEndpoint *bulk_in_endpoint,
    struct UsbEndpoint *bulk_out_endpoint)
{
    int result;

    if (bulk_in_endpoint == NULL || bulk_in_endpoint->endpnum != 1
        || bulk_out_endpoint == NULL || bulk_out_endpoint->endpnum != 2
        || g_tx_started != 0 || g_tx_event >= 0 || g_tx_thread >= 0) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
    }
    memset(&g_tx_request, 0, sizeof(g_tx_request));
    memset(&g_rx_request, 0, sizeof(g_rx_request));
    memset(g_tx_packets, 0, sizeof(g_tx_packets));
    memset(g_rx_packets, 0, sizeof(g_rx_packets));
    g_bulk_in_endpoint = bulk_in_endpoint;
    g_bulk_out_endpoint = bulk_out_endpoint;
    g_tx_attached = 0;
    g_tx_stopping = 0;
    g_tx_request_pending = 0;
    g_rx_request_pending = 0;
    g_tx_cancel_requested = 0;
    g_rx_cancel_requested = 0;
    g_tx_last_result = 0;
    g_tx_attach_generation = 0U;
    g_rx_transfer_size = 0;
    g_tx_event = UsbMidiIpc_GetEventFlag();
    if (g_tx_event < 0) {
        g_tx_event = -1;
        g_bulk_in_endpoint = NULL;
        g_bulk_out_endpoint = NULL;
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
    }
    result = sceKernelClearEventFlag(g_tx_event, 0U);
    if (result < 0) {
        g_tx_event = -1;
        g_bulk_in_endpoint = NULL;
        g_bulk_out_endpoint = NULL;
        return result;
    }
    g_tx_thread = sceKernelCreateThread("UsbMidiTxWorker", tx_worker, 0x18, 0x1000, 0, NULL);
    if (g_tx_thread < 0) {
        result = g_tx_thread;
        g_tx_thread = -1;
        g_tx_event = -1;
        g_bulk_in_endpoint = NULL;
        g_bulk_out_endpoint = NULL;
        return result;
    }
    g_tx_started = 1;
    result = sceKernelStartThread(g_tx_thread, 0, NULL);
    if (result < 0) {
        int cleanup_result = sceKernelDeleteThread(g_tx_thread);

        g_tx_started = 0;
        if (cleanup_result >= 0) {
            g_tx_thread = -1;
            g_tx_event = -1;
            g_bulk_in_endpoint = NULL;
            g_bulk_out_endpoint = NULL;
        }
        return cleanup_result < 0 ? cleanup_result : result;
    }
    return 0;
}

int UsbMidiTx_OnAttach(int speed)
{
    if ((speed != USB_MIDI_PSP_SPEED_FULL && speed != USB_MIDI_PSP_SPEED_HIGH)
        || g_tx_started == 0 || g_tx_event < 0 || g_tx_stopping != 0) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
    }
    g_rx_transfer_size = speed == USB_MIDI_PSP_SPEED_HIGH
        ? USB_MIDI_HIGH_SPEED_PACKET_SIZE
        : USB_MIDI_FULL_SPEED_PACKET_SIZE;
    ++g_tx_attach_generation;
    g_tx_attached = 1;
    return sceKernelSetEventFlag(g_tx_event, USB_MIDI_TX_EVENT_ATTACH);
}

int UsbMidiTx_OnDetach(void)
{
    if (g_tx_started == 0 || g_tx_event < 0) {
        return 0;
    }
    ++g_tx_attach_generation;
    g_tx_attached = 0;
    g_rx_transfer_size = 0;
    return sceKernelSetEventFlag(g_tx_event, USB_MIDI_TX_EVENT_DETACH);
}

int UsbMidiTx_Stop(void)
{
    int result;

    if (g_tx_started == 0) {
        if (g_tx_thread >= 0) {
            result = sceKernelDeleteThread(g_tx_thread);
            if (result < 0) {
                return result;
            }
            g_tx_thread = -1;
        }
        g_tx_event = -1;
        g_bulk_in_endpoint = NULL;
        g_bulk_out_endpoint = NULL;
        return 0;
    }
    g_tx_stopping = 1;
    g_tx_attached = 0;
    result = sceKernelSetEventFlag(g_tx_event, USB_MIDI_TX_EVENT_STOP);
    if (result < 0) {
        return result;
    }
    result = cancel_pending_tx_request();
    if (result < 0) {
        return result;
    }
    result = cancel_pending_rx_request();
    if (result < 0) {
        return result;
    }
    /* Match Sony's 6.61 USB microphone worker teardown sequence. */
    result = sceKernelWaitThreadEnd(g_tx_thread, NULL);
    if (result < 0 || g_tx_request_pending != 0 || g_rx_request_pending != 0) {
        if (g_tx_request_pending != 0) {
            g_tx_cancel_requested = 0;
        }
        if (g_rx_request_pending != 0) {
            g_rx_cancel_requested = 0;
        }
        return result < 0 ? result : PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
    }
    result = sceKernelDeleteThread(g_tx_thread);
    if (result < 0) {
        return result;
    }
    g_tx_thread = -1;
    g_tx_started = 0;
    g_tx_event = -1;
    g_bulk_in_endpoint = NULL;
    g_bulk_out_endpoint = NULL;
    return 0;
}
