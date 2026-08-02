# BioMimicry — for the Music Thing Workshop Computer

**A program card for the Music Thing Modular Workshop System Computer.**

> **Status: scaffold.** The card builds, flashes and runs — the BioMimicry engine
> itself is being written. This README grows with it.

Runs on a real Workshop Computer. Built on the RP2040 with
[ComputerCard](https://github.com/TomWhitwell/Workshop_Computer) (Chris Johnson).

---

## What it does

*(To be written as the engine lands.)*

The scaffold firmware currently proves the hardware path end to end: Knob Main
drives a slow triangle LFO on CV Out 1, Knob X sets a static CV on CV Out 2,
audio and pulses pass through, and the LEDs sweep so an unpatched card visibly
shows it is running.

## Panel

| Control | Function |
|---------|----------|
| **Knob Main** | *(scaffold: LFO rate on CV Out 1)* |
| **Knob X** | *(scaffold: static CV on CV Out 2)* |
| **Knob Y** | — |
| **Switch Up** | — |
| **Switch Middle** | — |
| **Switch Down** | — |

## Inputs

| Jack | Function |
|------|----------|
| **CV In 1** | — |
| **CV In 2** | — |
| **Audio In 1** | *(scaffold: passes to Audio Out 1)* |
| **Audio In 2** | *(scaffold: passes to Audio Out 2)* |
| **Pulse In 1** | *(scaffold: passes to Pulse Out 1)* |
| **Pulse In 2** | *(scaffold: passes to Pulse Out 2)* |

## Outputs

| Jack | Function |
|------|----------|
| **Pulse Out 1** | — |
| **Pulse Out 2** | — |
| **CV Out 1** | *(scaffold: triangle LFO)* |
| **CV Out 2** | *(scaffold: Knob X)* |
| **Audio Out 1** | — |
| **Audio Out 2** | — |
| **LEDs** | *(scaffold: running indicator)* |

Printable panel overlays will live in [`panels/`](panels/).

---

## Building

Raspberry Pi Pico SDK (2.2.0), Arm GCC 14.2, Ninja:

```sh
cmake -B build -G Ninja
cmake --build build
```

Produces `build/biomimicry.uf2`. Hold **BOOTSEL** on the Computer's RP2040 while
plugging in USB, then drop the `.uf2` on the mounted drive.

On Windows the toolchain the Pico VS Code extension installs works directly —
`CMakeLists.txt` picks it up via `~/.pico-sdk/cmake/pico-vscode.cmake`. To build
from a shell, put its `cmake`, `ninja` and `toolchain` `bin` directories on `PATH`
and set `PICO_SDK_PATH` to `~/.pico-sdk/sdk/2.2.0`.

## Conventions

`ProcessSample()` is called at **48 kHz**. It must be allocation-free and
fixed-point — no `malloc`, no `float` in the hot path, no blocking. Audio and CV
I/O is signed 12-bit (`-2048..2047`); knobs read unsigned 12-bit (`0..4095`).

If the engine needs more compute than 48 kHz allows, the RP2040's second core is
available (the `second_core` pattern used in
[WorkshopZX](https://github.com/uglifruit/WorkshopZX)): core 1 free-runs the
model, core 0 does 48 kHz I/O, and they meet through a small lock-free struct with
a single writer per field. Hardware setup belongs in `main()` or on core 1 —
never in the `ComputerCard` constructor.

---

## Credits

- **Music Thing Modular Workshop System Computer** — Tom Whitwell / Music Thing
  Modular. **ComputerCard** library by **Chris Johnson** (MIT, header-only).
- **Raspberry Pi Pico SDK** / RP2040 — Raspberry Pi Ltd.
- BioMimicry for the Workshop Computer — **Andy Jenkinson** (**uglifruit**), 2026,
  with **Claude Code** (Anthropic).

## Licence

The card's own source is Andy's. Vendored components keep their own licences
(`ComputerCard.h`).
