#include "usb_core.h"
#include "usb_midi_psp_transport.h"
#include "usb_midi_tx.h"

#include <pspkerror.h>
#include <pspusbbus.h>

#include <stddef.h>
#include <stdint.h>

#define USB_MIDI_DRIVER_NAME "UsbMidiDriver"
#define PSP_ERROR_AS_INT(error_code) ((int)(unsigned int)(error_code))

/* Endpoint zero, bulk IN endpoint one, and bulk OUT endpoint two. */
static struct UsbEndpoint g_endpoints[3] = {
    {0, 0, 0},
    {1, 0, 0},
    {2, 0, 0}
};
static struct UsbInterface g_interface = {-1, 0, 2};

/* High-speed and full-speed records remain live until the PRX is unloaded. */
static UsbMidiPspDescriptorSet g_usbdata[2] __attribute__((aligned(64)));
static unsigned char g_product_string[] = {8, 3, '<', 0, '>', 0, 0, 0};
static int g_registered = 0;

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(
    offsetof(UsbMidiPspDescriptorSet, link) == 20U,
    "configuration link offset must be 20");
_Static_assert(
    offsetof(UsbMidiPspDescriptorSet, configuration) == 36U,
    "configuration source offset must be 36");
_Static_assert(
    offsetof(UsbMidiPspDescriptorSet, interface_groups) == 60U,
    "interface groups must begin at offset 60");
_Static_assert(
    offsetof(UsbMidiPspDescriptorSet, interfaces) == 84U,
    "interface records must begin at offset 84");
#endif

static int inert_control_request(int arg1, int arg2, struct DeviceRequest *request)
{
    (void)arg1;
    (void)arg2;
    (void)request;
    return 0;
}

static int inert_callback(int arg1, int arg2, int arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return 0;
}

static int midi_attach(int speed, void *arg2, void *arg3)
{
    (void)arg2;
    (void)arg3;
    return UsbMidiTx_OnAttach(speed);
}

static int midi_detach(int arg1, int arg2, int arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return UsbMidiTx_OnDetach();
}

static int midi_start(int size, void *args);
static int midi_stop(int size, void *args);

static struct UsbDriver g_driver = {
    .name = USB_MIDI_DRIVER_NAME,
    .endpoints = 3,
    .endp = g_endpoints,
    .intp = &g_interface,
    .devp_hi = NULL,
    .confp_hi = NULL,
    .devp = NULL,
    .confp = NULL,
    .str = (struct StringDescriptor *)g_product_string,
    .recvctl = inert_control_request,
    .func28 = inert_callback,
    .attach = midi_attach,
    .detach = midi_detach,
    .unk34 = 0,
    .start_func = midi_start,
    .stop_func = midi_stop,
    .link = NULL
};

static int midi_start(int size, void *args)
{
    int result;

    (void)size;
    (void)args;
    if (UsbMidiPspTransport_Init(
            &g_usbdata[0],
            &g_usb_midi_high_speed_configuration,
            USB_MIDI_HIGH_SPEED_PACKET_SIZE) < 0
        || UsbMidiPspTransport_Init(
            &g_usbdata[1],
            &g_usb_midi_full_speed_configuration,
            USB_MIDI_FULL_SPEED_PACKET_SIZE) < 0) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ERROR);
    }

    result = UsbMidiTx_Start(&g_endpoints[1], &g_endpoints[2]);
    if (result < 0) {
        return result;
    }
    g_driver.devp_hi = &g_usbdata[0].device.descriptor;
    g_driver.confp_hi = &g_usbdata[0].link;
    g_driver.devp = &g_usbdata[1].device.descriptor;
    g_driver.confp = &g_usbdata[1].link;
    return 0;
}

static int midi_stop(int size, void *args)
{
    int result;

    (void)size;
    (void)args;
    result = UsbMidiTx_Stop();
    if (result < 0) {
        return result;
    }
    g_driver.devp_hi = NULL;
    g_driver.confp_hi = NULL;
    g_driver.devp = NULL;
    g_driver.confp = NULL;
    return 0;
}

int UsbMidiUsbCore_Register(void)
{
    int result;

    if (g_registered != 0) {
        return PSP_ERROR_AS_INT(SCE_KERNEL_ERROR_ALREADY_STARTED);
    }
    result = sceUsbbdRegister(&g_driver);
    if (result < 0) {
        return result;
    }
    g_registered = 1;
    return 0;
}

int UsbMidiUsbCore_Unregister(void)
{
    int result;

    if (g_registered == 0) {
        return 0;
    }
    result = sceUsbbdUnregister(&g_driver);
    if (result < 0) {
        return result;
    }
    g_registered = 0;
    return 0;
}
