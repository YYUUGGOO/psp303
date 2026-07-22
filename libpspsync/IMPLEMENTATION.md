# libpspsync implementation

This document describes how `libpspsync` generates sequencer timing and
Pocket Operator/Volca-compatible audio pulses on the PSP. The library is
PSP-specific in its intended use, but it does not own the PSP audio device. An
application calls it once for every output sample from its existing PSPSDK
audio callback.

## Design goals

- Keep the internal sequencer and external pulse output sample-aligned.
- Avoid allocation, locking, floating-point timing, and system calls in the
  audio callback.
- Preserve long-term tempo accuracy when a step does not contain an integer
  number of samples.
- Let applications change tempo or output profile without recreating the
  clock.
- Keep pulse sync independent from USB MIDI and application audio generation.

The public interface is in `include/pspsync.h`; the complete implementation is
in `src/pspsync.c`.

## Clock model

One `PSPSyncClock` owns all timing state. `pspSyncNext()` advances that state by
exactly one audio sample and returns:

```c
typedef struct {
    int16_t pulse;
    uint8_t tick;
} PSPSyncFrame;
```

`tick` is a one-sample event used to advance the application's sequencer.
`pulse` is the current signed 16-bit sync signal. Both values come from the
same accumulator, so a sequencer step and its external pulse begin on the same
sample.

The clock uses a 32.32 fixed-point phase accumulator. One complete phase is:

```text
PHASE_ONE = 2^32
```

The phase increment per audio sample is:

```text
increment = BPM_milli * ticks_per_quarter * 2^32
            ------------------------------------------------
                    60000 * sample_rate
```

For the default `ticks_per_quarter = 4`, each clock tick is one sixteenth note.
Using an accumulator preserves the fractional part between samples instead of
rounding a sixteenth note to a fixed integer sample count. This prevents the
cumulative drift caused by repeatedly using a rounded samples-per-step value.

The calculation uses 64-bit integers. Tempo is represented as milli-BPM, so
`120000` means 120.000 BPM.

## Per-sample operation

For every call to `pspSyncNext()`:

1. Return silence if the clock is stopped.
2. Emit an immediate tick if `pending_tick` was set by a reset start.
3. Otherwise add `increment_q32` to `phase_q32`.
4. When phase reaches `2^32`, subtract `2^32` and emit one tick.
5. On a tick selected by the active profile's divider, load
   `pulse_remaining` with the configured pulse length.
6. While `pulse_remaining` is nonzero, return `pulse_level` and decrement the
   counter.

At the supported PSP tempos and audio sample rates, one call cannot cross more
than one tick boundary. The implementation therefore emits at most one tick
per sample.

## Pulse profiles

Both hardware profiles currently use a 15 ms positive PCM pulse. At 44.1 kHz
this is calculated as 661 samples because the integer conversion truncates:

```text
pulse_samples = sample_rate * 15 / 1000
```

| Profile | Divider | Result |
| --- | ---: | --- |
| `PSPSYNC_PROFILE_OFF` | 0 | Clock ticks continue; pulse output stays silent. |
| `PSPSYNC_PROFILE_POCKET_OPERATOR` | 2 | Pulse on every second sixteenth-note tick. |
| `PSPSYNC_PROFILE_VOLCA` | 1 | Pulse on every sixteenth-note tick. |

The default pulse level is `29490`, approximately 90% of positive 16-bit full
scale. The actual voltage at connected hardware depends on PSP volume, the
headphone output stage, cabling, and the receiving device. Start with a low PSP
volume and raise it only until the receiving device detects stable pulses.

The PSP headphone output is an audio output, not a DC logic output. The
receiving device sees the pulse after the PSP's analog output path. Use an
appropriate stereo breakout cable and do not connect the headphone output to
ports that are not designed to accept audio or sync-level signals.

## Start, stop, and reset behavior

```c
pspSyncStart(&clock, 1);
```

starts the clock, clears phase and pulse state, resets the tick counter, and
sets `pending_tick`. The next call to `pspSyncNext()` therefore produces the
first sequencer tick and, when enabled, its pulse immediately.

```c
pspSyncStart(&clock, 0);
```

starts or resumes without resetting phase. It does not create an immediate
tick.

`pspSyncStop()` stops phase advancement and clears pending/pulse output. The
clock retains its phase, so a later non-reset start can resume it.

## Tempo and profile changes

`pspSyncSetTempo()` recalculates the phase increment but preserves current
phase. A live tempo change therefore does not create a duplicate tick or reset
the pattern.

