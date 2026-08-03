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

/// Variants per mode. Four covers one-per-hoof in Horses, which is the case
/// that most needs it; the flock modes cycle through them for variety.
constexpr int kNumVariants = 4;

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

	// PCM backend state
	const int8_t *pcm;
	uint32_t pcmLen;
	uint32_t pcmPos;     // Q16 fractional position
	uint32_t pcmInc;     // Q16 rate
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

	bool usingPcm() const { return usePcm_; }

private:
	/// Pick the round-robin variant for this hit and place it in the stereo
	/// field, both according to the mode's own logic.
	void selectVariantAndPan(Voice &v, int agent, Mode m, uint8_t member);

	Voice v_[kNumAgents];
	uint8_t lastVariant_[kNumAgents];  // for no-immediate-repeat
	uint32_t rng_ = 0x9E3779B9u;
	bool  usePcm_ = false;
};

} // namespace bio
