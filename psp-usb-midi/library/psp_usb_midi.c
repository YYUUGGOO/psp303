#include "psp_usb_midi.h"

#include "usb_midi_state.h"
#include "usb_midi_ipc_protocol.h"
#include "usb_midi_packets.h"

#include <pspkernel.h>
#include <pspkerror.h>
#include <pspmodulemgr.h>
#include <pspusb.h>

#include <stddef.h>
#include <string.h>

#define USB_MIDI_DRIVER_NAME "UsbMidiDriver"
#define USB_MIDI_PRODUCT_ID 0x01C9U
#define PSP_ERROR_AS_INT(error_code) ((int)(unsigned int)(error_code))

typedef enum ModuleLifecycle {
    MODULE_NOT_LOADED = 0,
    MODULE_LOADED,
    MODULE_REGISTERED
} ModuleLifecycle;

typedef enum UsbLifecycle {
    USB_STOPPED = 0,
    USB_BUS_STARTED,
    USB_DRIVER_STARTED,
    USB_ACTIVATED
} UsbLifecycle;

typedef struct UsbMidiContext {
    SceUID module_id;
    SceUID lock;
    SceUID user_to_kernel_pipe;
    SceUID kernel_to_user_pipe;
    SceUID event_flag;
    ModuleLifecycle module_lifecycle;
    UsbLifecycle usb_lifecycle;
    UsbMidiState state;
    int last_result;
    int was_connected;
    int ipc_ready;
    uint32_t next_tx_sequence;
    uint32_t next_rx_sequence;
} UsbMidiContext;

static UsbMidiContext g_context = {
    -1,
    -1,
    -1,
    -1,
    -1,
    MODULE_NOT_LOADED,
    USB_STOPPED,
    USB_MIDI_STATE_UNINITIALIZED,
    0,
    0,
    0,
    0U,
    0U
};

static int destroy_ipc_locked(void)
{
    int result = 0;

    g_context.ipc_ready = 0;
    g_context.next_tx_sequence = 0U;
    g_context.next_rx_sequence = 0U;
    if (g_context.kernel_to_user_pipe >= 0) {
        int delete_result = sceKernelDeleteMsgPipe(g_context.kernel_to_user_pipe);

        if (delete_result >= 0) {
            g_context.kernel_to_user_pipe = -1;
        } else {
            result = delete_result;
        }
    }
    if (g_context.user_to_kernel_pipe >= 0) {
        int delete_result = sceKernelDeleteMsgPipe(g_context.user_to_kernel_pipe);

        if (delete_result >= 0) {
            g_context.user_to_kernel_pipe = -1;
        } else if (result == 0) {
            result = delete_result;
        }
    }
    if (g_context.event_flag >= 0) {
        int delete_result = sceKernelDeleteEventFlag(g_context.event_flag);

        if (delete_result >= 0) {
            g_context.event_flag = -1;
        } else if (result == 0) {
            result = delete_result;
        }
    }
    return result;
}

static int create_ipc_locked(UsbMidiIpcStartup *startup)
{
    UsbMidiIpcMessage hello;
    int result;

    if (startup == NULL || g_context.user_to_kernel_pipe >= 0
        || g_context.kernel_to_user_pipe >= 0 || g_context.event_flag >= 0) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    g_context.user_to_kernel_pipe = sceKernelCreateMsgPipe(
        "UsbMidiUserToKernel",
        PSP_MEMORY_PARTITION_USER,
        0,
        (void *)(uintptr_t)USB_MIDI_IPC_PIPE_BYTES,
        NULL);
    if (g_context.user_to_kernel_pipe < 0) {
        result = g_context.user_to_kernel_pipe;
        g_context.user_to_kernel_pipe = -1;
        return result;
    }
    g_context.kernel_to_user_pipe = sceKernelCreateMsgPipe(
        "UsbMidiKernelToUser",
        PSP_MEMORY_PARTITION_USER,
        0,
        (void *)(uintptr_t)USB_MIDI_IPC_PIPE_BYTES,
        NULL);
    if (g_context.kernel_to_user_pipe < 0) {
        int cleanup_result;

        result = g_context.kernel_to_user_pipe;
        g_context.kernel_to_user_pipe = -1;
        cleanup_result = destroy_ipc_locked();
        return cleanup_result < 0 ? cleanup_result : result;
    }
    g_context.event_flag = sceKernelCreateEventFlag(
        "UsbMidiIpcEvent",
        0,
        0,
        NULL);
    if (g_context.event_flag < 0) {
        int cleanup_result;

        result = g_context.event_flag;
        g_context.event_flag = -1;
        cleanup_result = destroy_ipc_locked();
        return cleanup_result < 0 ? cleanup_result : result;
    }

    hello.magic = USB_MIDI_IPC_MAGIC;
    hello.version = USB_MIDI_IPC_VERSION;
    hello.kind = USB_MIDI_IPC_MESSAGE_HELLO;
    hello.sequence = 1U;
    result = sceKernelTrySendMsgPipe(
        g_context.user_to_kernel_pipe,
        &hello,
        sizeof(hello),
        0,
        NULL);
    if (result < 0) {
        int cleanup_result = destroy_ipc_locked();

        return cleanup_result < 0 ? cleanup_result : result;
    }

    startup->size = sizeof(*startup);
    startup->magic = USB_MIDI_IPC_MAGIC;
    startup->version = USB_MIDI_IPC_VERSION;
    startup->user_to_kernel_pipe = g_context.user_to_kernel_pipe;
    startup->kernel_to_user_pipe = g_context.kernel_to_user_pipe;
    startup->event_flag = g_context.event_flag;
    return 0;
}

