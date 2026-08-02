// voices.cpp — voice rendering at 48kHz.
//
// Each mode gets a timbre that suggests its creature without pretending to be a
// recording: a pitch-dropping sine for hooves, a formant-ish noise burst for
// honks, Karplus-Strong for ribbits, a high short ping for drips, and a swept
// noise swoosh for meteors.

#include "voices.h"
#include "samples_default.h"   // pulls in samples.h if baked, else stubs

namespace bio {

// Equal-power-ish pan positions for agents 0..3, spread L to R. Q15 gains.
static const int32_t kPanL[kNumAgents] = { 31000, 24000, 14000,  6000 };
static const int32_t kPanR[kNumAgents] = {  6000, 14000, 24000, 31000 };

// Per-mode envelope decay shifts (larger = longer tail at 48kHz).
static const uint8_t kDecay[kNumModes] = {
	9,   // Horses  — tight clop
	11,  // Geese   — honk with some length
	11,  // Frogs   — ribbit body
	8,   // Rain    — very short drip
	13   // Meteors — long swoosh
};

// Per-mode base pitch as a 48kHz phase increment (~ Hz * 89478).
static const uint32_t kBaseInc[kNumModes] = {
	 16000000u,  // Horses  ~180Hz thump
	 39000000u,  // Geese   ~440Hz honk
	 27000000u,  // Frogs   ~300Hz ribbit
	124000000u,  // Rain    ~1.4kHz drip
	  9000000u   // Meteors ~100Hz rumble under the noise
};

// Agent pitch offsets so four simultaneous hits don't sound like one. Q16.
static const int32_t kAgentPitch[kNumAgents] = { 65536, 55000, 78000, 46000 };

void VoiceBank::init(bool usePcm)
{
	usePcm_ = usePcm && kHaveSamples;
	for (int i = 0; i < kNumAgents; i++)
	{
		Voice &v = v_[i];
		v.env = 0; v.phase = 0; v.inc = 0; v.pitchEnv = 0;
		v.noiseRng = 0x1234567u + static_cast<uint32_t>(i) * 2654435761u;
		v.filt = 0; v.filt2 = 0;
		v.decayShift = 10; v.mode = 0;
		v.ksLen = 64; v.ksPos = 0;
		for (int k = 0; k < 128; k++) v.ks[k] = 0;
		v.pcm = nullptr; v.pcmLen = 0; v.pcmPos = 0; v.pcmInc = 65536;
	}
}

void VoiceBank::note(int i, Mode m, int32_t accent, int32_t variation)
{
	Voice &v = v_[i];
	int mi = static_cast<int>(m);

	v.mode = static_cast<uint8_t>(mi);
	v.env = (accent > kQ16One) ? kQ16One : accent;
	v.decayShift = kDecay[mi];
	v.pitchEnv = kQ16One;
	v.phase = 0;
	v.filt = 0;
	v.filt2 = 0;

	// Pitch: mode base, offset per agent, then nudged by the variation amount
	// so repeated hits are never identical.
	int32_t pitchScale = mul_q16(kAgentPitch[i], kQ16One + (variation >> 2) - 8192);
	if (pitchScale < 8192) pitchScale = 8192;
	v.inc = static_cast<uint32_t>(
		(static_cast<int64_t>(kBaseInc[mi]) * pitchScale) >> 16);

	if (usePcm_)
	{
		// PCM backend: pick this mode's sample, play at a per-agent rate.
		v.pcm    = kModeSample[mi];
		v.pcmLen = kModeSampleLen[mi];
		v.pcmPos = 0;
		v.pcmInc = static_cast<uint32_t>(pitchScale);
		return;
	}

	// Frogs use Karplus-Strong: excite the delay line with noise, then let the
	// averaging low-pass in render() turn it into a pitched pluck.
	if (m == Mode::Frogs)
	{
		uint32_t len = 48000u * 65536u / (v.inc / 1024u + 1u) / 1024u;
		if (len < 16) len = 16;
		if (len > 127) len = 127;
		v.ksLen = static_cast<uint8_t>(len);
		v.ksPos = 0;
		for (uint8_t k = 0; k < v.ksLen; k++)
			v.ks[k] = static_cast<int16_t>(rand_bipolar(v.noiseRng) >> 3);
	}
}

void VoiceBank::render(int active, int16_t &l, int16_t &r)
{
	int32_t accL = 0, accR = 0;

	for (int i = 0; i < kNumAgents; i++)
	{
		Voice &v = v_[i];
		int32_t s = 0;

		if (usePcm_)
		{
			if (v.pcm && v.pcmPos < (v.pcmLen << 16))
			{
				// 8-bit PCM, linear-interpolated so pitch-shifted playback
				// doesn't alias badly.
				uint32_t idx = v.pcmPos >> 16;
				int32_t  mu  = static_cast<int32_t>(v.pcmPos & 0xFFFF);
				int32_t  a   = v.pcm[idx];
				int32_t  b   = (idx + 1 < v.pcmLen) ? v.pcm[idx + 1] : 0;
				s = (a + (((b - a) * mu) >> 16)) << 4;   // 8-bit -> 12-bit
				v.pcmPos += v.pcmInc;
			}
		}
		else if (v.env > 0)
		{
			switch (static_cast<Mode>(v.mode))
			{
			case Mode::Horses:
			{
				// Sine with a fast downward pitch sweep — the classic thump.
				uint32_t inc = static_cast<uint32_t>(
					(static_cast<int64_t>(v.inc) * (kQ16One + v.pitchEnv * 3)) >> 16);
				v.phase += inc;
				s = fast_sin(v.phase) >> 4;
				v.pitchEnv = fast_exp_decay(v.pitchEnv, 6);
				break;
			}
			case Mode::Geese:
			{
				// Buzzy honk: a saw-ish tone through two poles of low-pass, so
				// it keeps a nasal formant edge rather than going pure.
				v.phase += v.inc;
				int32_t saw = static_cast<int32_t>(v.phase >> 20) - 2048;
				v.filt  += (saw - v.filt) >> 2;
				v.filt2 += (v.filt - v.filt2) >> 1;
				s = v.filt2;
				break;
			}
			case Mode::Frogs:
			{
				// Karplus-Strong: average adjacent taps (the low-pass) and feed
				// it back around the delay line.
				uint8_t nxt = static_cast<uint8_t>((v.ksPos + 1) % v.ksLen);
				int32_t avg = (v.ks[v.ksPos] + v.ks[nxt]) >> 1;
				avg -= avg >> 6;                  // damping
				v.ks[v.ksPos] = static_cast<int16_t>(avg);
				v.ksPos = nxt;
				s = avg;
				break;
			}
			case Mode::Rain:
			{
				// High short ping with a touch of noise at the attack.
				v.phase += v.inc;
				s = fast_sin(v.phase) >> 4;
				if (v.pitchEnv > kQ16One / 2)
					s += rand_bipolar(v.noiseRng) >> 5;
				v.pitchEnv = fast_exp_decay(v.pitchEnv, 4);
				break;
			}
			case Mode::Meteors:
			default:
			{
				// Filtered noise swoosh: the cutoff opens as the envelope
				// decays, giving the sound its passing-overhead sweep.
				int32_t n = rand_bipolar(v.noiseRng) >> 3;
				int32_t k = 1 + (v.env >> 14);        // 1..4
				v.filt  += (n - v.filt) >> k;
				v.filt2 += (v.filt - v.filt2) >> 3;
				v.phase += v.inc;
				s = v.filt2 + (fast_sin(v.phase) >> 6);
				break;
			}
			}

			// Apply and advance the amplitude envelope.
			s = mul_q16(s, v.env);
			v.env = fast_exp_decay(v.env, v.decayShift);
		}

		// Silence agents outside the current population.
		if (i >= active) continue;

		accL += mul_q15(s, kPanL[i]);
		accR += mul_q15(s, kPanR[i]);
	}

	l = clamp12(accL);
	r = clamp12(accR);
}

} // namespace bio
