# PSP USB MIDI

A reusable, class-compliant USB MIDI 1.0 device library for PSP homebrew. It is
split into a user-mode static library and a manually loaded kernel PRX. The PRX
contains only USB enumeration, bounded MIDI transport, and IPC; it has no UI,
sequencer, synthesizer, clock generator, echo, or diagnostic MIDI traffic.

The implementation targets a regular PSP-2000 series console running ARK-4 on
firmware 6.61. The underlying enumeration, bidirectional transport, reconnect,
and shutdown design has been exercised on that hardware with macOS and an OP-XY.
The cleaned 1.0 PRX removes the earlier probe messages and therefore requires a
final hardware smoke test before being treated as release-qualified.

## Features

- Native USB Audio Class 1.0 / MIDIStreaming 1.0 descriptors
- Bulk MIDI IN and OUT endpoints
- Note, pressure, CC, program, pitch bend, system common, clock, transport,
  active sensing, and reset messages
- Atomic nonblocking writes of 1-8 complete messages
- Nonblocking reads of up to 8 messages in wire order
- Fixed-capacity queues and static kernel transfer buffers
- Clean stop, disconnect cancellation, PRX unload, and reconnect
- No USB audio, serial emulation, mass storage, SysEx streaming, allocation in
  callbacks, flash writes, plugin installation, or persistent state

SysEx is intentionally not part of the 1.0 API because it requires a separate
stateful stream interface; short messages are never silently treated as SysEx.

## Build

```sh
export PSPDEV="$HOME/pspdev"
export PATH="$PSPDEV/bin:$PATH"
psp-cmake -S . -B build
cmake --build build --parallel
```

The folder already includes matching prebuilt artifacts under `dist/`. A fresh
build recreates the same layout under `build/dist/`:

```text
dist/PspUsbMidi/
  PspUsbMidiConfig.cmake
  include/psp_usb_midi.h
  lib/libPspUsbMidi.a
  kernel/UsbMidiDriver.prx

dist/UsbMidiExample/
  EBOOT.PBP
  UsbMidiDriver.prx
```

Copy the complete `UsbMidiExample` directory to
`ms0:/PSP/GAME/UsbMidiExample/` for the minimal smoke test.

## Use from another CMake project

Either add this source tree with `add_subdirectory`, or include the generated
bundle config:

```cmake
include("/path/to/PspUsbMidi/PspUsbMidiConfig.cmake")
target_link_libraries(MyPspApp PRIVATE PspUsbMidi::PspUsbMidi)
```

Copy `${PSP_USB_MIDI_DRIVER}` beside your EBOOT. The application passes the
complete Memory Stick path to `UsbMidi_Init`:

```c
#include <psp_usb_midi.h>

int result = UsbMidi_Init(
    "ms0:/PSP/GAME/MyPspApp/UsbMidiDriver.prx");
if (result == 0) {
    result = UsbMidi_Start();
}

/* Poll UsbMidi_GetStatus until link_established is nonzero. */
if (result == 0 && UsbMidi_IsConnected()) {
    const UsbMidiEvent note_pair[2] = {
        {0U, 0U, 0x90U, 60U, 100U},
        {0U, 0U, 0x80U, 60U, 0U}
    };
    result = UsbMidi_Write(note_pair, 2);
}

/* Poll input from the normal application loop. */
UsbMidiEvent input[PSP_USB_MIDI_MAX_BATCH_EVENTS];
int count = UsbMidi_Read(input, PSP_USB_MIDI_MAX_BATCH_EVENTS);

/* Every exit and error path must perform guarded shutdown. */
result = UsbMidi_Shutdown();
```

`UsbMidi_Start` performs bus start, driver start, and activation as one guarded
operation with reverse-order rollback. `UsbMidi_Stop` deactivates USB but keeps
the PRX registered for a later `UsbMidi_Start`. `UsbMidi_Shutdown` stops USB,
unregisters the driver, stops the module, and unloads it.

Calls are serialized by the user library. `UsbMidi_Write` is nonblocking and
the fixed TX queue holds three maximum-size batches. `UsbMidi_Read` is
nonblocking and the RX queue holds ten events. Always check return values. A
two-second conservative settle window after host attachment is retained from
the hardware-proven transport before queued output and continuous input begin.

## Runtime safety

The PRX is loaded only from the application directory and unloads on shutdown.
It never writes to PSP storage. The HOME callback must only signal the normal
application thread; call `UsbMidi_Shutdown` from that normal thread, never from
callback or interrupt context. See [SAFETY.md](SAFETY.md).
