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

> **Status:** running on hardware. Six ecosystems, two boot modes, and a full
> sample library baked in. Rhythm mode has been played and iterated on; Drone
> mode and the audio-reactive inputs are built but not yet heard.
>
> Development history and the reasoning behind the design is in
> [docs/DEVLOG.md](docs/DEVLOG.md).

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
| **Switch Down** (momentary) | **Tap to cycle** the ecosystem. **Hold at power-on** to boot **Drone** mode instead of Rhythm. |
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
| **Audio In 1** | **Loudness** — the ecosystem hears the rest of your patch. A loud room agitates the geese into honking and shuts the cicadas up; the shy modes thin out as the patch gets busy and fill back in when it quietens. |
| **Audio In 2** | **Disturbance** — transient-sensitive rather than level-sensitive, because it is sudden movement that alarms an animal, not steady noise. A sharp attack counts as a Spook. |

Both audio inputs only act when something is patched in.

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

## Two instruments: Rhythm and Drone

Hold the momentary switch **Down at power-on** to boot **DRONE** instead of the normal
**RHYTHM** card. On power-up the LEDs announce which you got: Rhythm lights the left
column, Drone the right.

**Rhythm** fires the recordings as discrete one-shots — one animal per trigger.

**Drone** granulates the *same* recordings into continuous texture. Each voice keeps up
to eight overlapping grains running, started at random points inside the samples and
played back stretched, so a one-shot becomes a sustained layer. The engines drive grain
**density** and **spread** rather than pitch: Cicadas becomes a continuous field, Frogs a
whole pond at night, Rain a steady downpour, Horses a herd passing on a road. Knob Y
widens the pitch spread across grains, from a recognisable layer out to a smeared cloud.

There are no oscillators in Drone — it is your recordings, sustained.

## PCM samples

Sample playback is a **build-time** choice, not a boot mode: bake recordings into
`samples/` and they replace the synthesized voices in Rhythm boot. With no `samples/`
directory the firmware still builds and uses synthesis.

Each mode takes up to **eight round-robin recordings**, and the variants are not decoration.

Drop WAV files into `samples/incoming/` (or any folder) and run the importer:

```sh
python tools/importwav.py AnimalSFX
```

It accepts **any sample rate, mono or stereo, 8/16/24/32-bit or float**, and converts to
the 8-bit signed mono 48 kHz `.raw` the build bakes in — resampling, summing to mono and
trimming silence. Standard library only, no ffmpeg needed.

**It also matches loudness across the whole library.** Sample packs are typically all
over the place; the importer measures every source and scales each to a common RMS, then
soft-limits the result. RMS rather than peak, because a sample's peak is usually a single
transient — two recordings peak-normalised to the same ceiling can still sound nothing
alike. A real pack needed corrections from **-15 dB to +21 dB**, and came out matched to
1.07x with no clipping.

Name them `mode_variant.wav`, or use common animal names (`HORSE_1.wav`, `GOOSE_3.wav`,
`WHOOSH_2.wav`, `DRIP_5.wav`, `CICADA_8.wav`) — the importer maps those onto modes:

```
horses_1..4    (four: the hooves)
geese_1..8   frogs_1..8   rain_1..8   meteors_1..8   cicadas_1..8
```

**`horses_1..4` are LH, LF, RH, RF** — left hind, left fore, right hind, right fore. The
firmware asks for the hoof that actually landed, and hind hooves strike lower and heavier
than fores on a real animal, so putting them in the right slots is most of what makes a
gait sound like an animal. For the other modes the four are simply different individuals,
picked at random with no immediate repeat.

Fewer is fine: missing variants reuse whichever you supplied — and the baker points the
repeats at one copy in flash rather than storing it twice. A bare `horses.wav` with no
number covers every slot.

**What makes a good source:** a single isolated hit, trimmed tight to the transient (the
attack is what identifies the sound), and **dry** — the card has no reverb, so any
recorded ambience is baked in forever.

Rough lengths, and the flash they cost at four variants each:

| Mode | Length | Flash |
|---|---|---|
| Horses | ~180 ms | 35 KB |
| Geese | ~400 ms | 77 KB |
| Frogs | ~300 ms | 58 KB |
| Rain | ~120 ms | 23 KB |
| Meteors | ~700 ms | 134 KB |
| Cicadas | ~200 ms | 38 KB |

Total budget: code is ~107 KB of the 2 MB flash, leaving **~41 seconds** of 48 kHz mono
PCM — about 1.7 s per variant across all 24. The table above comes to ~365 KB, so there
is a lot of room; longer samples are fine, they just cost flash.

`python tools/gensamples.py` writes procedural placeholders in the same layout, so the
whole path works before you have a single recording.

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
- **Sample library** — animal and environment recordings from
  [**Pixabay**](https://pixabay.com), used under the Pixabay Content License.
- BioMimicry for the Workshop Computer — **Andy Jenkinson** (**uglifruit**), 2026, with
  **Claude Code** (Anthropic).

## Licence

The card's own source is Andy's. Vendored components keep their own licences
(`ComputerCard.h`).

*Step away from the grid and let the ecosystem run.*
