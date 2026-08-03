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

## Hardware: "samples playing far far too slow — like racing cars"

They were not playing at all. **Every boot was entering Drone mode**, whose old
Meteors root was a 40 Hz saw pair through a sweeping filter — which is exactly
what a passing vehicle sounds like.

`ComputerCard` derives the switch from `knobs[3]`, which comes off a ~60 Hz
smoothing filter **initialised to zero — and zero decodes as `Switch::Down`**.
For roughly the first 5 ms of every boot the card reports Down wherever the
switch actually is. The boot window latched on "Down seen at any point", so it
latched on *every* boot.

Two follow-ups from the bench settled it: *"both boot modes seem to be doing the
same"* pointed at the latch, and *"four sounds, rising in pitch"* was the synth
hoof pitches (153/202/164/216 Hz), proving the PCM path was never reached. The
fix is a single reading after settling — what WorkshopZX's `BootSelector` does,
and what is proven on this hardware. I had claimed to be following that pattern
and was not.

There is now a **boot splash** — Rhythm lights the left LED column, Drone the
right — because the whole diagnosis was slow for want of any way to tell the
modes apart.

Found while tracing it: the PCM end-of-sample test used `pcmLen << 16`, which
wraps above 65536 bytes. It was silently truncating the two longest meteor
swooshes to under half their length.

---

## Drone was useless, and rightly called out

The first Drone mapped engine `state[]` to oscillator pitch. Most engines put a
**phase ramp** in `state[]`, so the pitch swept upward and snapped back forever,
in half the modes — and it ignored the entire sample library in favour of saw
oscillators.

Rebuilt with **no oscillators**: Drone now granulates the same recordings, up to
eight overlapping grains per voice started at random points and played stretched.
The engines drive grain *density* and *spread*, never pitch. Grain periods were
sized against the real sample lengths so overlap lands at 2–4×; the first pass
asked for 16× overlap against four grain slots, which would have stolen grains
mid-playback and chopped.

---

## Hardware: cicadas galloping

Reported as clusters then silence, *"almost like galloping horses"* at CW. That
detail was the diagnosis — it said the clusters were **periodic**. Measured:
every 0.32 s, standard deviation 0.01 s. A metronome.

**My first guess was wrong.** I assumed the twelve insects shared one fatigue
time constant, gave each its own recovery rate and stamina, and it stayed
rhythmic in every configuration. Measuring phase clustering instead found the
real problem: phases converged from R=0.24 to **R=0.88 within five seconds** and
froze, because every insect ran off one shared rate law with no natural frequency
of its own. Detuning them dropped clustering to R=0.26.

That stopped the unison but the field still pulsed, for a reason no amount of
tuning fixes:

> One shared field, plus "everyone speeds up when it's loud and tires when they
> call", **is a relaxation oscillator**. Charge, discharge, repeat. It has exactly
> one period.

So the field became **four patches** that mostly hear themselves. Patches charge
and discharge independently — measured correlation **0.01** — and cluster spacing
went from a fixed 0.32 s to an irregular 0.4–8.6 s (cv 0.06 → 0.44–0.78).

*Note for anyone reading `simulate.py`:* `cicadas_swing` reports **one patch**,
not the mean. The mean is deliberately flat because independent patches cancel,
and that cancellation is the point.

---

## Real samples, and a way to change them

42 Pixabay recordings, already 48 kHz 16-bit mono, with levels spanning ~36 dB.
All of them fit (834 KB of 1943 KB free), so rather than discarding
three-quarters of the round robins the firmware went to **eight variants** —
except Horses, whose variants are the hooves.

Levels are matched by **RMS, not peak**. A peak is usually one transient: the
quietest goose had a body at RMS 0.004 against another at 0.059, yet both peaked
near 1.0, so peak-normalising would have left the quiet one still sounding quiet.
Corrections ran −15.5 dB to +20.6 dB; the library came out matched to 1.07×.

The web editor then removes the toolchain from the loop entirely: drag WAVs
into a browser, they are converted and loudness-matched there and streamed over
WebMIDI SysEx into a reserved 1 MB flash region, overriding the baked recordings
per slot. The card mutes during the write because flash writes halt execution —
honest about the constraint rather than glitching through it.

The firmware image now sits only ~95 KB below that region, and adding baked audio
would silently push it over, making flashing destroy uploads and uploads destroy
firmware. `tools/checksize.cmake` reads the real image end from the ELF and fails
the build if it ever reaches the boundary.

---

## A developer's correction: /32 does not buy you time

A Workshop Computer developer read the code and pointed out:

> *"Unless I misunderstand, calling the physics once per 32 samples doesn't help
> with performance — because the physics still needs to finish within that ~20us
> sample in which it is run?"*

He is right, and it invalidated a claim we had been repeating.

`controlTick()` is called **inline** from `ProcessSample()`, which runs inside
the DMA interrupt handler (`ComputerCard::AudioCallback`). So on the one sample
in 32 where the physics run, the whole engine still has to finish inside that
single **20.83 µs** slot. Dividing by `kCtrlDiv` lowers the *average* load. It
does not move the deadline.

The figure in this log — "~55 cycles/sample amortised against a 2604-cycle
budget" — was therefore measuring **throughput**, not the thing that decides
whether audio glitches. What matters is the **worst single sample**, and that had
never been measured. Static instruction counts could not settle it either: the
Geese tick is 1233 instructions but contains 243 branches, so the count is a very
loose upper bound rather than a real path.

The honest response was to measure rather than argue, so `profile.h` was added:
a SysTick-based cycle counter around the whole callback and each of its phases,
reporting the worst case on the LEDs (one LED per ~16% of budget, all six
flashing on an overrun). It compiles to nothing unless `-DBIO_PROFILE=ON`, and
this was verified by checking that the normal build is byte-for-byte identical to
the released firmware.

Geese is the mode to watch: its excitation spread is the only O(n²) path left, and
a full cascade is 12×11 = 132 inner iterations in one tick. Frogs was already made
O(n) by the mean-field rewrite.

His second point stands too: if the measurement shows headroom, the control rate
can go *up* for finer trigger timing. That is a one-line change to `kCtrlDiv` —
but worth making only once the headroom is a number rather than an assumption.

**Status: instrumented, not yet measured on hardware.**

---

## Standing notes

- **`tools/simulate.py` duplicates the C++ constants.** It will drift if
  `engines.cpp` changes without it. Update it or delete it — a model that lies is
  worse than none.
- **No host C++ compiler on this machine**, which is why the harness is a Python
  model rather than `tools/simulate.cpp` compiling the real sources.
- **CPU timing is now measurable** — see the section above. The old claim here
  ("~55 cycles/sample amortised") measured the wrong quantity and has been
  removed.
- **Drone mode has never been heard.** Its grain rates and stretch factors are
  calculated, not auditioned. Same for the audio-reactive thresholds, which are
  untested against real signal levels.
- **The USB path has never run on hardware.** TinyUSB enumeration, SysEx
  reassembly and flash writing are verified only in the sense that they compile
  and that the 7-bit codec round-trips against the firmware's decoder.
- **Listening beats simulation, and simulation beats reading the code.** Every
  fix in this log after the first release came from someone playing the card, and
  more than one of my confident first guesses was wrong until I measured
  something instead of reasoning about it.