static int receive_ipc_ack_locked(void)
{
    UsbMidiIpcMessage message;
    int result = sceKernelTryReceiveMsgPipe(
        g_context.kernel_to_user_pipe,
        &message,
        sizeof(message),
        0,
        NULL);

    if (result < 0) {
        return result;
    }
    if (message.magic != USB_MIDI_IPC_MAGIC
        || message.version != USB_MIDI_IPC_VERSION
        || message.kind != USB_MIDI_IPC_MESSAGE_ACK
        || message.sequence != 1U) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    g_context.ipc_ready = 1;
    g_context.next_tx_sequence = message.sequence + 1U;
    g_context.next_rx_sequence = message.sequence + 1U;
    return 0;
}

static int ensure_lock(void)
{
    if (g_context.lock >= 0) {
        return 0;
    }
    g_context.lock = sceKernelCreateSema("UsbMidiUserLock", 0, 1, 1, NULL);
    if (g_context.lock < 0) {
        int result = g_context.lock;

        g_context.lock = -1;
        return result;
    }
    return 0;
}

static int lock_context(void)
{
    int result = ensure_lock();

    return result < 0 ? result : sceKernelWaitSema(g_context.lock, 1, NULL);
}

static int unlock_context(int operation_result)
{
    int result = sceKernelSignalSema(g_context.lock, 1);

    g_context.last_result = operation_result != 0 ? operation_result : result;
    return g_context.last_result;
}

static int unlock_context_without_recording(int operation_result)
{
    int result = sceKernelSignalSema(g_context.lock, 1);

    return operation_result != 0 ? operation_result : result;
}

static int unlock_context_with_count(int count)
{
    int result = sceKernelSignalSema(g_context.lock, 1);

    return result < 0 ? result : count;
}

static int transition_state(UsbMidiState next_state)
{
    return UsbMidiState_Transition(&g_context.state, next_state) < 0
        ? PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT)
        : 0;
}

static UsbMidiState active_state_from_usb(int raw_usb_state)
{
    if ((raw_usb_state & PSP_USB_CONNECTION_ESTABLISHED) != 0) {
        g_context.was_connected = 1;
        return USB_MIDI_STATE_CONNECTED;
    }
    if (g_context.was_connected != 0) {
        return USB_MIDI_STATE_DISCONNECTED;
    }
    return USB_MIDI_STATE_ACTIVE;
}

static UsbMidiState state_from_lifecycle(void)
{
    int raw_usb_state;

    if (g_context.module_lifecycle == MODULE_NOT_LOADED) {
        return g_context.state == USB_MIDI_STATE_UNINITIALIZED
            ? USB_MIDI_STATE_UNINITIALIZED
            : USB_MIDI_STATE_STOPPED;
    }
    if (g_context.module_lifecycle == MODULE_LOADED) {
        return USB_MIDI_STATE_INITIALIZED;
    }
    if (g_context.usb_lifecycle == USB_DRIVER_STARTED) {
        return USB_MIDI_STATE_STARTED;
    }
    if (g_context.usb_lifecycle != USB_ACTIVATED) {
        return USB_MIDI_STATE_REGISTERED;
    }

    raw_usb_state = sceUsbGetState();
    if (raw_usb_state < 0) {
        g_context.last_result = raw_usb_state;
        return USB_MIDI_STATE_ACTIVE;
    }
    return active_state_from_usb(raw_usb_state);
}

static int restore_state_from_lifecycle(void)
{
    return transition_state(state_from_lifecycle());
}

