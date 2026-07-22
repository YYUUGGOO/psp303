#ifndef USB_MIDI_USB_CORE_H
#define USB_MIDI_USB_CORE_H

/**
 * Register the UsbMidiDriver record and validated USB MIDI descriptor model.
 *
 * Registration itself does not start or activate the driver. The user
 * application owns the separately armed lifecycle. Registration submits no
 * endpoint requests; the driver's start/attach callbacks own those resources.
 *
 * @return 0 on success, otherwise a negative PSP kernel error code.
 */
int UsbMidiUsbCore_Register(void);

/**
 * Unregister the USB MIDI driver record from the PSP USB bus.
 *
 * @return 0 on success, otherwise a negative PSP kernel error code. A failed
 * unregister leaves the internal state registered so module unload is denied.
 */
int UsbMidiUsbCore_Unregister(void);

#endif
