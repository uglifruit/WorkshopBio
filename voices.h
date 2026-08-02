// voices.h — the audio side: four panned voices rendered at 48kHz.
//
// Two backends. The synthesized one is always available; the PCM one plays
// baked-in samples and is selected by holding the momentary switch at power-on
// (and only if samples were baked into the build).

#pragma once
#include "biomimicry.h"

namespace bio {

/// One polyphonic slot, owned by one agent.
struct Voice
{
	// Envelope + tone state (synth backend)
	int32_t  env;        // Q16 amplitude envelope
	uint32_t phase;      // oscillator phase
	uint32_t inc;        // phase increment
	int32_t  pitchEnv;   // Q16 pitch-drop envelope (the "clop")
	uint32_t noiseRng;   // per-voice noise source
	int32_t  filt;       // one-pole filter memory for noise colouring
	int32_t  filt2;      // second pole, for the swoosh/honk shapes
	uint8_t  decayShift; // envelope decay rate
	uint8_t  mode;       // which timbre to render

	// Karplus-Strong delay line (Frogs). 128 samples covers down to ~375Hz,
	// which is the bottom of the ribbit range.
	int16_t  ks[128];
	uint8_t  ksLen;
	uint8_t  ksPos;

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
	void note(int i, Mode m, int32_t accent, int32_t variation);

	/// Render one sample of all voices, summed and panned into L/R.
	void render(int active, int16_t &l, int16_t &r);

	bool usingPcm() const { return usePcm_; }

private:
	Voice v_[kNumAgents];
	bool  usePcm_ = false;
};

} // namespace bio
