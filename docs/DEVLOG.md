# BioMimicry — development log

Why the card is built the way it is, and what listening to it changed. Written
as we went, newest last.

---

## 0.1.0 — the five engines

Built the scaffold into a working card: five physics engines driving four agents,
synthesized voices, an alt-boot PCM path, and the CV/gate routing the Workshop
Computer's real I/O allows.

Three hardware constraints shaped the whole design and are worth restating,
because they look like arbitrary choices otherwise:

- **There are two pulse outs, not four.** Agents 3 and 4 fire as calibrated 5 V
  blips on the CV outs, so every agent still gets a physical trigger.
- **There is one switch, not two.** A Down tap cycles the mode; the Up/Middle
  position selects routing. Holding Down at power-on picks the boot mode.
- **There are six LEDs.** With five modes the sixth showed activity; at six modes
  they share (see 0.2.0).

Physics run at a **1.5 kHz control rate** (every 32nd sample) while voices, gates
and CV run at the full 48 kHz. That split is what makes four agents of Kuramoto
coupling affordable, and 0.67 ms of timing granularity is far finer than the ear
resolves for triggers.

### What simulation caught before any hardware trip

`tools/simulate.py` models the engine maths and reports trigger rates. Four real
defects, none of which a compiler would have found:

| Defect | Symptom |
|---|---|
| Kuramoto coupling ~2% authority | Frogs could **never** synchronise — the model was decorative |
| Geese excitation saturating | Knob was a switch: silence, or a flat 16 Hz buzz |
| Rain topping out at 100 Hz | Hiss, not rhythm |
| Meteors topping out at 47 Hz | Same |

A useful lesson from this round: **flat trigger rates do not mean coupling is
broken.** Synchronised oscillators fire at the same rate, they just align in
time. Measuring the Kuramoto order parameter showed the coupling working when
the rate table suggested it wasn't.

---

## Reality check — the gaits were wrong

Asked directly whether the horse gaits were actually correct. They were not.
Three separate errors:

- **The gallop was byte-identical to the walk.** A `kGaitSteps` table meant to
  compress it into a burst-plus-suspension was `{4,4,4,4}` and silenced behind a
  `(void)` cast. The knob's entire top quadrant was a faster walk.
- **The walk wasn't a walk.** It fired both forelegs consecutively. A real walk
  is 4-beat *lateral* — same-side legs follow each other.
- **The canter had no suspension.** Evenly spaced 90° beats read as a waltz.

Underneath was a structural flaw: **four free-running clocks at 0.95–1.03×
cannot hold a gait.** A trot's diagonal pair separated within seconds. The drift
was the mode's poetry, but only the walk survived it.

Rebuilt around **one stride clock** with per-hoof landing offsets from real
equine footfall timing. Chaos jitters each hoof's *timing* rather than its rate,
so gaits stay locked however long they run. `python tools/simulate.py gaits`
asserts all four biomechanically.

Also this round: Rain got downstream splash coupling (it was statistically close
to Meteors — Poisson with a refractory period), Geese and Meteors went to
12-member swarms, and Pulse In 2 became an entrainment clock rather than a second
spook.

---

## 0.2.0 — Cicadas, round robins, stereo

Added a sixth ecosystem coupling on **amplitude**, where Frogs couple on phase
and Geese on events. It took two attempts, both caught in simulation:

1. With insects tiring only when *calling*, the swarm self-organised into an even
   spread and never surged — defeating the entire point of the mode. Ambient
   fatigue (being in a loud field is itself tiring) more than doubled the swing.
2. The knob still did nothing, because feedback speeds calls while fatigue is
   call-driven — they cancelled. Scaling **both halves** of the loop by the knob
   fixed it.

Round robins were wired for meaning rather than variety: in Horses the variant
**is the hoof**, hinds pitched lower and heavier than fores. Stereo placement
comes from the ecosystem — fixed for the horse, spread for flocks, random per hit
for rain and meteors.

