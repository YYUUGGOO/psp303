# Safety contract

This library is temporary runtime code. It must be launched from
`ms0:/PSP/GAME/` and must leave the PSP unchanged after application exit or
reboot.

It never writes to flash, NAND, IPL, recovery settings, boot configuration,
`VSH.TXT`, `GAME.TXT`, or `POPS.TXT`; installs no plugin; replaces no Sony
module; and writes no persistent configuration.

Applications must:

1. Ship `UsbMidiDriver.prx` beside their EBOOT.
2. Load it only with `UsbMidi_Init` and start USB only with `UsbMidi_Start`.
3. Check every library return value.
4. Release any application-owned active notes before teardown.
5. Call `UsbMidi_Shutdown` from the normal application thread on every exit and
   error path.
6. Leave the PRX loaded if shutdown reports an error; never force unload while
   an endpoint request may still be active.

The driver owns one worker thread, one static bulk-IN request, and one static
bulk-OUT request. Completion callbacks only clear request state and signal an
event flag. Stop cancels pending transfers, waits for callbacks and the worker,
unregisters the driver, then permits module unload.

If an application hangs, disconnect USB and try HOME. If HOME is unavailable,
power the PSP off completely and reboot normally. Nothing starts automatically.