static int stop_usb_locked(void)
{
    int result;

    if (g_context.usb_lifecycle == USB_ACTIVATED) {
        result = sceUsbDeactivate(USB_MIDI_PRODUCT_ID);
        if (result != 0) {
            return result;
        }
        g_context.usb_lifecycle = USB_DRIVER_STARTED;
    }
    if (g_context.usb_lifecycle == USB_DRIVER_STARTED) {
        result = sceUsbStop(USB_MIDI_DRIVER_NAME, 0, NULL);
        if (result != 0) {
            return result;
        }
        g_context.usb_lifecycle = USB_BUS_STARTED;
    }
    if (g_context.usb_lifecycle == USB_BUS_STARTED) {
        result = sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, NULL);
        if (result != 0) {
            return result;
        }
        g_context.usb_lifecycle = USB_STOPPED;
    }
    g_context.was_connected = 0;
    return 0;
}

static int start_step_locked(void)
{
    int result;

    switch (g_context.usb_lifecycle) {
        case USB_STOPPED:
            result = sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, NULL);
            if (result == 0) {
                g_context.usb_lifecycle = USB_BUS_STARTED;
            }
            break;
        case USB_BUS_STARTED:
            result = sceUsbStart(USB_MIDI_DRIVER_NAME, 0, NULL);
            if (result == 0) {
                g_context.usb_lifecycle = USB_DRIVER_STARTED;
                result = transition_state(USB_MIDI_STATE_STARTED);
            }
            break;
        case USB_DRIVER_STARTED:
            result = sceUsbActivate(USB_MIDI_PRODUCT_ID);
            if (result == 0) {
                g_context.usb_lifecycle = USB_ACTIVATED;
                g_context.was_connected = 0;
                result = transition_state(USB_MIDI_STATE_ACTIVE);
            }
            break;
        case USB_ACTIVATED:
        default:
            result = 0;
            break;
    }
    return result;
}

static int rollback_start_locked(int original_result)
{
    int cleanup_result;
    int restore_result;

    if (g_context.usb_lifecycle == USB_STOPPED) {
        return original_result;
    }
    cleanup_result = transition_state(USB_MIDI_STATE_STOPPING);
    if (cleanup_result == 0) {
        cleanup_result = stop_usb_locked();
    }
    restore_result = restore_state_from_lifecycle();
    if (cleanup_result != 0) {
        return cleanup_result;
    }
    return restore_result != 0 ? restore_result : original_result;
}

int UsbMidi_Init(const char *driver_path)
{
    UsbMidiIpcStartup startup;
    int module_status = 0;
    int result;
    SceUID module_id;

    if (driver_path == NULL || driver_path[0] == '\0') {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    result = lock_context();
    if (result < 0) {
        return result;
    }
    if (g_context.module_lifecycle == MODULE_REGISTERED
        && g_context.ipc_ready != 0) {
        return unlock_context(0);
    }
    if (g_context.module_lifecycle != MODULE_NOT_LOADED) {
        return unlock_context(PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT));
    }

    result = create_ipc_locked(&startup);
    if (result < 0) {
        return unlock_context(result);
    }

    module_id = sceKernelLoadModule(driver_path, 0, NULL);
    if (module_id < 0) {
        int cleanup_result = destroy_ipc_locked();

        return unlock_context(cleanup_result < 0 ? cleanup_result : module_id);
    }
    g_context.module_id = module_id;
    g_context.module_lifecycle = MODULE_LOADED;
    result = transition_state(USB_MIDI_STATE_INITIALIZED);
    if (result < 0) {
        int original_result = result;
        int cleanup_result = sceKernelUnloadModule(module_id);

        if (cleanup_result >= 0) {
            g_context.module_id = -1;
            g_context.module_lifecycle = MODULE_NOT_LOADED;
            cleanup_result = destroy_ipc_locked();
        }
        return unlock_context(cleanup_result < 0 ? cleanup_result : original_result);
    }

    result = sceKernelStartModule(
        module_id,
        sizeof(startup),
        &startup,
        &module_status,
        NULL);
    if (result < 0 || module_status < 0) {
        int original_result = result < 0 ? result : module_status;
        int cleanup_result = sceKernelUnloadModule(module_id);

        if (cleanup_result >= 0) {
            g_context.module_id = -1;
            g_context.module_lifecycle = MODULE_NOT_LOADED;
            (void)transition_state(USB_MIDI_STATE_UNINITIALIZED);
            cleanup_result = destroy_ipc_locked();
        }
        return unlock_context(cleanup_result < 0 ? cleanup_result : original_result);
    }
    g_context.module_lifecycle = MODULE_REGISTERED;
    result = receive_ipc_ack_locked();
    if (result < 0) {
        int original_result = result;
        int cleanup_result = sceKernelStopModule(
            module_id, 0, NULL, &module_status, NULL);

        if (cleanup_result < 0 || module_status < 0) {
            cleanup_result = cleanup_result < 0 ? cleanup_result : module_status;
            (void)transition_state(USB_MIDI_STATE_REGISTERED);
            return unlock_context(cleanup_result);
        }
        g_context.module_lifecycle = MODULE_LOADED;
        cleanup_result = sceKernelUnloadModule(module_id);
        if (cleanup_result < 0) {
            (void)transition_state(USB_MIDI_STATE_INITIALIZED);
            return unlock_context(cleanup_result);
        }
        g_context.module_id = -1;
        g_context.module_lifecycle = MODULE_NOT_LOADED;
        (void)transition_state(USB_MIDI_STATE_UNINITIALIZED);
        cleanup_result = destroy_ipc_locked();
        return unlock_context(cleanup_result < 0 ? cleanup_result : original_result);
    }
    result = transition_state(USB_MIDI_STATE_REGISTERED);
    return unlock_context(result);
}