Six modes filled all six LEDs, so mode and activity now share one light: the
active mode glows dim and flares to full on each trigger.

---

## Hardware: "the horse sounds like one horse"

Correct, and a real bug rather than a tuning problem — one I introduced when
consolidating to a single stride clock.

`HorsesEngine::tick()` **never read `c.population`**. Knob X only reached the mode
through the generic agent mask, which here removed *legs from one animal*.
Turning it up added nothing; turning it down made a lame horse.

Population is now a **herd**: one stride clock per horse, all four hooves intact,
with per-animal speed offsets so the animals slide in and out of step. That is
the phasing the old per-leg drift was reaching for, at the level where it doesn't
destroy the gaits. `EngineOut::member` carries which hoof landed so each animal
plays all four of its own.

---

## Hardware: trot and canter sound like a density drop

Also correct, with an acoustic explanation. Trot and canter were the **only**
gaits with mathematically coincident landings — and two identical clops fired on
the same sample don't sound like two hooves, they **sum into one louder clop**.
So a trot genuinely was half the density of a walk, not just perceptually.

Real diagonal pairs land 10–30 ms apart. The second foot of each pair now holds
back by a fixed delay (trot 12/18 ms, canter 15 ms), given in absolute time
rather than as a fraction of the stride — a real animal's flam doesn't stretch
with tempo. Walk and gallop already had four distinct landings and are untouched.

Frogs also went to a pond of twelve. That needed the Kuramoto sum rewritten
mean-field (O(n) instead of O(n²)) — **and the first attempt had the angle
identity's terms swapped and was 100% wrong.** Checking against a direct O(n²)
sum over 300 random ponds caught it; the corrected form agrees to 0.006%. Worth
recording because it compiled and ran silently.

---

## Alt-boot became Drone

Alt-boot was spending a whole boot mode on a sample toggle. Sample playback is
now a **build-time** choice (bake `samples/` or don't), which freed it for
**Drone**: the same six engines driving sustained tone instead of triggers.

Both Audio In jacks were also completely unused. Audio In 1 is now loudness (a
loud room agitates geese, silences cicadas), Audio In 2 transient-sensitive —
sudden movement alarms animals, steady noise doesn't.

---

## The real samples

42 recordings, already 48 kHz 16-bit mono. Two problems: more round robins than
the firmware's four, and levels spanning ~36 dB.

All 42 fit comfortably (834 KB of 1943 KB free), so rather than discarding
three-quarters of the variety the firmware went to **eight variants**. Horses
stayed at four — its variants are the hooves.

Levels are matched by **RMS, not peak**. A sample's peak is usually one
transient: the quietest goose had a body at RMS 0.004 against another at 0.059,
yet both peak near 1.0, so peak-normalising would have left the quiet one still
sounding quiet. Corrections ran from −15.5 dB to +20.6 dB; the library came out
matched to 1.07× with nothing clipping and peaks still spread 0.16–0.85, so
transients keep their character.

The baker points repeated variant slots at one copy in flash rather than storing
padding twice (saved 163 KB). Every baked slice was verified to byte-match its
source.

---

## Standing notes

- **`tools/simulate.py` duplicates the C++ constants.** It will drift if
  `engines.cpp` changes without it. Update it or delete it — a model that lies is
  worse than none.
- **No host C++ compiler on this machine**, which is why the harness is a Python
  model rather than `tools/simulate.cpp` compiling the real sources.
- CPU headroom is comfortable: worst engine ~55 cycles/sample amortised against a
  2604-cycle budget. Those are static upper bounds from instruction counts, not
  a measurement — the scope test on `ProcessSample()` duty cycle is still worth
  doing.
- **Drone mode has never been heard.** Its pitch ranges and drone roots are
  guesses. Same for the audio-reactive thresholds, which are untested against
  real signal levels.
