// voices.h — the audio side: four voices rendered and placed at 48kHz.
//
// Two backends. The synthesized one is always available; the PCM one plays
// baked-in samples and is selected by holding the momentary switch at power-on
// (and only if samples were baked into the build).
//
// Both back-ends use ROUND ROBINS: every trigger picks a variant, and the
// variant means something per mode — which hoof, which bird, which species —
// rather than being decorative randomisation.

#pragma once
#include "biomimicry.h"

namespace bio {

/// Round-robin variants per mode. Eight, because with a flock of twelve geese
/// four recordings repeat audibly — and flash is not the constraint (the full
/// sample set is ~834KB of ~1.9MB free).
///
/// HORSES is the exception: its variants are the four HOOVES, not arbitrary
/// alternates, so it only ever uses the first four slots. See kHoofVariants.
constexpr int kNumVariants = 8;

/// Horses uses exactly one variant per hoof (LH, LF, RH, RF).
constexpr int kHoofVariants = 4;

/// One polyphonic slot, owned by one agent.
struct Voice
{
	// Envelope + tone state (synth backend)
	int32_t  env;        // Q16 amplitude envelope
	uint32_t phase;      // oscillator phase
	uint32_t phase2;     // second oscillator / formant
	uint32_t inc;        // phase increment
	uint32_t inc2;
	int32_t  pitchEnv;   // Q16 pitch-drop envelope (the "clop")
	uint32_t noiseRng;   // per-voice noise source
	int32_t  filt;       // one-pole filter memory for noise colouring
	int32_t  filt2;      // second pole, for the swoosh/honk shapes
	int32_t  bp;         // band-pass state, for resonant chirps
	uint8_t  decayShift; // envelope decay rate
	uint8_t  mode;       // which timbre to render
	uint8_t  variant;    // which round-robin variant this hit is

	// Karplus-Strong delay line (Frogs). 128 samples covers down to ~375Hz,
	// which is the bottom of the ribbit range.
	int16_t  ks[128];
	uint8_t  ksLen;
	uint8_t  ksPos;

	// Stereo placement for THIS hit, Q15 gains. Set at note-on so a mode can
	// place each trigger where its ecosystem would put it.
	int32_t  panL, panR;

	// PCM backend state.
	//
	// Position is split into WHOLE SAMPLES and a Q16 fraction rather than being
	// one Q16 number. A single uint32 Q16 position tops out at 2^32/65536 =
	// 65536 samples, i.e. 1.37 seconds — and meteors_4 (97493) and meteors_5
	// (118596) are longer than that, so the position wrapped back to zero before
	// it ever reached pcmLen. `idx < pcmLen` was then never false, the voice was
	// never freed, and the swoosh looped forever, straight through mode changes.
	const int8_t *pcm;
	uint32_t pcmLen;
	uint32_t pcmIdx;     // whole samples played
	uint32_t pcmFrac;    // Q16 fraction within the current sample
	uint32_t pcmInc;     // Q16 rate

	// Which agent this voice is currently sounding for, or -1 when free. Voices
	// are a POOL now rather than one-per-agent: a flock of twelve folded onto
	// four outputs retriggers the same agent long before its last sample has
	// finished, and with one voice each that cut the sound off mid-body.
	int8_t   agent;

