#include <pspkernel.h>

#include "usb_core.h"
#include "usb_midi_ipc.h"

PSP_MODULE_INFO("UsbMidiDriver", PSP_MODULE_KERNEL, 1, 0);

/**
 * Register the USB MIDI driver; the linked user library owns USB activation.
 */
int module_start(SceSize args, void *argp)
{
    int result = UsbMidiIpc_Configure(args, argp);

    if (result < 0) {
        return result;
    }
    result = UsbMidiUsbCore_Register();
    if (result < 0) {
        UsbMidiIpc_Reset();
        return result;
    }
    result = UsbMidiIpc_Acknowledge();
    if (result < 0) {
        int cleanup_result = UsbMidiUsbCore_Unregister();

        UsbMidiIpc_Reset();
        return cleanup_result < 0 ? cleanup_result : result;
    }
    return 0;
}

/**
 * USB is stopped by the linked user library before this call;
 * unregistration must succeed before module unload.
 */
int module_stop(SceSize args, void *argp)
{
    int result;

    (void)args;
    (void)argp;
    result = UsbMidiUsbCore_Unregister();
    if (result == 0) {
        UsbMidiIpc_Reset();
    }
    return result;
}
