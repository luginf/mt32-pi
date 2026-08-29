# SC-55 emulation - licensing notice

This subtree is **not** licensed under mt32-pi's GPLv3. It is a separately-licensed,
experimental component kept apart from the rest of the codebase deliberately - see below.

## What's here

`upstream/` is an unmodified vendored copy of [Nuked SC-55](https://github.com/nukeykt/Nuked-SC55)
by nukeykt, commit `dd2f525f15fe4580a8fbc59535170651ca559f59` (2025-10-13), a cycle-accurate
Roland SC-55 emulator running the real SC-55 firmware on emulated Hitachi H8/532 and Mitsubishi
M37450M2 MCUs. It is distributed under the **original MAME license** (see `upstream/LICENSE`):
redistribution is permitted, but **not for commercial use**, and modifications must ship their
complete source.

## Why this is kept separate from the rest of mt32-pi

mt32-pi is GPLv3. GPLv3 guarantees the right to commercial use; the MAME license's
non-commercial clause contradicts that. The two cannot be combined into one distributed binary.
Concretely, that means:

- **This code is never linked into the mt32-pi kernel image**, and there is no plan to do so
  until/unless that changes.
- It is built by an **opt-in, separate `make sc55` target** (see the top-level `Makefile`) that
  produces a native desktop executable for evaluation and benchmarking on the host machine, not
  an ARM kernel image. It is not part of `make`/`make all`, and not part of CI
  (`.github/workflows/ci.yml`).
- No compiled binary that includes this code is (or will be) published as a GitHub Release or
  CI artifact - full mt32-pi image or SC-55-only image alike. Publishing an image with this
  code combined into the rest of the project would not be legally sound. Building it privately,
  for personal evaluation, is unrestricted.

## Status

Paused 2026-08-29 (Alan had to reboot his machine) - not finished, pick back up here. Nothing in
`src/synth/sc55/` is committed to git yet.

`make sc55` produces a working native binary, correctly loads a real SC-55mk2 ROM set, and
produces real audio via `-nogui` (confirmed on real hardware/audio by Alan). Multi-channel
instrument routing (Bank Select + Program Change per part) confirmed correct via `-mididebug`
and a minimal hand-built test MIDI file (`roms/test_violin_flute.mid`, not committed) - an
earlier "everything plays as piano" report turned out to be a specific test file
(`sharks_ahead.mid`) sending a GS Reset SysEx *after* its own per-channel patch setup, which is
standard Roland GS behaviour (GS Reset reverts all parts to Piano) working exactly as
documented, not a port bug; relocating that one SysEx to the start of the file (confirmed fixed
by Alan) proved it. Not yet integrated as an mt32-pi synth mode (no `CSC55Synth`/`CSynthBase`
wiring exists) - deliberately not started, see the benchmark result below for why.

**Next session should start with**: the full-polyphony CPU benchmark (see below) - that's the
one open question that determines whether any bare-metal integration work is worth doing at
all.

## Local modifications to the vendored upstream code

`upstream/src/mcu.cpp` (plus `midi.h`/`midi_rtmidi.cpp`) has several deliberate changes from
pristine upstream (search for "mt32-pi fork addition" to find every changed spot):

- `-nogui`: skips `SDL_INIT_VIDEO` and the LCD window/renderer entirely (audio + MIDI only).
  Added because the stock code's `SDL_CreateRenderer` call crashed with a GLX/
  `X_GLXCreateContext` X error on one real test machine, and forcing `SDL_RENDER_DRIVER=software`
  did not help - the crash was in window/context setup itself, not renderer backend selection.
- `-bench:<seconds>` (implies `-nogui`): a flat-out, single-threaded throughput benchmark
  (`MCU_RunBench()`). Runs the exact same per-instruction body as the real `work_thread()`, with
  no audio-consumer pacing and no real SDL audio device (the sample ring buffer is allocated
  directly), until `<seconds>` worth of the chip's own 24,000,000-units/sec clock has been
  simulated, then reports wall-clock time and the resulting realtime speed multiple.
- `SDL_HINT_NO_SIGNAL_HANDLERS` set whenever headless: SDL installs its own SIGINT/SIGTERM
  handler on the first `SDL_Init()` call, which just posts an `SDL_QUIT` event rather than
  terminating the process. Normal (GUI) mode consumes that event via `LCD_Update()`'s event
  poll; the headless loop never polls SDL events at all, so without this, Ctrl+C silently did
  nothing (confirmed both ways: hung on Alan's machine before the fix, then verified fixed by
  sending a real SIGINT to a running `-bench` process and confirming it terminated).
- `-listmidi`: lists available MIDI input ports (index + name) and exits, so the right
  `-p:<index>` can be found without guesswork.
- `-mididebug`: logs every raw MIDI byte received, to check what the chip is actually being
  sent.
- `-fullpoly` (modifier for `-bench:`): saturates polyphony (~36 voices across 9 channels) via
  `MCU_PostUART()` before the timed loop starts, for a worst-case throughput number instead of
  idle/boot load.
- `upstream/src/lcd.cpp`: `SDL_CreateRenderer` now explicitly requests `SDL_RENDERER_SOFTWARE`
  via its flags instead of driver auto-selection (`-1, 0`). Root cause of the original GLX
  crash: the window is created without `SDL_WINDOW_OPENGL`, so SDL's "opengl" driver being
  probed during auto-selection hits an invalid FBConfig and crashes with a GLX BadValue X error
  instead of failing cleanly; forcing the software renderer by flag skips that probe entirely.
  `SDL_RENDER_DRIVER=software` as an env var had NOT fixed this (confirmed by Alan) - only the
  flag-level fix should. Compiles, but **still crashed identically on Alan's machine even with
  the software renderer forced** (same X error serial numbers, meaning the same point of
  failure, not renderer-driver-selection related after all). Traced no further: `nvidia-smi`
  was also broken on the same machine at the time, which points to a system-level GPU/driver
  problem, not our code - not investigated further, GUI mode was deprioritized since `-nogui`
  is fully validated and unaffected (it never touches GPU/X11/GLX at all).

## Benchmark results so far

`-bench:20` gives **~2.0-2.1x realtime**, confirmed on *two* independent machines: Claude's
sandboxed dev container (unknown/shared x86-64 CPU, max reported 3.2 GHz, 4 cores) and Alan's
own real desktop ("simulated 20.00 emulated seconds in 9.386 wall seconds, 2.13x realtime").
The fact that two unrelated machines land on essentially the same number makes this a real,
reproducible figure, not an environment artifact - worth taking seriously.

**Full-polyphony number now measured too**: `-bench:20 -fullpoly` (saturates ~36 simultaneous
voices across 9 channels via `MCU_PostUART()`, no Note Off, before the timed loop starts) gives
**~1.77x realtime**, vs ~2.0-2.1x at idle - about 20% heavier, on the same x86 hardware. Both
numbers are now real and reproducible; full polyphony is confirmed the worse case, as expected,
but not dramatically worse than idle.

If a modern x86 desktop core only clears full-polyphony SC-55 emulation by ~1.77x, and a
Raspberry Pi 4's Cortex-A72 core is commonly several times slower per-core than a modern x86
desktop core for branch-heavy interpreter workloads like this, that's a real feasibility concern
for real-time bare-metal use (rough, unverified projection: well under 1x realtime on a Pi 4) -
worth taking seriously rather than assuming it'll work out. Still not conclusive - only an x86
projection, no Pi measurement yet (see below).

## Getting a real Pi number - cross-compiling from x86 abandoned, use a different approach

Tried cross-compiling `-bench` for aarch64 Linux (assuming Raspberry Pi OS on a spare SD card,
confirmed available) using `g++-aarch64-linux-gnu` + apt's `arm64` multiarch packages for
SDL2/RtMidi. Hit real friction: arm64 packages aren't on Alan's regional Ubuntu mirror at all
(they live on `ports.ubuntu.com`, a separate repository from the regular amd64 archive) -
fixable, but fiddly enough (extra apt sources, architecture juggling on Alan's own dev machine)
that Alan called it not worth it and asked to revert (`dpkg --remove-architecture arm64`, etc).
**Decision: abandoned cross-compiling from x86.**

**Better approach for next time**: skip cross-compilation entirely - copy this source tree (or
`git clone` once committed) onto the Pi itself, booted into Raspberry Pi OS, and build there
with its own native toolchain and `apt install libsdl2-dev librtmidi-dev` (arm64 IS the native
architecture on-device, so none of the foreign-architecture/ports.ubuntu.com friction applies).
Exactly the same `make sc55` recipe used on Alan's x86 desktop should work unmodified on the Pi.
This is the next concrete task for whenever this is picked back up.
