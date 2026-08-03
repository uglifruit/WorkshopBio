# BioMimicry — Organic Rhythms for the Workshop Computer

**A generative stochastic rhythm card born under the dark skies of rural Wales.**

Modular synthesis often traps us in rigid, Euclidean grids. BioMimicry breaks that grid
by using **six distinct mathematical physics engines** to generate triggers, CV and audio
that feel unmistakably alive.

It was built to capture the semi-random behaviours of the natural world: the polyrhythmic
clopping of horses on a road, the cascading panic of a flock of geese, the rushing
accumulator of a waterfall walk, and the sudden silent clustering of a meteor shower
overhead. *(We added the frogs later — there weren't any on the farm, but the swamp
mathematics were too good to leave out.)*

Runs on a real Music Thing Modular Workshop System Computer. Built on the RP2040 with
[ComputerCard](https://github.com/TomWhitwell/Workshop_Computer).

> **Status:** firmware complete and building; awaiting hardware validation.

---

## The Ecosystems

Tap the momentary switch (Down) to cycle habitats. Each has its own internal logic.

| Mode | Model | Behaviour |
|---|---|---|
| **Horses** | Real equine gaits | A **herd**: each horse has its own stride clock driving four hooves at their true footfall offsets, and the animals slide in and out of step the way real horses travelling together never quite match pace. Walk, trot, canter and gallop are the actual biomechanical patterns — including the suspension phase where all four feet leave the ground. |
| **Geese** | Stochastic contagion | A cascading probability network across a **flock of twelve**. One bird honking raises the odds for all the others, creating tight reactive clusters that erupt and fade. |
| **Frogs** | Coupled oscillators | The Kuramoto model. Voices pull on each other's timing, fighting between perfect metronomic synchronisation and chaotic swarms — and they'll entrain to an external clock if you give them one. |
| **Rain** | Leaky integrate-and-fire | Buckets fill with noise and constantly leak. An overflow **splashes downstream** into the next bucket, so drips pull each other along into rushing clusters, then fall apart. |
| **Meteors** | Inhomogeneous Poisson | An invisible slow-moving weather system dictates the density of a **swarm of twelve**. Long eerie silences swell smoothly into heavy overlapping barrages. |
| **Cicadas** | Amplitude feedback | A field of twelve insects that call faster the louder the field already is — and tire from being in it. That loop produces the slow pulsing waves of loudness a real cicada field makes. Where Frogs couple on *phase* and Geese on *events*, Cicadas couple on *loudness*. |

### The gaits are real

Horses mode isn't four clocks approximating a rhythm — it's one stride clock with
each hoof landing at its correct point in the stride:

| Gait | Footfall (fraction of stride) | Suspension |
|---|---|---|
| **Walk** | LH 0.00 → LF 0.25 → RH 0.50 → RF 0.75 | — (4-beat lateral, same-side legs consecutive) |
| **Trot** | [LH+RF] 0.00 → [LF+RH] 0.50 | 2-beat diagonal |
| **Canter** | LH 0.00 → [LF+RH] 0.22 → RF 0.44 | 56% float |
| **Gallop** | LH 0.00 → RH 0.10 → LF 0.21 → RF 0.31 | 69% float (rotary — both hinds, then both fores) |

Because each horse's four hooves share one stride clock, its gait *holds together*
however long it runs; Knob Y jitters each hoof's timing without breaking the pattern.
Knob X adds **whole horses**, never partial ones — a three-legged horse is not a
smaller herd. Each animal runs slightly off its neighbours' pace, so the herd phases
continuously. `python tools/simulate.py gaits` verifies the footfalls biomechanically.

## Panel

| Control | Function |
|---------|----------|
| **Knob Main** | **The Physics** — the fundamental law of the current ecosystem (Gait & tempo · Contagion · Decoupling · Downpour · Debris density) |
| **Knob X** | **Population** — how many agents are alive: **horses in the herd**, birds in the flock, frogs in the pond, buckets on the leaf, meteors in the sky, insects in the field |
| **Knob Y** | **Chaos / Humanize** — per-mode randomness and spread (timing jitter, spark rate, frequency spread, threshold variance, LFO wander) |
| **Switch Down** (momentary) | **Tap to cycle** the ecosystem. **Hold at power-on** to boot the PCM sample voices. |
| **Switch Up** | Routing: **Discrete** |
| **Switch Middle** | Routing: **Summed / CV** |

### Per-mode meaning of Knob Main

| Mode | Knob Main |
|---|---|
| **Horses** | **Gait and stride rate.** `0.00–0.25` Walk · `0.25–0.50` Trot · `0.50–0.75` Canter · `0.75–1.00` Gallop. Each gait sweeps its own stride-rate band, so a gallop is genuinely faster than a walk rather than the same pattern sped up. |
| **Geese** | **Contagion** — 0.0 birds ignore each other; 1.0 one honk sets off a panicked chain reaction |
| **Frogs** | **Decoupling** — 0.0 is maximum coupling (locked metronomic sync); 1.0 is zero coupling (total chaos) |
| **Rain** | **Downpour** — 0.0 leak exceeds input (silence); 1.0 rapid stuttering torrents |
| **Meteors** | **Debris density** — 0.0 rare isolated hits; 0.5 long silences swelling into dense waves; 1.0 constant barrage |
| **Cicadas** | **Coupling depth** — 0.0 independent insects, a steady even drone; 1.0 the field drives itself into surges that collapse into silence |

### Per-mode meaning of Knob Y (Chaos)

| Mode | Knob Y |
|---|---|
| **Horses** | Per-hoof timing jitter — an uneven, real animal rather than a machine |
| **Geese** | Spontaneous spark rate — how readily a bird honks unprompted |
| **Frogs** | Natural-frequency spread — how hard sync is to reach |
| **Rain** | **Leak rate** — slow leak lets buckets accumulate into heavy irregular drips; fast leak keeps only the strongest bursts |
| **Meteors** | Density wander on top of the hidden weather system |
| **Cicadas** | Rate spread across the field, so it never sounds like one insect multiplied |

## Inputs

| Jack | Function |
|------|----------|
| **Pulse In 1** | **The Spook** — a hardware interrupt that disrupts the environment. Horses: the herd startles into step, every horse landing together before drifting apart again. Geese: spook the flock into a guaranteed cascade. Frogs: splash — scramble every phase, destroying sync. Rain: wind gust — dump energy into every bucket. Meteors: bolide — spike density to maximum. **Cicadas: a footstep in the grass — the whole field falls silent at once**, then creeps back in. |
| **Pulse In 2** | **The Clock** — an external tempo the ecosystem *entrains to* rather than obeys. Frogs treat it as a phantom frog in the pond and couple to it with whatever strength Knob Main is set to, so you can dial anywhere from locked-to-the-clock to completely indifferent. Horses lock their stride to it. Geese lean their honks toward the beat; Rain tops up every bucket so the nearest tips on the beat; Meteors swell the debris field. Stop the clock and the ecosystem drifts back to its own timing within ~3 seconds. |
| **CV In 1** | Modulates **Knob Main** (the physics variable) |
| **CV In 2** | Modulates **Knob X** (population) |

## Outputs

Routing is chosen with the toggle. The Computer has two pulse outs, so in Discrete mode
agents 3 and 4 fire as **calibrated 5 V blips on the CV outs** — every agent gets a
physical trigger.

**Switch Up — Discrete**

| Jack | Function |
|------|----------|
| **Pulse Out 1 / 2** | Agent 1 / Agent 2 triggers (5 ms gates) |
| **CV Out 1 / 2** | Agent 3 / Agent 4 triggers, as 5 V blips |

**Switch Middle — Summed / CV**

| Jack | Function |
|------|----------|
| **Pulse Out 1** | All active agents logically OR'd |
| **Pulse Out 2** | **Accent** — fires when two or more agents hit at once |
| **CV Out 1** | Continuous internal state of agent 1 (phase ramp, bucket level, excitation…) as 0–5 V |
| **CV Out 2** | Global ecosystem state — debris density, flock agitation, chorus coherence |

**Audio Out 1 / 2** — all agents rendered and placed in the stereo field (see below).

**LEDs** — one LED per ecosystem. The active mode's LED sits at a dim "you are here"
glow and flares to full on every trigger, so one light carries both meanings.

## Voices

**Standard boot — synthesized.** Each mode has its own DSP timbre, built around whatever
detail actually identifies the sound: hooves get a pitch-dropping body *plus a sharp
band-passed noise transient* for shoe-on-stone; honks are a saw whose filter opens at the
attack for that nasal kink; ribbits are Karplus-Strong; **drips rise in pitch as they
decay**, which is the acoustic signature of a bubble collapsing in liquid and the reason a
drip sounds like a drip; meteors are noise swept by a closing filter; cicadas are a high
tone ring-modulated by a wing-beat buzz.

### Round robins

Every trigger picks one of four variants, and the variant *means* something:

| Mode | What a variant is |
|---|---|
| **Horses** | **One per hoof** — the engine reports which hoof landed and the voice plays that hoof, hinds lower and heavier than fores. Every horse in the herd plays all four of its own. This is what stops a gait sounding like a drum machine. |
| **Geese** | Four birds of different size |
| **Frogs** | Four species in the chorus |
| **Rain** | Four drip sizes |
| **Meteors** | Four distances |
| **Cicadas** | Four insects, tightly spread — a real field is fairly uniform |

Everything except Horses picks at random with a **no-immediate-repeat** rule, so you never
hear the same honk or drip twice running.

### Stereo placement

Panning comes from the ecosystem, not from a knob:

- **Fixed** (Horses) — each horse holds its own place in the field, its four hooves
  sitting just either side of that spot (near side / off side). The animals stay put;
  you hear a herd spread in front of you, not four wandering sounds.
- **Spread** (Geese, Frogs, Cicadas) — every swarm member has its own place, so twelve
  birds occupy twelve positions and a cascade sweeps across the field.
- **Random** (Rain, Meteors) — each hit lands somewhere new, because each is a new object.

**Alt boot (hold the momentary switch Down at power-on) — PCM.** Plays baked-in samples
from flash instead. The repo ships procedural placeholders; drop real recordings into
`samples/` and rebuild:

```sh
ffmpeg -i clop.wav -ac 1 -ar 48000 -f s8 samples/horses.raw
```

Expected files: `horses.raw` `geese.raw` `frogs.raw` `rain.raw` `meteors.raw`
`cicadas.raw` (8-bit signed mono, 48 kHz). If `samples/` is absent the firmware still
builds and alt-boot falls back to the synthesized voices.

There is room for far more: code is ~100 KB of the 2 MB flash, leaving **~40 seconds** of
48 kHz mono PCM — enough for eight distinct round-robin recordings per mode at a second
each. The variant system is already wired for it.

---

## Under the hood

`ProcessSample()` runs at 48 kHz, but the physics don't need audio rate: engines tick at
**1.5 kHz** (every 32nd sample), which is 32× cheaper and still gives 0.67 ms timing
resolution — far finer than the ear resolves for triggers. Voice rendering, gate timing
and CV output stay at the full 48 kHz.

Everything is **integer fixed-point** — Q16 for levels and probabilities, `uint32_t`
phase accumulators that wrap for free, a 257-entry quarter-wave sine LUT, and xorshift32
for randomness. There is no `float` in the hot path and no libm; the RP2040 has no FPU.

| File | Purpose |
|---|---|
| [fastmath.h](fastmath.h) / [fastmath.cpp](fastmath.cpp) | Sine LUT, PRNG, fixed-point helpers |
| [biomimicry.h](biomimicry.h) | Shared types, `Engine` interface, control-rate constants |
| [engines.cpp](engines.cpp) | The six physics models |
| [voices.cpp](voices.cpp) | Synth + PCM voice rendering, panning |
| [main.cpp](main.cpp) | I/O, mode/routing UI, LEDs, boot dispatch |

### Verifying the physics

`tools/simulate.py` models the engine math in Python and reports gait correctness, the
Kuramoto sync curve, and triggers/second per agent across the knob range — used to
confirm every mode sweeps a musically useful range before flashing hardware:

```sh
python tools/simulate.py          # everything
python tools/simulate.py gaits    # just the biomechanical gait check
```

It has caught real defects that compiled perfectly cleanly: Kuramoto coupling too weak
to ever synchronise, contagion saturating into a flat buzz, a gallop table byte-identical
to the walk, and modes topping out at rates that read as hiss rather than rhythm.

(`tools/simulate.cpp` compiles the *real* engine sources natively if you have a host C++
compiler; the Python model is the fallback.)

## Building

Raspberry Pi Pico SDK 2.2.0, Arm GCC 14.2, Ninja:

```sh
cmake -B build -G Ninja
cmake --build build
```

Produces `build/biomimicry.uf2`. Hold **BOOTSEL** while plugging in USB and drop it on
the mounted drive. On Windows, `cmake`/`ninja` live in `~/.pico-sdk/` and are not on the
default PATH — see [CLAUDE.md](CLAUDE.md) for the exact invocation.

---

## Credits

- **Music Thing Modular Workshop System Computer** — Tom Whitwell / Music Thing Modular.
  **ComputerCard** by **Chris Johnson** (MIT, header-only).
- **Raspberry Pi Pico SDK** / RP2040 — Raspberry Pi Ltd.
- The Kuramoto model — Yoshiki Kuramoto. Leaky integrate-and-fire and inhomogeneous
  Poisson processes are standard computational-neuroscience and point-process models.
- BioMimicry for the Workshop Computer — **Andy Jenkinson** (**uglifruit**), 2026, with
  **Claude Code** (Anthropic).

## Licence

The card's own source is Andy's. Vendored components keep their own licences
(`ComputerCard.h`).

*Step away from the grid and let the ecosystem run.*
