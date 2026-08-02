# BioMimicry — working notes for Claude Code

A program card for the **Music Thing Modular Workshop System Computer** (RP2040),
built on the header-only **ComputerCard** library. Sibling project to
`../WorkshopZX` — reuse its conventions and structure where they fit.

## Build

Toolchain comes from the Pico VS Code extension install at `~/.pico-sdk/`.
`CMakeLists.txt` includes `~/.pico-sdk/cmake/pico-vscode.cmake`, which pins
SDK 2.2.0 / GCC 14_2_Rel1 / picotool 2.2.0-a4.

From PowerShell:

```powershell
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.2.0"
$env:PATH = "$env:USERPROFILE\.pico-sdk\cmake\v3.31.5\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:USERPROFILE\.pico-sdk\toolchain\14_2_Rel1\bin;$env:PATH"
cmake -B build -G Ninja
cmake --build build
```

Output: `build/biomimicry.uf2`. Copy to `FLASHME/` for flashing (git-ignored).
`cmake`/`ninja` are **not** on the default PATH — always set it as above.

## Hard rules

- `ProcessSample()` runs at **48 kHz** on core 0. Allocation-free, no `malloc`,
  no blocking, no `float` in the hot path — fixed-point only.
- Audio/CV I/O is signed 12-bit (`-2048..2047`). `KnobVal()` is unsigned 12-bit
  (`0..4095`).
- **Never** do hardware setup in the `ComputerCard` constructor — it wedges the
  chip. Setup goes in `main()` or on core 1 before the run loop.
- `PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64` is required for the Workshop
  Computer's crystal. Don't remove it.
- Build clean: `-Wall -Wextra -Wdouble-promotion -Wfloat-conversion` are on.
  Watch the `--print-memory-usage` output at link time.

## If the engine outgrows one core

Use the `second_core` pattern from `../WorkshopZX` (see its `spectrum.h` /
`main.cpp`): core 1 free-runs the model at its own rate, core 0 handles 48 kHz
I/O, and they communicate through a small lock-free struct with a **single
writer per field** — no locks, no queues.

## Architecture

Physics engines tick at **1.5kHz** (`kCtrlDiv = 32` samples), voices/gates/CV at 48kHz.
Keep that split: it is what makes four agents of Kuramoto coupling affordable.

All fixed-point. Q16 (65536 = 1.0) for levels and probabilities, `uint32_t` phase
accumulators that wrap for free, `fast_sin()` LUT, `xorshift32()` for randomness. Never
reach for libm or float here.

## Layout

| Path | Purpose |
|------|---------|
| `main.cpp` | Card entry point, `ProcessSample()`, UI, output routing, boot dispatch |
| `biomimicry.h` | Shared types, `Engine` interface, control-rate constants |
| `engines.cpp/.h` | The six physics models |
| `voices.cpp/.h` | Synth + PCM voice rendering and panning |
| `fastmath.h/.cpp` | Sine LUT, PRNG, fixed-point helpers |
| `samples_default.h` | `__has_include` shim — builds with or without baked PCM |
| `ComputerCard.h` | Vendored MTM library — do not edit |
| `info.yaml` | Workshop System card registry metadata |
| `samples/` | 8-bit signed mono 48kHz `.raw` one-shots — git-ignored |
| `panels/` | Printable panel overlay PNGs |
| `tools/` | `gensamples.py` (placeholder PCM), `mksamples.py` (bake to header), `simulate.py` (engine rate check), `bin2h.py` |
| `reference/` | Notes, papers, format docs backing the implementation |
| `docs/` | Longer-form design notes / devlog |
| `FLASHME/` | Local `.uf2` builds for flashing — git-ignored |

## Verifying engine changes

There is **no host C++ compiler on this machine**, so `tools/simulate.py` is a Python
model of the engine math used to check trigger rates before flashing:

```sh
python tools/simulate.py     # triggers/sec per agent across the knob range
```

It duplicates the C++ constants — **if you change `engines.cpp`, update it too** (or
delete it rather than let it drift). `tools/simulate.cpp` compiles the real sources
natively if a host compiler ever becomes available.

Sanity targets: no mode silent across its whole sweep, nothing above ~25 triggers/sec
per agent (that stops reading as rhythm), and every knob should change *something*
across its full travel.

## Repo

`https://github.com/uglifruit/WorkshopBio` (public). Commit as
Andy Jenkinson (uglifruit).