	// Release ramp, Q16. A stolen or restarted voice fades over a few ms instead
	// of jumping to the new sample's first byte — that discontinuity was the
	// audible "truncation" on overlapping hits.
	int32_t  release;
};

/// Voices in the pool. Two per agent: the swarm modes fold twelve members onto
/// four outputs, so one agent is routinely retriggered while its previous hit is
/// still sounding — Geese can retrigger at ~56ms against samples of 137-222ms.
constexpr int kNumVoices = kNumAgents * 2;

/// Grains per agent in Drone boot. Must be at least as large as the heaviest
/// overlap any mode asks for, or the round-robin steals grains that are still
/// playing and the texture chops. Eight covers the densest mode with headroom;
/// four voices x eight grains is still only 32 concurrent readers, which is
/// cheap (a pointer, a position and a rate each).
constexpr int kNumGrains = 8;

/// One grain: a playing copy of an animal recording.
struct Grain
{
	const int8_t *pcm;
	uint32_t len;
	uint32_t pos;      // Q16 position within the sample
	uint32_t inc;      // Q16 playback rate
	int32_t  level;    // Q16 gain for this grain
	// Reciprocal of the window half-length, Q16. The triangular window needs
	// dist/half every sample, and len never changes once the grain launches --
	// so the divide is hoisted to launch and the render loop multiplies. The
	// M0+ has no divider, so this was a libgcc call per grain per sample:
	// 32 of them at 48kHz, which measured 17546 cycles against a 2604 budget.
	uint32_t invHalf;
	bool     active;
};

/// One Drone voice: several overlapping grains of the SAME animal recordings
/// the Rhythm card fires as one-shots.
///
/// The first version of this mode used saw oscillators whose pitch followed the
/// engine state. That was a mistake: most engines put a PHASE RAMP in state[],
/// so the pitch swept upward and snapped back, forever. It sounded like a broken
/// synth rather than an ecosystem, and none of the sample library was used.
struct DroneVoice
{
	Grain    g[kNumGrains];
	int32_t  level;      // Q16, slewed overall amplitude
	int32_t  density;    // Q16, how often new grains are launched
	int32_t  spread;     // Q16, pitch spread across grains
	int32_t  countdown;  // control ticks until the next grain
	uint8_t  next;       // round-robin grain slot
	uint32_t rng;
};

class VoiceBank
{
public:
	void init(bool usePcm);

	/// Trigger agent `i`'s voice with the timbre for `m`.
	/// `accent` (Q16) scales level; `variation` (Q16) nudges pitch per hit.
	/// `member` names the sub-member that fired where the mode has one (in
	/// Horses, which hoof); modes without one pass 0 and get a round robin.
	void note(int i, Mode m, int32_t accent, int32_t variation, uint8_t member);

	/// Render one sample of all voices, summed and panned into L/R.
	void render(int active, int16_t &l, int16_t &r);

	/// Drone boot: hand the engine's continuous state to the drone voices. Call
	/// once per control tick; `triggers` still marks events, which the drone
	/// uses as accents rather than as note-ons.
	void droneUpdate(Mode m, const int32_t *state, int32_t global,
	                 uint8_t triggers, int active, int32_t timbre);

	/// Drone boot: render one sample of the sustained voices.
	void droneRender(int active, int16_t &l, int16_t &r);

	/// Drone boot: how thick voice `i`'s texture currently is, Q16. Goes out on
	/// the CV outs so the control side describes the same thing you hear.
	int32_t droneDensity(int i) const { return d_[i].density; }

	bool usingPcm() const { return usePcm_; }

private:
	/// Pick the round-robin variant for this hit and place it in the stereo
	/// field, both according to the mode's own logic.
	void selectVariantAndPan(Voice &v, int agent, Mode m, uint8_t member);

	// Twice as many voices as agents, so an overlapping hit usually finds a free
	// one rather than cutting a live one short. ~332 bytes each against ~80KB
	// free, so the cost is RAM we have; the constraint was always CPU, and the
	// core-1 split left core 0 with headroom (Total 2209 of 2604).
	Voice v_[kNumVoices];
	DroneVoice d_[kNumAgents];
	uint8_t lastVariant_[kNumAgents];  // for no-immediate-repeat
	uint32_t rng_ = 0x9E3779B9u;

	// Which slots actually hold audio, resolved ONCE in init() rather than per
	// note-on. VariantCount() and PickUserVariant() each walk all eight slots
	// through the user header in XIP flash, and calling them per trigger cost
	// real cycles in the hot path for an answer that only changes when samples
	// are uploaded — which reboots the card anyway.
	uint8_t variantCount_[kNumModes] = {};
	uint8_t variantSlot_[kNumModes][kNumVariants] = {};

	// Resolved sample pointers, also cached at init. ResolveSample() reads the
	// user header through XIP — magic, version, size, offset — and note() called
	// it per trigger. Four flash reads while core 1 is contending for the same
	// bus is not cheap, and the answer only changes on upload, which reboots.
	const int8_t *sampleData_[kNumModes][kNumVariants] = {};
	uint32_t      sampleLen_[kNumModes][kNumVariants] = {};
	bool  usePcm_ = false;
};

} // namespace bio
