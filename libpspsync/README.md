# libpspsync

`libpspsync` is a small, allocation-free clock and audio-pulse sync library for
PSP homebrew. It is designed to run once per sample inside an existing PSPSDK
audio callback. The same clock event can advance the application's sequencer
and produce the external pulse, keeping them sample-aligned.

See [IMPLEMENTATION.md](IMPLEMENTATION.md) for timing math, profile behavior,
audio routing, concurrency assumptions, and hardware verification guidance.

## Add it to another PSP project

```cmake
add_subdirectory(path/to/libpspsync)
target_link_libraries(your_app PRIVATE pspsync)
```

```c
#include <pspsync.h>

PSPSyncClock clock;
PSPSyncConfig config = {
    .sample_rate = 44100,
    .bpm_milli = 120000,
    .ticks_per_quarter = 4,
    .pulse_level = 29490
};

pspSyncInit(&clock, &config);
pspSyncSetProfile(&clock, PSPSYNC_PROFILE_VOLCA);
pspSyncStart(&clock, 1);
```

In the audio callback:

```c
PSPSyncFrame sync = pspSyncNext(&clock);

if (sync.tick) {
    sequencer_advance();
}

output[i].left = sync.pulse;
output[i].right = application_audio;
```

Profiles:

- `PSPSYNC_PROFILE_OFF`: clock events remain active, pulse output is silent
- `PSPSYNC_PROFILE_POCKET_OPERATOR`: 15 ms pulse every second 16th-note tick
- `PSPSYNC_PROFILE_VOLCA`: 15 ms pulse on every 16th-note tick

Tempo uses integer milli-BPM and an internal 32.32 fixed-point accumulator to
avoid long-term drift from rounded samples-per-step calculations.