`pspSyncSetProfile()` validates the profile, calculates its pulse length and
divider, and cancels any pulse currently in progress. Switching profiles does
not alter clock phase or the sequencer tick count.

An invalid profile falls back to `PSPSYNC_PROFILE_OFF`. Zero-valued initial
configuration fields receive these defaults:

| Field | Default |
| --- | ---: |
| Sample rate | 44100 Hz |
| Tempo | 120000 milli-BPM |
| Ticks per quarter | 4 |
| Pulse level | 29490 |

## PSP audio integration

The recommended output contract is:

- Left channel: hardware pulse when a sync profile is enabled.
- Right channel: application audio.
- Both channels: application audio when the profile is off.

Example callback core:

```c
for (unsigned int i = 0; i < frame_count; ++i) {
    PSPSyncFrame sync = pspSyncNext(&sync_clock);
    int16_t synth_sample;

    if (sync.tick) {
        sequencer_step = (sequencer_step + 1) % 16;
        trigger_step(sequencer_step);
    }

    synth_sample = render_synth_sample();
    output[i].right = synth_sample;
    output[i].left = sync_profile == PSPSYNC_PROFILE_OFF
                   ? synth_sample
                   : sync.pulse;
}
```

Initialization:

```c
PSPSyncClock sync_clock;
PSPSyncConfig config = {
    .sample_rate = 44100,
    .bpm_milli = 120000,
    .ticks_per_quarter = 4,
    .pulse_level = 29490
};

pspSyncInit(&sync_clock, &config);
pspSyncSetProfile(&sync_clock, PSPSYNC_PROFILE_VOLCA);
pspSyncStart(&sync_clock, 1);
```

## Independence from MIDI synchronization

`libpspsync` has no USB or MIDI dependency. Its clock is driven only by audio
samples. An application can therefore use any of these combinations:

- Internal audio only.
- Internal audio plus PO/Volca pulse sync.
- Internal audio plus USB MIDI clock.
- Internal audio plus pulse sync and USB MIDI clock simultaneously.
- USB MIDI clock with MIDI notes disabled while pulse sync remains active.

PSP-303 uses a separate USB MIDI scheduler for 24-PPQN MIDI Clock. Changing
`PSPSyncProfile` affects only the left audio channel and never enables or
disables USB MIDI notes or clock messages.

## Concurrency contract

The library contains no locks and performs no atomic operations. The safest
ownership model is to call all mutating functions from the audio thread. If UI
code changes simple application parameters, copy those values into the clock
at an audio-buffer boundary before calling `pspSyncNext()`.

PSP-303 currently updates tempo and profile from its audio callback based on
small shared scalar values. Other projects with more complex state should use
a message, double-buffered configuration, or a short critical section outside
the per-sample loop. Never allocate memory, perform file I/O, or wait on a lock
inside `pspSyncNext()`.

One `PSPSyncClock` should have one owning audio callback. Multiple independent
clocks are supported as separate structures, but they will remain aligned only
if they are started together and advanced by the same number of samples.

## Building and reusing

```cmake
add_subdirectory(path/to/libpspsync)
target_link_libraries(your_app PRIVATE pspsync)
```

The library is a small static archive and has no external dependency beyond a
C compiler with 64-bit integer support. Its API remains PSP-oriented because
it is designed around signed 16-bit PSP audio callback samples and PSP hardware
sync routing.

## Tests

`tests/test_pspsync.c` exercises all three profiles over one second, including
tick counts, PO/Volca pulse division, pulse duration, and stopped-clock
behavior. A host test can be compiled from the library sources without PSPSDK
because the core has no platform calls.

Hardware verification should additionally check:

1. Stable step detection by both a Volca and Pocket Operator.
2. Required PSP volume for each receiving device.
3. Long-run tempo agreement with the receiving sequencer.
4. Start alignment between the internal first step and external gear.
5. Left/right isolation through the intended breakout cable.

## Known limits and extension points

- Pulse width is fixed at 15 ms per hardware profile.
- Pulse polarity is positive only at the PCM layer.
- PO division and Volca division are fixed by the profile table.
- There is no swing; every tick uses the same phase increment.
- There is no external clock input or tempo detection.
- Tempo changes preserve phase but are not smoothed.

New hardware profiles can be added by extending `PSPSyncProfile`, configuring
their `pulse_divider` and `pulse_samples` in `pspSyncSetProfile()`, updating
`pspSyncProfileName()`, and adding divider/duration tests. If future devices
need alternating pulse widths, polarity, or swing, keep those rules inside the
clock structure so the tick and pulse continue to derive from one sample-time
source.