int UsbMidi_Start(void)
{
    int result = lock_context();

    if (result < 0) {
        return result;
    }
    if (g_context.module_lifecycle != MODULE_REGISTERED
        || g_context.ipc_ready == 0) {
        return unlock_context(PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT));
    }

    while (g_context.usb_lifecycle != USB_ACTIVATED) {
        result = start_step_locked();
        if (result != 0) {
            result = rollback_start_locked(result);
            break;
        }
    }
    return unlock_context(result);
}

int UsbMidi_Stop(void)
{
    int result = lock_context();

    if (result < 0) {
        return result;
    }
    if (g_context.module_lifecycle == MODULE_NOT_LOADED) {
        return unlock_context(0);
    }
    if (g_context.usb_lifecycle == USB_STOPPED) {
        return unlock_context(restore_state_from_lifecycle());
    }

    result = transition_state(USB_MIDI_STATE_STOPPING);
    if (result == 0) {
        result = stop_usb_locked();
    }
    if (restore_state_from_lifecycle() < 0 && result == 0) {
        result = PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    return unlock_context(result);
}

int UsbMidi_Shutdown(void)
{
    int module_status = 0;
    int result = lock_context();

    if (result < 0) {
        return result;
    }
    if (g_context.module_lifecycle == MODULE_NOT_LOADED) {
        result = destroy_ipc_locked();
        if (g_context.state == USB_MIDI_STATE_UNINITIALIZED) {
            int transition_result = transition_state(USB_MIDI_STATE_STOPPED);

            if (result == 0) {
                result = transition_result;
            }
        }
        return unlock_context(result);
    }

    result = transition_state(USB_MIDI_STATE_STOPPING);
    if (result == 0) {
        result = stop_usb_locked();
    }
    if (result != 0) {
        (void)restore_state_from_lifecycle();
        return unlock_context(result);
    }

    if (g_context.module_lifecycle == MODULE_REGISTERED) {
        result = sceKernelStopModule(
            g_context.module_id, 0, NULL, &module_status, NULL);
        if (result < 0 || module_status < 0) {
            result = result < 0 ? result : module_status;
            (void)restore_state_from_lifecycle();
            return unlock_context(result);
        }
        g_context.module_lifecycle = MODULE_LOADED;
    }

    result = sceKernelUnloadModule(g_context.module_id);
    if (result < 0) {
        (void)restore_state_from_lifecycle();
        return unlock_context(result);
    }
    g_context.module_id = -1;
    g_context.module_lifecycle = MODULE_NOT_LOADED;
    g_context.usb_lifecycle = USB_STOPPED;
    g_context.was_connected = 0;
    result = destroy_ipc_locked();
    if (transition_state(USB_MIDI_STATE_STOPPED) < 0 && result == 0) {
        result = PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    return unlock_context(result);
}

int UsbMidi_Write(const UsbMidiEvent *events, int count)
{
    UsbMidiIpcMidiBatch batch;
    int index;
    int result;

    if (events == NULL || count < 1
        || count > (int)USB_MIDI_IPC_MAX_BATCH_EVENTS) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    memset(&batch, 0, sizeof(batch));
    for (index = 0; index < count; ++index) {
        UsbMidiPacket packet;

        if (UsbMidiPacket_EncodeShort(
                events[index].cable,
                events[index].status,
                events[index].data1,
                events[index].data2,
                &packet) < 0) {
            return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
        }
        batch.events[index].timestamp_us = events[index].timestamp_us;
        batch.events[index].cable = (uint8_t)(packet.header >> 4U);
        batch.events[index].status = packet.midi0;
        batch.events[index].data1 = packet.midi1;
        batch.events[index].data2 = packet.midi2;
    }
    result = lock_context();
    if (result < 0) {
        return result;
    }
    if (g_context.module_lifecycle != MODULE_REGISTERED
        || g_context.usb_lifecycle != USB_ACTIVATED
        || g_context.ipc_ready == 0
        || g_context.next_tx_sequence == 0U) {
        return unlock_context(PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT));
    }

    batch.magic = USB_MIDI_IPC_MAGIC;
    batch.version = USB_MIDI_IPC_VERSION;
    batch.kind = USB_MIDI_IPC_MESSAGE_MIDI_TX_BATCH;
    batch.sequence = g_context.next_tx_sequence;
    batch.count = (uint32_t)count;
    result = sceKernelTrySendMsgPipe(
        g_context.user_to_kernel_pipe,
        &batch,
        sizeof(batch),
        0,
        NULL);
    if (result == 0) {
        ++g_context.next_tx_sequence;
        result = sceKernelSetEventFlag(
            g_context.event_flag,
            USB_MIDI_IPC_EVENT_TX_AVAILABLE);
    }
    return unlock_context(result);
}

int UsbMidi_Read(UsbMidiEvent *events, int max_events)
{
    int event_count = 0;
    int result;

    if (events == NULL || max_events < 1
        || max_events > (int)USB_MIDI_IPC_MAX_BATCH_EVENTS) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    result = lock_context();
    if (result < 0) {
        return result;
    }
    if (g_context.module_lifecycle != MODULE_REGISTERED
        || g_context.ipc_ready == 0
        || g_context.next_rx_sequence == 0U) {
        return unlock_context(PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT));
    }

    while (event_count < max_events) {
        UsbMidiIpcMidiEvent message;
        UsbMidiPacket packet;

        result = sceKernelTryReceiveMsgPipe(
            g_context.kernel_to_user_pipe,
            &message,
            sizeof(message),
            0,
            NULL);
        if (result == PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_MPP_EMPTY)) {
            break;
        }
        if (result < 0) {
            return event_count > 0
                ? unlock_context_with_count(event_count)
                : unlock_context(result);
        }
        if (message.magic != USB_MIDI_IPC_MAGIC
            || message.version != USB_MIDI_IPC_VERSION
            || message.kind != USB_MIDI_IPC_MESSAGE_MIDI_RX
            || message.sequence != g_context.next_rx_sequence
            || UsbMidiPacket_EncodeShort(
                message.cable,
                message.status,
                message.data1,
                message.data2,
                &packet) < 0) {
            return event_count > 0
                ? unlock_context_with_count(event_count)
                : unlock_context(
                    PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT));
        }

        events[event_count].timestamp_us = message.timestamp_us;
        events[event_count].cable = (uint8_t)(packet.header >> 4U);
        events[event_count].status = packet.midi0;
        events[event_count].data1 = packet.midi1;
        events[event_count].data2 = packet.midi2;
        ++event_count;
        ++g_context.next_rx_sequence;
    }
    return unlock_context_with_count(event_count);
}

