# usb midi research notes

tested on a normal PSP-2004, ARK-4, firmware 6.61. macOS host and later an
OP-XY. no usb audio needed, just the AudioControl interface that USB MIDI 1.0
expects plus the MIDIStreaming interface.

the big finding was that the PSP usb bus does not want a normal flat descriptor
blob. it walks Sony-specific source records with pointers and lengths. On 6.61
the config source record is 24 bytes and the interface-group pointer is at
offset 12. interface records are 24 bytes. endpoint records have the 7 byte
standard prefix inline, then an extension pointer and length. zero/sentinel
records after the interface and endpoint arrays also matter.

leaving the config interface-group pointer null was the nasty bug. registration
worked, bus start worked, but activation/enumeration hung and the PSP eventually
hard powered off. once the pointer graph matched the 6.61 layout it activated
without hanging. useful states were `0x211` = active, no cable/link and `0x222`
= active with cable and link.

macOS sees it using the built in MIDI driver as `"PSP" Type B`, Sony VID
`054c`, PID `01c9`, 480 Mb/s. no custom mac driver. the complete config chain is
101 bytes, two interfaces, two bulk endpoints and embedded/external MIDI jacks.

safe startup order is: load PRX, start module/register driver, start Sony USB
bus, start our USB driver, then activate PID 01c9. stop is exactly backwards:
deactivate, stop our driver, stop bus, unregister/stop PRX, unload. trying to do
cleanup from the HOME callback was a bad idea. callback should only set a flag,
normal app thread does the shutdown. this fixed the endless grey `please wait`
screen.

only one kernel worker owns both endpoints. request structs and packet buffers
are static and 64-byte aligned. completion callbacks dont parse or draw or wait,
they only clear pending state and set event flags. detach/stop cancels requests,
then joins the worker before unload. this was much more reliable than doing USB
work from multiple places.

user/kernel bridge ended up being two fixed 256 byte message pipes and one event
flag. fixed hello/ack handshake at module start. writes are atomic batches of
1-8 short MIDI messages, three max-size batches fit in TX. RX holds ten events
and drops new ones rather than blocking when full. timestamps are metadata, not
scheduled send times.

RX sometimes completes with zero bytes and malformed host packets can happen.
zero-length rearms must be bounded (we used max four in a row). malformed MIDI
packets get ignored. never trust CIN/status/data bytes without validating all of
them.

we had a couple unrelated traps too. linking the user facade against
`pspkernel` pulled `ForKernel` imports into the EBOOT and caused `8002013C`.
normal app side has to use the user stubs. also debug screen double buffering
needs the real uncached EDRAM base plus offset, passing only the offset gave an
instant black screen.

the precision MIDI clock worked best in one user thread using absolute
deadlines, not the display loop. late clocks are skipped instead of sent as a
burst. measured 968 clocks in 20.145 sec, basically 120.005 BPM, and the OP-XY
stayed locked.

the old development driver sent an automatic C3 pair, CC118 debug markers and a
special CC119 echo. all of that is removed from this clean library. it should
only transmit what the app submits. sequencer/UI/clock are application code and
also not in the reusable driver.

current limit: complete non-SysEx MIDI messages only. SysEx needs real streaming
state across USB packets so dont fake it with this API. the cleaned 1.0 build is
warning clean and host tested; final clean-PRX hardware smoke test is still the
last release check.