int UsbMidi_GetStatus(UsbMidiStatus *status)
{
    int raw_usb_state = 0;
    int result;

    if (status == NULL) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
    }
    result = lock_context();
    if (result < 0) {
        return result;
    }

    if (g_context.usb_lifecycle != USB_STOPPED) {
        raw_usb_state = sceUsbGetState();
        if (raw_usb_state < 0) {
            return unlock_context_without_recording(raw_usb_state);
        }
    }
    if (g_context.usb_lifecycle == USB_ACTIVATED) {
        result = transition_state(active_state_from_usb(raw_usb_state));
        if (result < 0) {
            return unlock_context_without_recording(result);
        }
    }

    memset(status, 0, sizeof(*status));
    status->size = sizeof(*status);
    status->state = g_context.state;
    status->last_result = g_context.last_result;
    status->raw_usb_state = (uint32_t)raw_usb_state;
    status->active = (raw_usb_state & PSP_USB_ACTIVATED) != 0;
    status->cable_connected = (raw_usb_state & PSP_USB_CABLE_CONNECTED) != 0;
    status->link_established =
        (raw_usb_state & PSP_USB_CONNECTION_ESTABLISHED) != 0;
    return unlock_context_without_recording(0);
}

bool UsbMidi_IsConnected(void)
{
    UsbMidiStatus status;

    return UsbMidi_GetStatus(&status) == 0 && status.link_established != 0;
}
